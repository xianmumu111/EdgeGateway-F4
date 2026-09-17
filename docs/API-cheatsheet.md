# EdgeGateway-F4 函数速查

> 2026-09-17 整理。每个函数写清：**干什么 → 参数是什么 → 返回值怎么用 → 坑在哪**。
> 用途是「讲得出」训练：合上代码，能对着这份表把整条链路复述出来，就算长在身上了。

---

## 0. 全景：三层架构与谁调谁

```
应用层   main.c（F407 主站）  /  main.c（F103 从站）
             │                        │
             ↓                        ↓
协议层   mb_master.c            modbus_rtu.c      ← 纯 C，零 HAL，可在 PC 上 gcc 测
（纯逻辑）  组帧/预判长度/解析      收一帧出一帧
             │                        │
             ↓                        ↓
port 层  mb_port_f4.c           mb_port_f1.c      ← 依赖 HAL，只能在板子上跑
（硬件）  USART2+PG8 方向控制     UART+DMA+IDLE
             │                        │
             └────────┬───────────────┘
                      ↓
               crc16.c / ringbuf.c   ← 纯 C 工具，PC 可测
```

| 文件 | 层 | 职责一句话 | PC 上能测吗 | 测试成绩 |
|---|---|---|---|---|
| `Drivers/crc16/crc16.c` | 工具 | 算 CRC-16/MODBUS | ✅ | ALL PASS |
| `Drivers/ringbuf/ringbuf.c` | 工具 | 无锁环形缓冲（SPSC） | ✅ | **99 项** + 变异 |
| `Drivers/modbus/modbus_rtu.c` | 从站协议 | 输入请求帧 → 输出响应帧 | ✅ | **14/14** + 4 变异 |
| `Drivers/modbus/mb_master.c` | 主站协议 | 组帧 / 预判长度 / 校验解析 | ✅ | **45/45** + 6 变异 |
| `Drivers/modbus/mb_port_f1.c` | 从站硬件 | UART+DMA+IDLE 收发 | ❌ 依赖 `stm32f1xx_hal.h` | — |
| `Drivers/modbus/mb_port_f4.c` | 主站硬件 | RS485 方向控制 + 收发 | ❌ 依赖 `stm32f4xx_hal.h` | — |
| `nodes/f103_slave/.../modbus_slave.c` | 从站（板载实际跑的） | 单文件自包含从站 | ❌ | 真机已跑通 |

> ⚠️ **两套从站实现并存**（见第 8 节）。真正烧进 F103 的是 `modbus_slave.c`，
> 不是 `modbus_rtu.c`——后者目前只活在 PC 测试里。

---

## 1. crc16 —— 唯一的工具函数

### `uint16_t crc16_modbus(const uint8_t *buf, uint16_t len)`

| 项 | 内容 |
|---|---|
| **作用** | 对 `buf` 前 `len` 字节算 CRC-16/MODBUS（poly `0xA001`，初值 `0xFFFF`，输入输出均反转） |
| **参数** | `buf` 数据首地址；`len` 参与计算的字节数 |
| **返回** | 16 位 CRC 的**逻辑值**（不是线上字节序） |
| **返回值怎么用** | ① **组帧**：`c = crc16_modbus(f, n); f[n] = c & 0xFF; f[n+1] = c >> 8;` <br> ② **校验**：把收到的末两字节拼成 `recv`，比较 `recv == crc16_modbus(f, len-2)` |
| **⚠️ 坑** | **线上是低字节在前**。`0x0BC4` 发出去是 `C4 0B`，不是 `0B C4`。这是全项目最高频的错点 |
| **⚠️ 坑** | `len` 是**参与计算的长度**，不含 CRC 自己那 2 字节。传总长会把垃圾值算进去 |

**golden vector（背下来，调试时一眼定位）**

```
11 03 00 6B 00 03      -> CRC = 0x7687 -> 线上 76 87   (Modbus 官方 spec)
11 03 06 AE 41 56 52 43 40 -> CRC = 0x49AD -> 线上 49 AD
01 03 00 00 00 02      -> CRC = 0x0BC4 -> 线上 C4 0B   (本项目请求帧)
01 03 04 00 64 00 C8   -> CRC = 0x7ABA -> 线上 BA 7A   (本项目响应帧，真机实测)
```

---

## 2. ringbuf —— 无锁环形缓冲（16 个函数）

**核心机制（面试必答）**：`head`/`tail` **单调递增、永不取模**，取下标时才 `& mask`。
所以 `used = head - tail` 恒成立，即使 32 位溢出回绕差值依然正确。
代价：`size` 必须是 2 的幂。SPSC 无锁的前提是 head 只由生产者写、tail 只由消费者写。

### 2.1 初始化

| 函数 | 返回值 | 说明 |
|---|---|---|
| `int rb_init(rb_t *rb, uint8_t *buf, uint32_t size)` | **1 成功 / 0 失败** | 失败 = 参数空 或 `size` 不是 2 的幂 |
| `RB_STATIC_DEFINE(name, size)` 宏 | — | 编译期静态定义，连 `rb_init` 都不用调，size 写错编译期就报错 |

### 2.2 状态查询（生产者消费者都能调）

| 函数 | 返回 | 含义 |
|---|---|---|
| `int rb_is_valid(const rb_t *rb)` | 1/0 | 控制块是否可用 |
| `uint32_t rb_capacity(const rb_t *rb)` | 字节数 | 总容量 == `size` |
| `uint32_t rb_used(const rb_t *rb)` | 字节数 | 已存 == `head - tail` |
| `uint32_t rb_free(const rb_t *rb)` | 字节数 | 还能写多少 |
| `int rb_is_empty(const rb_t *rb)` | 1/0 | |
| `int rb_is_full(const rb_t *rb)` | 1/0 | |
| `void rb_flush(rb_t *rb)` | — | 丢弃全部。**只能消费者调** |

### 2.3 生产者接口（只允许生产者调）

| 函数 | 返回 | 含义 / 坑 |
|---|---|---|
| `int rb_put(rb_t *rb, uint8_t c)` | **1 成功 / 0 满** | 满了**不会覆盖**，返回 0，丢弃与否由调用方决定 |
| `uint32_t rb_write(rb_t *rb, const uint8_t *src, uint32_t n)` | **实际写入数** | **部分写**：空间不足能写多少写多少，返回 0 = 一点没写 |
| `uint8_t *rb_write_ptr(rb_t *rb, uint32_t *room)` | 可写首地址 / NULL | 给 **DMA** 用。`*room` 是从该地址起**物理连续**的长度（不是总剩余！） |
| `uint32_t rb_commit(rb_t *rb, uint32_t n)` | 实际提交数 | 配 `rb_write_ptr` 用，DMA 收完后提交。n 自动截断到剩余空间 |

> `rb_write_ptr` + `rb_commit` 这对是 DMA 零拷贝的关键：
> ```c
> uint32_t room;
> uint8_t *p = rb_write_ptr(&rb, &room);
> HAL_UARTEx_ReceiveToIdle_DMA(&huart, p, room);   // DMA 直接写进环形缓冲
> // IDLE 回调里：rb_commit(&rb, Size);
> ```

### 2.4 消费者接口（只允许消费者调）

| 函数 | 返回 | 含义 / 坑 |
|---|---|---|
| `int rb_get(rb_t *rb, uint8_t *c)` | **1 成功 / 0 空** | |
| `uint32_t rb_read(rb_t *rb, uint8_t *dst, uint32_t n)` | **实际读出数** | 部分读，有多少读多少 |
| `uint32_t rb_peek(const rb_t *rb, uint8_t *dst, uint32_t n)` | 实际偷看数 | **不移动 tail**。Modbus 分帧全靠它：先 peek 前几字节判断长度够不够，够才 read |
| `int rb_peek_at(const rb_t *rb, uint32_t off, uint8_t *c)` | 1/0 | 看第 `off` 个字节（从 tail 起算，0 开始） |
| `uint32_t rb_skip(rb_t *rb, uint32_t n)` | 实际跳过数 | 解析出非法帧后整段丢掉，比 read 到临时数组再扔更省 |

---

## 3. modbus_rtu —— 从站协议层（纯 C）

### `mb_status_t mb_slave_handle(...)`

```c
mb_status_t mb_slave_handle(mb_slave_t *s,
                            const uint8_t *req, uint16_t req_len,
                            uint8_t *rsp, uint16_t rsp_max,
                            uint16_t *rsp_len);
```

| 参数 | 含义 |
|---|---|
| `s` | 从站对象（地址 + 寄存器表 + 统计），见下 |
| `req` / `req_len` | 一帧**完整**的请求（完整性由分帧层保证，本层不负责） |
| `rsp` / `rsp_max` | 响应输出缓冲区 |
| `rsp_len` | **输出**：实际生成的响应长度（**仅返回 `MB_OK` 时有效**） |

**返回值——这四个值的区分是这个模块最值钱的地方：**

| 返回 | 含义 | 调用方该做什么 |
|---|---|---|
| `MB_OK` | 已生成正常响应 | 把 `rsp` 前 `*rsp_len` 字节发出去 |
| `MB_NO_REPLY` | **不该回应**（地址不匹配 / 广播） | **什么都别发。** 被点名的从站此刻正在回，你开口就撞车 |
| `MB_BAD_FRAME` | 帧损坏（太短 / CRC 错） | **什么都别发**，计一次通信错误。CRC 错时连地址都不可信，回什么都可能是错的 |
| `MB_NO_SPACE` | `rsp` 给小了 | 这是**调用方的 bug**，不是通信问题 |

> ⚠️ `MB_NO_REPLY` 和 `MB_BAD_FRAME` 对调用方的动作**完全一样**（都别发），
> 但语义天差地别：一个是"正常的不说话"，一个是"收到了垃圾"。
> 面试时要能说清这两个的区别。

### `mb_slave_t` 结构体

| 字段 | 含义 |
|---|---|
| `addr` | 本从站地址 1~247 |
| `regs` / `regs_n` | 保持寄存器数组和个数（**外部提供，绝不 malloc**） |
| `stat_ok` / `stat_except` / `stat_bad_crc` / `stat_ignored` | 运行统计。加了可观测性，面试能说"我看得到总线健康状况" |

### 工具函数

| 函数 | 返回 | 说明 |
|---|---|---|
| `void mb_put_be16(uint8_t *p, uint16_t v)` | — | 按**大端**写 16 位。不能直接 `memcpy`，STM32 是小端 |
| `uint16_t mb_get_be16(const uint8_t *p)` | 16 位值 | 按**大端**读 16 位 |
| `const char *mb_status_str(mb_status_t st)` | 字符串 | 打印日志用 |

### 宏常量（背下来）

```
MB_FC_READ_HOLDING 0x03    MB_FC_WRITE_SINGLE 0x06
MB_EX_ILLEGAL_FUNC 0x01    非法功能码
MB_EX_ILLEGAL_ADDR 0x02    非法数据地址
MB_EX_ILLEGAL_VAL  0x03    非法数据值
MB_EX_SLAVE_FAILURE 0x04
MB_ADDR_BROADCAST 0        MB_ADDR_MIN 1   MB_ADDR_MAX 247
MB_READ_REGS_MAX 125       0x03 一次最多读 125 个
MB_WRITE_REGS_MAX 123      0x10 一次最多写 123 个
MB_FRAME_MAX 256           RTU 帧最大 256 字节
```

> **125 怎么来的**：响应帧最长 256 字节 = 5 字节头 + 2×125 = 255，刚好塞下。
> 126 就会让响应溢出。这是能背出来的面试答案。

---

## 4. mb_port_f1 —— 从站硬件层（F103，依赖 HAL）

| 函数 | 返回 | 作用 / 什么时候调 |
|---|---|---|
| `void mb_port_init(UART_HandleTypeDef *huart, uint8_t addr)` | — | 建环形缓冲、绑寄存器表、启动 DMA+IDLE 接收。**必须在 `MX_USARTx_UART_Init()` 之后调** |
| `void mb_port_rx_event(UART_HandleTypeDef *huart, uint16_t size)` | — | 在 `HAL_UARTEx_RxEventCallback()` 里调，告诉本层"收到了 size 个字节" |
| `void mb_port_poll(void)` | — | **主循环里不停调**。取一帧 → 交协议层 → 发响应。非阻塞，没数据立刻返回 |
| `void mb_port_reg_set(uint16_t idx, uint16_t val)` | — | 应用采样后写传感器值 |
| `uint16_t mb_port_reg_get(uint16_t idx)` | 寄存器值 | 应用读（比如显示在 OLED 上） |
| `const mb_slave_t *mb_port_slave(void)` | 从站对象指针 | 读 `stat_ok` / `stat_bad_crc` 等统计 |

### 寄存器映射表（主站轮询的数据点）

```
0 = 温度（0.1℃，250 = 25.0℃）      1 = 光照（ADC 0~4095）
2 = 倾角 X（int16，0.1°）           3 = 倾角 Y
4 = 按键累计次数                     5 = 运行秒数
6 = 报警标志（bit0 防拆 / bit1 温度超限）        总数 MB_REG_N = 8
```

> ⚠️ **F1 v1.8.7 的坑**：用 `HAL_UARTEx_ReceiveToIdle_DMA` 必须
> `__HAL_DMA_DISABLE_IT(..., DMA_IT_HT)` 关掉半满中断，否则一帧数据会收双份。

---

## 5. mb_master —— 主站协议层（纯 C，45/45）

### 5.1 组帧

```c
uint16_t mb_master_build_read(uint8_t *buf, uint16_t buf_max,
                              uint8_t slave, uint16_t start, uint16_t qty);
uint16_t mb_master_build_write_single(uint8_t *buf, uint16_t buf_max,
                                      uint8_t slave, uint16_t addr, uint16_t val);
```

| 项 | 内容 |
|---|---|
| **作用** | 把请求装进 `buf`，两个函数都产出**固定 8 字节**的帧 |
| **参数** | `buf_max` 缓冲区大小；`slave` 从站地址；`start`/`addr` 寄存器号；`qty` 个数；`val` 要写的值 |
| **返回** | **成功 = 8（帧长）；失败 = 0** |
| **为什么返回 uint16_t** | 用 0 表示失败的约定贯穿整个模块。调用方 `if (len == 0) 报错` 即可 |
| **⚠️ 坑** | `buf_max` 必须 ≥ 8，否则直接返回 0。**这是防御性检查，防的是越界写** |
| **⚠️ 坑** | `qty` 范围 1~125，越界返回 0 |
| **⚠️ 坑** | `append_crc(buf, 6)` 里的 **6 不是 8**——CRC 只覆盖前 6 字节，传 8 会把两个还没写的垃圾字节算进去 |

### 5.2 预判响应长度（主站分帧的核心）

```c
uint16_t mb_master_rsp_len(const uint8_t *req, uint16_t req_len,
                           const uint8_t *head, uint16_t head_len);
```

| 项 | 内容 |
|---|---|
| **作用** | 用**已收到的前 3 个字节**算出整帧该有多长 |
| **参数** | `req` 刚才发出去的请求（要取里面的 fc 和 qty）；`head` 已收到的头几个字节；`head_len` 已有几个 |
| **返回** | `head_len >= 3` → 总长度（≥5）；`head_len < 3` → **0**（信息不够，继续收） |
| **原理** | `head[1]` 最高位为 1 → 异常帧，**固定 5 字节**；否则 `0x03` = `5 + 2*qty`，`0x06` = 8 |
| **为什么必须有它** | 一次性收 9 字节的话，从站回 5 字节异常帧时你会**卡满整个超时周期**（几百 ms 到几秒）。这是它存在的全部意义 |

### 5.3 校验 + 解析

```c
mb_m_err_t mb_master_parse(const uint8_t *req, uint16_t req_len,
                           const uint8_t *rsp, uint16_t rsp_len,
                           uint16_t *out, uint16_t out_max, uint16_t *out_n,
                           uint8_t *exc);
```

| 参数 | 含义 |
|---|---|
| `req`/`req_len` | 我刚才发出去的请求（**用来比对**响应对不对得上） |
| `rsp`/`rsp_len` | 收到的响应 |
| `out`/`out_max` | 输出：解析出的寄存器值（**已转成本机字节序**） |
| `out_n` | 输出：个数。`0x03` → `qty`；`0x06` → 1 |
| `exc` | 输出：异常码（仅 `MB_M_EXCEPTION` 时有效） |

**内部检查顺序（面试必背）：**

```
1. 参数合法性     buf 空 / buf_max 太小 / qty 越界     -> MB_M_BAD_ARG
2. 长度 >= 5                                          -> MB_M_BAD_ARG
3. CRC            整帧算一遍                           -> MB_M_CRC_ERR
4. 地址           rsp[0] 必须 == req[0]                -> MB_M_ADDR_ERR
5. 功能码         正常 == req[1]；异常 == req[1]|0x80   -> MB_M_FC_ERR
6. 字节计数       rsp[2] 必须 == 2*qty                 -> MB_M_LEN_ERR
7. 帧长           3 + rsp[2] + 2 必须 == rsp_len        -> MB_M_LEN_ERR
8. 空间           out_max >= qty                       -> MB_M_NO_SPACE
```

> **为什么 CRC 排在地址前面**：地址字节本身也可能被打坏。
> 噪声把 `0x02` 打成 `0x01`，先看地址就会误以为"是叫我"然后开口回响应——
> 但真正的 0x02 也在回，两帧在线上叠加。**CRC 是唯一能证明"含地址在内整帧无误"的手段。**

### 5.4 `mb_m_err_t` 返回码（9 个）

| 值 | 名称 | 含义 | 谁产生 |
|---|---|---|---|
| 0 | `MB_M_OK` | 收到合法响应，数据已解析 | 协议层 |
| 1 | `MB_M_CRC_ERR` | CRC 失败 | 协议层 |
| 2 | `MB_M_ADDR_ERR` | 响应地址和请求对不上 | 协议层 |
| 3 | `MB_M_FC_ERR` | 功能码既非正常也非对应异常 | 协议层 |
| 4 | `MB_M_LEN_ERR` | 帧长不合法 | 协议层 |
| 5 | `MB_M_EXCEPTION` | 从站回了异常帧 —— **这是通信成功**，只是从站拒绝执行 | 协议层 |
| 6 | `MB_M_NO_SPACE` | 输出缓冲放不下 | 协议层 |
| 7 | `MB_M_TIMEOUT` | 超时没收到 | **port 层** |
| 8 | `MB_M_BAD_ARG` | 调用方参数不合法 | 协议层 |

> ⚠️ `MB_M_EXCEPTION` 千万别当通信故障处理。异常帧说明**链路完全正常**，
> 只是从站在说"你这个请求我做不了"（比如读了不存在的寄存器）。

### 5.5 内部 helper（static，外部看不到，但要懂）

| 函数 | 作用 |
|---|---|
| `static void put_be16(uint8_t *p, uint16_t v)` | 大端写 |
| `static uint16_t get_be16(const uint8_t *p)` | 大端读 |
| `static int crc_ok(const uint8_t *buf, uint16_t len)` | **返回 1 = CRC 正确**（布尔真值，不是错误码！） |
| `static uint16_t append_crc(uint8_t *buf, uint16_t len)` | 在 `buf[len]` 处追加 2 字节 CRC，返回 `len+2` |

> ⚠️ `crc_ok()` 返回 **1 = 好**，`MB_M_OK` 是 **0 = 好**——两种约定在同一个函数里撞车，
> 这是 C 里最高频的一类 bug。你 9/16 就栽在这里一次。

---

## 6. mb_port_f4 —— 主站硬件层（RS485，依赖 HAL）

### `mb_m_err_t mb_port_transfer(...)`

```c
mb_m_err_t mb_port_transfer(const uint8_t *req, uint16_t req_len,
                            uint8_t *rsp, uint16_t rsp_max, uint16_t *rsp_len,
                            uint32_t timeout_ms);
```

| 项 | 内容 |
|---|---|
| **作用** | 一次完整的 Modbus RTU 收发。**只管收发，不管解析** |
| **返回** | `MB_M_OK` / `MB_M_TIMEOUT` / `MB_M_NO_SPACE` / `MB_M_BAD_ARG` |

**七步流程：**

```
1) PG8 = 1                        发送使能（DE=1, /RE=1，驱动总线）
2) HAL_UART_Transmit(req, req_len)  阻塞发送
3) PG8 = 0                        接收使能（DE=0, /RE=0，接收器打开）
4) HAL_UART_Receive(rsp, 3, tmo)  ★ 只收 3 个字节
5) total = mb_master_rsp_len(...)  ★ 用这 3 字节推算总长；total==0 -> TIMEOUT
6) total > rsp_max -> NO_SPACE；否则收剩下 total-3 字节
7) *rsp_len = total; return MB_M_OK
```

| ⚠️ 坑 | 说明 |
|---|---|
| **第 4 步必须是 Receive** | 写成 Transmit 编译器完全不报错（参数类型都对），但会往总线再扔 3 字节垃圾，且 `rsp` 一个字节都没填。你 9/16 栽过一次 |
| **两段接收之间不能插耗时操作** | 尤其不能 `uprintf`。9600bps 下 1 字节约 1.04ms，收完第 3 字节时第 4 字节已在路上。打印几毫秒 → 后面字节永久丢失 → **偶发 CRC 错**，最难查 |
| **阻塞发送下 DE 拉早是安全的** | `HAL_UART_Transmit` 内部会等 TC 标志。这个坑只在 `Transmit_IT/DMA` 下才成立（要在 `TxCpltCallback` 里拉） |
| **PG8 命名** | 原理图叫 `RS485_RE`（RE 与 DE 并联）。高 = 发送，低 = 接收 |
| **HAL_UART_Receive 的 timeout** | 是**整段总超时**，不是字节间超时。给 200ms 绰绰有余 |

---

## 7. main.c（F407 主站）的应用层

### `static void uprintf(const char *fmt, ...)`

| 项 | 内容 |
|---|---|
| **作用** | 串口打印。`<stdarg.h>` + `vsnprintf` + `HAL_UART_Transmit(&huart1, ...)` |
| **为什么不用 printf** | 没勾 MicroLIB 时 `printf` 走 semihosting **直接卡死**；勾了 MicroLIB 又和 retarget 写法冲突，AC5/AC6 还不一样 |
| **你加的 clamp** | `n > sizeof(buf)` 时把 `n` 截断——`vsnprintf` 返回的是"想写的长度"不是"实际写的长度"。这是骨架里没要求的，你自己想到的边界情况 |

### 主循环三步

```c
req_len = mb_master_build_read(g_req, sizeof g_req, 0x01, 0x0000, 2);  // 1) 组帧
e = mb_port_transfer(g_req, req_len, g_rsp, sizeof g_rsp, &rsp_len, 200); // 2) 收发
e = mb_master_parse(g_req, req_len, g_rsp, rsp_len, g_out, N, &out_n, &exc); // 3) 解析
```

---

## 8. ⚠️ 两套从站实现并存（重要事实）

| | `Drivers/modbus/modbus_rtu.c` | `nodes/f103_slave/Core/Src/modbus_slave.c` |
|---|---|---|
| 架构 | 分层（协议层 + port 层分离） | 单文件自包含 |
| 测试 | ✅ PC 上 **14/14** + 4 变异 | ✅ PC 仿真台 **18/18** + 6 变异 |
| 进了 Keil 工程吗 | ❌ **没有** | ✅ **就是这个在板子上跑** |
| 分帧方式 | 交给 `mb_port_f1.c` 的 DMA+IDLE | 自己用 `HAL_GetTick()` 判 t3.5 静默 |

**这不是 bug，是演进痕迹**：先写了单文件版跑通真机，后来才重构出分层版。
但**现在的状态有风险**——`modbus_rtu.c` 没人用，将来会腐化。

**建议（你自己决定）**：把 F103 工程的 `modbus_slave.c` 换成 `modbus_rtu.c` + `mb_port_f1.c`，
或者干脆删掉 `modbus_rtu.c`。**两套都在 = 三个月后你分不清哪个是真的。**

### `modbus_slave.c`（板载实际跑的）函数表

| 函数 | 返回 | 作用 |
|---|---|---|
| `void modbus_init(void)` | — | 初始化，主循环前调一次 |
| `void modbus_rx_byte(uint8_t byte)` | — | **在 UART 中断里调**，喂一个字节进来，同时刷新 `last_rx_tick` |
| `void modbus_poll(void)` | — | **主循环里不停调**。判 t3.5 静默 → 够一帧才解析 → 回响应 |
| `void modbus_tick(void)` | — | 每秒调一次，运行秒数 +1 |
| `uint16_t modbus_crc16(const uint8_t *buf, uint16_t len)` | CRC | 本文件自带的 CRC 实现 |
| `int modbus_crc_ok(const uint8_t *f, uint16_t len)` | 1/0 | 校验 |
| `void modbus_err(uint8_t fc, uint8_t code)` | — | 组异常帧并发出 |
| `void modbus_handle(uint8_t *req, uint16_t len)` | — | 处理 0x03 / 0x06 |

> ⚠️ 已知待修：`MODBUS_T35_MS` 在波特率 >19200 分支写死 2ms，
> 规范要求 **1750µs**（高波特率下 3.5 字符太短，收发器响应不过来）。

---

## 9. 完整运行过程：一帧 0x03 的一生

以真机实测的那帧为例（`reg[0]=100, reg[1]=200`）：

```
【F407 主站】
 1. main 循环                    while(1)
 2. mb_master_build_read(g_req, ..., 0x01, 0x0000, 2)
        buf[0]=0x01  buf[1]=0x03
        put_be16(&buf[2], 0x0000)  -> 00 00
        put_be16(&buf[4], 0x0002)  -> 00 02
        append_crc(buf, 6)         -> CRC=0x0BC4 -> buf[6]=C4 buf[7]=0B
        返回 8
 3. mb_port_transfer(...)
        PG8=1                                  发送使能
        HAL_UART_Transmit(USART2, req, 8, 100) 线上: 01 03 00 00 00 02 C4 0B
        PG8=0                                  接收使能
        HAL_UART_Receive(USART2, rsp, 3, 200)  收到: 01 03 04
        mb_master_rsp_len(req, 8, rsp, 3)      fc=0x03 非异常 -> 5+2*2 = 9
        HAL_UART_Receive(USART2, rsp+3, 6, 200) 收到: 00 64 00 C8 BA 7A
        *rsp_len = 9
        返回 MB_M_OK
 4. mb_master_parse(...)
        CRC: crc16_modbus(rsp前7字节) = 0x7ABA == rsp[7,8] 拼的 0x7ABA  ✓
        地址: rsp[0]=0x01 == req[0]=0x01                                 ✓
        功能码: rsp[1]=0x03 == req[1]=0x03  (正常响应分支)                ✓
        字节计数: rsp[2]=0x04 == 2*2                                     ✓
        帧长: 3+4+2 = 9 == rsp_len                                       ✓
        取数: out[0]=get_be16(&rsp[3])=0x0064=100
             out[1]=get_be16(&rsp[5])=0x00C8=200
        返回 MB_M_OK, out_n=2
 5. uprintf("reg[%u] = %u")  ->  reg[0] = 100 / reg[1] = 200

【F103 从站】（同一时刻在另一侧）
 A. USART1 中断: modbus_rx_byte(byte)  逐字节存进 rx_buf，刷新 last_rx_tick
 B. modbus_poll():
        rx_len>0 且 (HAL_GetTick()-last_rx_tick) >= MODBUS_T35_MS(4ms)  -> 够一帧
        modbus_crc_ok()                          CRC 错 -> 沉默丢弃
        addr == 0x01 ?                           不是 -> 沉默（别人在说话）
        modbus_handle(): 0x03 -> 校验 qty/越界 -> 组响应帧 -> 发出
 线上: 01 03 04 00 64 00 C8 BA 7A
```

**两边的时间关系**（9600bps，1 字节 ≈ 1.04ms）：

```
主站: |-- 发 8 字节 8.3ms --|-- 等从站处理 --|-- 收 9 字节 9.4ms --|
从站:                       |-- t3.5=4ms --|-- 处理 --|-- 发 9 字节 --|
```

---

## 10. 自检：合上代码能答出这些吗

| # | 问题 | 答不出说明 |
|---|---|---|
| 1 | 为什么 CRC 必须第一个查，不能先查地址？ | 没理解"帧坏了后面每个字段都不可信" |
| 2 | 为什么先收 3 字节，不一次收 9 字节？ | 没理解异常帧只有 5 字节 |
| 3 | 为什么地址不匹配时**沉默**，而不是回异常帧？ | 没理解半双工会撞车、Modbus 无总线仲裁 |
| 4 | 为什么 RTU 用 3.5 字符分帧，ASCII 却能用字符定界？ | 没理解二进制帧里任意字节都可能出现 |
| 5 | 为什么协议层一个 HAL 函数都不能调？ | 没理解分层的收益（能在 PC 上测） |
| 6 | `crc_ok()` 返回 1 是好还是坏？`MB_M_OK` 呢？ | 两种约定撞车，栽过一次 |
| 7 | `append_crc(buf, 6)` 为什么是 6 不是 8？ | CRC 覆盖范围是参与运算的长度 |
| 8 | 125 这个上限是怎么算出来的？ | 256 - 5 = 251，÷2 = 125 |
| 9 | `rb_peek` 和 `rb_read` 的差别？为什么必须有 peek？ | 分帧要先看长度再决定读不读 |
| 10 | `MB_M_EXCEPTION` 算通信失败吗？ | 不算，是通信成功 + 从站拒绝执行 |

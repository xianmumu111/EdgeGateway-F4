# EdgeGateway-F4 代码使用手册

> 最后更新：2026-09-19　｜　对应状态：CP0 完成（FreeRTOS 三任务跑通 + 38400bps 五步轮询全通）
>
> **这份文档回答一个问题**：半年后你（或者面试官、或者 clone 你仓库的人）拿到这台电脑，
> 怎么把东西跑起来、每个文件是什么、改哪里会炸。不想解释了就把这篇甩过去。

---

## 目录

1. [一分钟速查](#1-一分钟速查)
2. [仓库结构](#2-仓库结构)
3. [从零恢复一台新电脑](#3-从零恢复一台新电脑)
4. [两个节点分别是什么](#4-两个节点分别是什么)
5. [硬件接线与跳线](#5-硬件接线与跳线)
6. [编译 / 烧录 / 单元测试](#6-编译--烧录--单元测试)
7. [模块 API 速查](#7-模块-api-速查)
8. [F407 网关的运行时架构](#8-f407-网关的运行时架构)
9. [打印与调试](#9-打印与调试)
10. [★ CubeMX Generate 之后的必做清单](#10--cubemx-generate-之后的必做清单)
11. [Git 规矩](#11-git-规矩)
12. [症状对照表（排错先看这里）](#12-症状对照表排错先看这里)

---

## 1. 一分钟速查

```bash
# 本机 Bash 第一条必须写，否则 ls/gcc/git 全 not found
export PATH="/usr/bin:/bin:$PATH"

# 命令行编译（不开 Keil GUI，看有没有编译错）
"D:\MDK5\UV4\UV4.exe" -j0 -b "D:\GitHub\EdgeGateway-F4\nodes\f407_gateway\MDK-ARM\f407_gateway.uvprojx" -o build.log
#   然后看 build.log 里的 "N Error(s)"

# 单元测试（PC 上跑，不用烧板）
cd D:\GitHub\EdgeGateway-F4\test && build.bat

# 推送（必须带这两个参数，本机代理有特殊脾气）
cd D:\GitHub\EdgeGateway-F4
git -c http.sslVerify=false -c http.sslBackend=schannel push origin main
#   报 502 / ssl_read eof 是代理随机掐连接，原样重试一两次
#   验证是否推成功别用 ls-remote（也会被掐），用：
curl -k -s https://api.github.com/repos/xianmumu111/EdgeGateway-F4/commits/main | grep -m1 '"sha"'
```

| 关键路径 | 是什么 |
|---|---|
| Keil 主程序 | `D:\MDK5\UV4\UV4.exe`（**不是** `D:\Keil_v5`，那个是空壳） |
| CubeMX | `D:\STM32_MX` |
| F4 器件包 | `D:\MDK5\work\Keil\STM32F4xx_DFP\2.15.0\` |
| Flash 算法 | `STM32F4xx_1024.FLM`（F407ZGT6 = 1MB） |
| 项目仓库 | `D:\GitHub\EdgeGateway-F4`（**唯一开发路径**） |

---

## 2. 仓库结构

```
EdgeGateway-F4/
├── DEVLOG.md                   每天三行日志（今天/卡点/明天），【卡点】必须本人写
├── README.md
├── docs/
│   ├── API-cheatsheet.md       所有函数的作用/参数/返回值速查 + 10 道自检题
│   ├── 09-18-freertos-multitask.md   FreeRTOS 三任务作战单
│   ├── 09-19-baudrate-stress.md      波特率压力测试方案
│   └── 09-19-dwt-cyccnt.md           DWT 精确计时作战单 ← 今天
│
├── Drivers/                    ★ 全部是可移植的纯代码，进 git
│   ├── crc16/  crc16.c/.h       CRC-16/MODBUS（他手写，gcc ALL PASS）
│   ├── ringbuf/ringbuf.c/.h     Linux kfifo 风格环形缓冲（99 单测 + 变异测试）
│   └── modbus/
│       ├── modbus_rtu.c/.h      从站协议层（PC 可测的那套）
│       ├── mb_master.c/.h       主站协议层 ★ 45 项单测 + 6 项变异全捕获
│       ├── mb_port_f1.c/.h      从站 port 层（依赖 HAL）
│       └── mb_port_f4.c/.h      主站 port 层（依赖 HAL）
│
├── nodes/                      每个 MCU 一个 CubeMX 工程
│   ├── f103_slave/             Modbus 从站（STM32F103C8T6）
│   │   ├── Core/Src/modbus_slave.c    从站主逻辑
│   │   ├── Core/Src/main.c            裸机 while(1)：modbus_poll() + modbus_tick()
│   │   ├── F103_ModbusSlave.ioc
│   │   ├── Drivers/                   ← 不入 git，Generate 恢复
│   │   ├── MDK-ARM/F103_ModbusSlave/  ← 不入 git，编译产物
│   │   └── test/                      PC 仿真台，18/18 通过 + 6/6 变异
│   │
│   └── f407_gateway/           ★ 主线工程（STM32F407ZGT6）
│       ├── Core/Src/main.c            外设初始化，while(1) 已空（交给 RTOS）
│       ├── Core/Src/freertos.c        ★★ 三任务全在这一个文件里
│       ├── Core/Inc/FreeRTOSConfig.h
│       ├── Core/Inc/dwt_us.h          今天新增：DWT 微秒计时（header-only）
│       ├── PINMAP.md                  原理图对照引脚表（手册错 5 处，以这个为准）
│       └── f407_gateway.ioc
│
└── test/                       PC 单元测试，双击 build.bat 全跑
    ├── test_crc.c     test_ringbuf.c
    ├── test_master.c  test_modbus.c
    └── build.bat
```

### ★ 一个容易混淆的点：有两个 `Drivers/`

| 路径 | 入 git？ | 内容 |
|---|---|---|
| **根目录 `/Drivers/`** | ✅ 入 | **你自己写的可移植代码**（crc16 / ringbuf / modbus） |
| `nodes/*/Drivers/` | ❌ 忽略 | CubeMX 生成的 **HAL 库**（F4 那份 57MB） |

`.gitignore` 里明确写着 `/nodes/f103_slave/Drivers/` 和 `/nodes/f407_gateway/Drivers/`，
还有 `/nodes/f407_gateway/Middlewares/`（FreeRTOS 内核源码，12MB）。
**仓库总共 ~300KB，clone 下来必须 Generate 才能编译。**

---

## 3. 从零恢复一台新电脑

```bash
# 1. clone（只有 ~300KB）
git clone https://github.com/xianmumu111/EdgeGateway-F4.git

# 2. 打开 CubeMX → File → Load Project
#      nodes/f407_gateway/f407_gateway.ioc      （或 F103 的）
#    右上角 GENERATE CODE
#    ⚠️ 第一次会联网下载 HAL 库，耐心等

# 3. 打开 Keil 工程
#      nodes/f407_gateway/MDK-ARM/f407_gateway.uvprojx

# 4. ★ 补回被 Generate 冲掉的东西（见第 10 节）
#    文件组 + IncludePath + 中间的 Some.c 之类

# 5. 命令行编译验证
```

> **为什么 README 里必须写这段**：STM32 工程原始体积 90MB（HAL 62MB + 编译产物 22MB）。
> 精简到 300KB 的代价就是 clone 下来不能直接编译，必须走上面 5 步。这是刻意的取舍。

---

## 4. 两个节点分别是什么

| | nodes/f103_slave | nodes/f407_gateway |
|---|---|---|
| MCU | STM32F103C8T6（72MHz） | STM32F407ZGT6（168MHz） |
| 角色 | **Modbus RTU 从站** | **Modbus RTU 主站 + 网关** |
| 是否上 RTOS | 否，裸机 `while(1)` | ✅ FreeRTOS CMSIS_V2，三任务 |
| 串口 | USART1 **中断逐字节**接收 | USART2 **DMA + IDLE + ringbuf** |
| 寄存器表 | `regs[8] = {100, 200, 0, 0, 0, 0, 0, 0}` | — |
| 波特率 | **38400**（`#define MODBUS_BAUD`） | **38400** |

### F103 从站做什么

- `regs[2]` 每次 `modbus_tick()` 自增 → 用来观察数据在动
- `regs[3] = HAL_GetTick()/1000` → 上电秒数
- 支持 **0x03 读保持寄存器** / **0x06 写单个寄存器**
- 越界回**异常帧 0x02**；CRC 错或地址不符**沉默**（Modbus 规范要求）

### F407 主站做什么

五步 round-robin 轮询，每步间隔 500ms：

| 步 | 干什么 | 期望结果 |
|---|---|---|
| S0 | 读 slave=01 reg0 开始 2 个 | OK，reg=100 / 200 |
| S1 | 写 slave=01 reg4 = 0x037F | OK，回显 |
| S2 | 读回 reg4 | OK，**895**（0x037F）← 证明真的写进去了 |
| S3 | 读 slave=01 reg200 | **异常帧 0x02** |
| S4 | 读 slave=02（不存在） | **超时 202ms** |

**为什么必须有 S2**：S1 的回显只证明"从站收到了我的请求"，不证明"它真的写进了寄存器"。
read-back verification = 写完必须读回来比对。这是工业现场的标准做法。

---

## 5. 硬件接线与跳线

### 5.1 两块板之间（当前：TTL 直连）

```
F407 探索者                      F103 最小系统板
PA2 (USART2_TX)  ─────────────>  PA10 (USART1_RX)
PA3 (USART2_RX)  <─────────────  PA9  (USART1_TX)
GND              ──────────────  GND          ★ 共地不能少
```

⚠️ 现在是 **TTL 直连**，板载 SP3485 被旁路，所以 PG8 方向控制写反了也能通。
**真 RS485 差分模块到了之后，PG8 才有意义**（发=1，收=0）。

### 5.2 F407 探索者板上的规矩

| 项目 | 值 |
|---|---|
| 晶振 | **8 MHz**（CubeMX 默认填 25MHz，错的） |
| 调试口 | **SYS 必须 Serial Wire** —— 选 JTAG(5pins) 会占掉 PB3/PB4，W25Q128 直接废 |
| P6 跳线 | USART1 ↔ CH340 → 电脑（printf 从这里出来） |
| P9 拨码 | 拨到 **485** 侧才能用板载 SP3485 |
| P10 拨码 | USART3 ↔ ATK-MODULE（以后接 ESP8266） |
| LED | PF9 红 / PF10 绿，**低电平点亮** |
| 蜂鸣器 | PF8 |
| RS485 方向 | PG8（RS485_RE），DE/RE 并联 |

> ⚠️ **RS485 和以太网不能同时用**（PA2 冲突）。这也是本项目走 WiFi 上云的硬件理由。
> 完整引脚表看 `nodes/f407_gateway/PINMAP.md` —— **是照原理图核对的，手册有 5 处错，别信手册。**

### 5.3 串口助手参数

```
波特率 115200 / 8 / N / 1        ← USART1 调试口（波特率跟随 CubeMX 配置）
波特率 38400  / 8 / N / 1        ← USART2 Modbus
```

---

## 6. 编译 / 烧录 / 单元测试

### 6.1 命令行编译（改完代码必跑）

```bash
"D:\MDK5\UV4\UV4.exe" -j0 -b "<工程路径>" -o build.log
# 读 build.log 里 "N Error(s)" 判断
```

比肉眼检查靠谱得多，而且不用打开 GUI。

### 6.2 PC 单元测试

```bash
cd D:\GitHub\EdgeGateway-F4\test
build.bat
```

每个 `test_*.c` 有自己的 `main()`，脚本会逐个编译运行。
目前覆盖：crc16 / ringbuf(99 项) / mb_master(45 项) / modbus_rtu。

**写完测试必须做变异测试**：改坏实现，确认测试真的会红。
不做这一步，绿油油的测试通过等于什么都没证明。工具见 `nodes/f103_slave/test/mutate.py`。

### 6.3 三条开发纪律

1. 每天 DEVLOG 三行（今天/卡点/明天）—— **卡点必须自己写**，写了才算今天的
2. 每天一个 commit
3. **3 小时连续 > 3 个 1 小时**（嵌入式出成果靠连续调试时间，碎片时间只够看文档）

---

## 7. 模块 API 速查

### 7.1 crc16

```c
uint16_t crc16_modbus(const uint8_t *buf, uint16_t len);
```
- 初值 0xFFFF，多项式 0xA001，**低字节先出**
- 加到帧尾时：`buf[n] = crc & 0xFF; buf[n+1] = crc >> 8;`

### 7.2 ringbuf（Linux kfifo）

```c
int      rb_init(rb_t *rb, uint8_t *buf, uint32_t size);
RB_STATIC_DEFINE(name, size_)              /* 静态分配一块(size 必须是 2 的幂) */

uint32_t rb_used(const rb_t *rb);          /* == head - tail */
uint32_t rb_free(const rb_t *rb);
int      rb_put (rb_t *rb, uint8_t c);
uint32_t rb_write(rb_t *rb, const uint8_t *src, uint32_t n);
uint32_t rb_read (rb_t *rb, uint8_t *dst, uint32_t n);
uint32_t rb_peek (const rb_t *rb, uint8_t *dst, uint32_t n);   /* 读但不移 tail */
int      rb_peek_at(const rb_t *rb, uint32_t off, uint8_t *c);
uint32_t rb_skip(rb_t *rb, uint32_t n);
void     rb_flush(rb_t *rb);               /* 只能消费者调用 */
```

**三个必须讲得出的点**：
1. head/tail **单调递增永不取模**，下标用 `& mask` 得到；`used = head - tail`
2. size 个格子**装得下 size 个字节**（不是 size-1）—— 靠 used 判断而不是比较指针
3. 32 位溢出回绕时 `head - tail` **依然正确**

> SPSC 无锁的前提：head 只有生产者写，tail 只有消费者写。
> ⚠️ **volatile 不够**。有 DMA / D-Cache 时必须用真正的屏障。

### 7.3 mb_master（主站协议层，纯 C 无 HAL）

```c
uint16_t mb_master_build_read        (uint8_t *buf, uint16_t buf_max,
                                      uint8_t slave, uint16_t start, uint16_t qty);
uint16_t mb_master_build_write_single(uint8_t *buf, uint16_t buf_max,
                                      uint8_t slave, uint16_t addr,  uint16_t val);
uint16_t mb_master_rsp_len           (const uint8_t *req, uint16_t req_len,
                                      const uint8_t *rsp3);
mb_m_err_t mb_master_parse           (const uint8_t *req, uint16_t req_len,
                                      const uint8_t *rsp, uint16_t rsp_len,
                                      uint16_t *out, uint16_t out_max,
                                      uint16_t *out_n, uint8_t *exc);
const char *mb_m_err_str(mb_m_err_t e);
```

返回 0 = 组帧失败；`mb_master_rsp_len` 返回需要接收的总字节数。

**`mb_m_err_t` 九种**（注意 **0 = OK**）：

```
MB_M_OK        = 0   ← 注意这里是 0，和下面 rb_*/crc_ok 的「1=成功」相反
MB_M_CRC_ERR   = 1
MB_M_ADDR_ERR  = 2
MB_M_FC_ERR    = 3
MB_M_LEN_ERR   = 4
MB_M_EXCEPTION = 5   ← 异常帧是**合法响应**，不是错误
MB_M_NO_SPACE  = 6
MB_M_TIMEOUT   = 7   ← 由 port 层产生
MB_M_BAD_ARG   = 8
```

### 7.4 mb_port_f4（主站 port 层，依赖 HAL）

```c
void mb_port_init(void);
mb_m_err_t mb_port_transfer(const uint8_t *req,  uint16_t req_len,
                            uint8_t       *rsp,  uint16_t rsp_max,
                            uint16_t      *rsp_len, uint32_t timeout_ms);
```

内部流程（他的实现）：

```
RS485_RE(PG8) 拉高 → HAL_UART_Transmit 发 → 拉低 → 从 ringbuf 取 3 字节
→ mb_master_rsp_len 算总长 → 取剩下的 → 填 rsp_len
```

### 7.5 分层原则

```
应用层    main.c / freertos.c      业务编排
协议层    mb_master.c              纯 C，可以在 PC 上测 ← 一个 HAL 函数都不能调
port 层   mb_port_f4.c             依赖 HAL，换 MCU 只换这一层
工具层    crc16.c / ringbuf.c
```

**为什么协议层不许调 HAL**：能 PC 单测的部分才有 45 项测试和变异覆盖率，
才有"我验证过"这句话的底气。这条也是面试常被问的设计题。

---

## 8. F407 网关的运行时架构

### 8.1 三任务

| 任务 | 栈 | 优先级 | 干什么 |
|---|---|---|---|
| `taskModbus` | 512 words | BelowNormal | 五步轮询、组帧、收发、**入队**。不打印 |
| `taskReport` | 512 words | BelowNormal | 出队、`uprintf`。以后 MQTT publish 也在这里 |
| `taskLed` | 128 words | Low | PF10 绿灯翻转，心跳（`defaultTask` 待删） |

配置：`configTICK_RATE_HZ = 1000`、heap 15360、抢占式、`configCHECK_FOR_STACK_OVERFLOW = 2`。

**为什么采集和上报的栈要给 512 words（2KB）**：
`uprintf` 内部走 `vsnprintf`，一次能吃掉 1KB 栈。给 128 会 HardFault，而且不一定触发，随机崩。

### 8.2 数据怎么流动

```
taskModbus ──[osMessageQueuePut]──> s_q_sample(8) ──[osMessageQueueGet]──> taskReport
              mb_m_transaction_t                                              │
              **值拷贝，不传指针**                                              └─> uprintf → USART1
```

**为什么不传指针**：生产者那边 `mb_m_transaction_t s;` 是栈上的局部变量，
任务一 `osDelay` 就被改写。传指针过去的后果是**时好时坏的野指针**——最难的 bug。
几十字节换确定性，这笔买卖划算。

### 8.3 三个单写者 → 一个互斥量都不需要

| 资源 | 唯一写者 |
|---|---|
| USART2 / Modbus 总线 | taskModbus |
| USART1 打印口 | taskReport |
| `s_dropped` 丢包计数 | taskModbus |

**这是本项目的核心设计主张**。对比 `USE_NEWLIB_REENTRANT`：
一个是"人人都能写，加个开关让系统兜底"；一个是"设计上只有一个能写，压根不用兜底"。
**第二个放进简历比第一个值钱。**

### 8.4 三条学习成果（串起来讲）

```
ringbuf: head - tail，32 位溢出差值仍正确
DWT    : (uint32_t)(now - last)，25.6 秒溢出差值仍正确
Modbus : 一帧字节 async 到齐，靠 t3.5 静默判定
```
**同一个模式的三次出现** —— 能这么讲，说明你理解了而不是背下来了。

---

## 9. 打印与调试

### 9.1 uprintf（唯一允许的打印方式）

```c
static void uprintf(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(buf)) n = (int)sizeof(buf);   /* clamp */
        HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)n, 100);
    }
}
```

**绝对不用 `printf`**：没勾 MicroLIB 时它走 semihosting，**程序直接卡死**；
勾了 MicroLIB 又和 `#pragma import(__use_no_semihosting)` 的 retarget 冲突，而且 AC5/AC6 不一样。

*（注意：vsnprintf 返回的是"想写的长度"而不是"实际写的长度"，超长时必须 clamp，否则会把垃圾字节发出去。）*

### 9.2 LED 语义

| 现象 | 含义 |
|---|---|
| 绿灯 PF10 每 500ms 翻一次 | 正常，调度器活着 |
| **红灯 PF9 常亮** | 栈溢出 或 队列创建失败 → 墓碑，**不是**普通报错 |
| 绿灯停了 | 卡死在某个阻塞调用里 |

### 9.3 DWT 微秒计时（今天新增）

```c
#include "dwt_us.h"
dwt_init();                      /* SystemClock_Config() 之后 */
uint32_t t0 = dwt_us();
/* ... 要测的东西 ... */
uint32_t used = dwt_elapsed_us(t0);
```

168MHz 下分辨率 **5.95ns**，32 位 25.6 秒溢出。
详见 `docs/09-19-dwt-cyccnt.md`。

⚠️ **DWT 测的是墙钟时间不是 CPU 时间** —— 测量期间被抢占，会把别人的执行时间算进来。

---

## 10. ★ CubeMX Generate 之后的必做清单

CubeMX 每次 Generate 都会**重写 `.uvprojx`**，把你在 Keil 里手动加的东西冲掉。
**Generate 完必须重新过一遍：**

- [ ] **File Groups**：把 `$PROJ_DIR$\..\Drivers\modbus\mb_master.c`、`crc16.c`、`ringbuf.c` 重新加回来
- [ ] **Include Paths**：确认 `../../Drivers/modbus`、`../../Drivers/ringbuf`、`../../Drivers/crc16` 还在
- [ ] `HSE_VALUE` 确认是 **8000000**（CubeMX 有 25MHz 的默认，会覆盖回去，两个文件都要改：`stm32f4xx_hal_conf.h` 和 `.ioc`）
- [ ] 检查 `main.c` 里自己的代码还在不在（`while(1)` 里的内容可能会被删！）
- [ ] SYS 还是 Serial Wire
- [ ] 命令行编译验证 0 Error

> ⚠️ **`crc16.c` 最容易加两次**（它是多个模块都依赖的），加完记得数一遍：
> `Manage Project Items` 里每个 `.c` 应该且只应该出现一次。

### 工程名 / 路径的死规矩

**CubeMX 工程名和输出目录必须纯 ASCII。** 中文路径会导致：

```
"no source": Error: #219: error while deleting file "xxx????\yyy.d": No such file or directory
```

中文被处理成 `????`，源码根本没编译到。**父目录带中文无妨**（实测过）。

---

## 11. Git 规矩

```bash
# 本机必须用这一套，代理有 CA 问题
git -c http.sslVerify=false -c http.sslBackend=schannel push origin main
```

- **openssl 后端会 `SSL_read: unexpected eof`**，只有 **schannel** 能用
- 代理还会随机掐连接：报 `502` / `server closed abruptly` 就**原样重试一两次**
- 验证 push 成功用 GitHub API，别用 `ls-remote`
- GitHub: `xianmumu111/EdgeGateway-F4`（main）
- 身份：`user.name = 周雄伟` / `user.email = 15362954741@163.com`

### 提交前的三条自检

```bash
git status --short        # 有没有该入没入的新文件？
                          # （FreeRTOSConfig.h 曾经脏了整整一天没人发现）
git diff --stat           # 改的是不是你以为的那些
"D:\MDK5\UV4\UV4.exe" -j0 -b <proj> -o build.log    # 能不能编译过
```

> **曾经的真事**：`freertos.c` / `FreeRTOSConfig.h` / `stm32f4xx_hal_timebase_tim.c`
> 三个新文件一直是未跟踪状态，工作区脏了一整天。
> **新文件不会自己进版本库**，`git commit -a` 也不救。养成 `git status` 的习惯。

---

## 12. 症状对照表（排错先看这里）

### 编译 / 工具链

| 症状 | 原因 |
|---|---|
| 19 个 `#219: error while deleting file "xxx????\yyy.d"` | **工程名或输出目录含中文** |
| `L6200E: multiply defined symbol xxx` | 同一个 `.c` 被加进了两个文件组（常见于 crc16.c） |
| `L6218E: Undefined symbol xxx` | 忘了把 `.c` 加进 Keil 文件组，或者忘了加 IncludePath |
| `fatal error: reent.h: No such file` | 开了 `USE_NEWLIB_REENTRANT` —— newlib 是 gcc 的库，**Keil 下根本不存在**，改回 Disabled |

### 运行时

| 症状 | 原因 |
|---|---|
| 串口全乱码 | `HSE_VALUE` 还是 25000000，实际晶振 8MHz，**时钟差 3 倍** |
| 程序一 `printf` 就卡死 | 走 semihosting 了，用 `uprintf` |
| DMA 收双份数据 | `HAL_UARTEx_ReceiveToIdle_DMA` 默认开了 **HT 半传输中断**，必须 `__HAL_DMA_DISABLE_IT(hdmarx, DMA_IT_HT)`，**而且每次重启接收都要再关一次** |
| 栈溢出钩子里调 `HAL_Delay` 永远不返回 | 钩子执行时关了中断，TIM6 时基进不来 → 用 GPIO 拉红灯当墓碑 |
| `HAL_Delay` 延时不准 | SYS Timebase 没改成 **TIM6**（默认 SysTick 会和 FreeRTOS 抢） |
| `dwt_us()` 恒为 0 | 漏了 `CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk` |
| 读到寄存器值是 0x0A00 而不是 0x000A | 忘了大端/小端转换，走 `mb_put_be16` / `mb_get_be16` |

### 通信

| 症状 | 原因 |
|---|---|
| 从站不响应，主站一直超时 | ① 地址不对 ② CRC 错（它选择沉默）③ TX/RX 接反 ④ **忘了共地** |
| 收到前半帧 | 从站 t3.5 判据太小，帧没静默够就判定结束 |
| 高波特率下丢字节 | 接收路径太长，解决方案是 DMA + ringbuf（已在 F407 侧实现） |
| Modbus Poll 收到 `RX: 01 03...` 这种 ASCII | 从站开了 `MODBUS_DEBUG 1`，调试打印和协议数据共用 USART1。**接主站必须设 0** |

---

## 附：项目里程碑

| 节点 | 日期 | 内容 | 状态 |
|---|---|---|---|
| CP0 | 9/26 | ringbuf + 多任务跑通，可投简历版 | ✅ **已完成** |
| CP1 | 10/10 | Modbus RTU + 真实变送器 | 协议层全通，缺真 RS485 差分 + 真实变送器 |
| **CP2** | **10/24** | **MQTT 上云（秋招死线）** | 未开始，ESP8266 已有 ✅ |
| CP3/4/5 | 11/7·11/21·12/5 | 断网续传 / OTA / PCB | 未开始 |

降级预案：做不完砍 CP4/CP5，把 CP0~CP2 讲深。
**一个模块讲 10 分钟 > 五个模块各讲 1 分钟。**

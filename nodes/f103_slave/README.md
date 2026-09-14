# F103 Modbus RTU 从站节点

EdgeGateway-F4 的下行采集节点。STM32F103C8T6 运行一个**手写**的 Modbus RTU 从站协议栈，
通过 USART1 (TTL/RS485) 接受主站轮询，暴露一块 8 个 16 位寄存器的保持寄存器区。

> 主站是 `STM32F407ZGT6`（正点原子探索者），本节点是 1 主 2 从架构里的从站之一。

---

## 1. 硬件

| 项目 | 值 |
|---|---|
| MCU | STM32F103C8T6（蓝色最小系统板） |
| 主频 | HSE 8MHz × 9 = 72MHz |
| 调试口 | SWD（`__HAL_AFIO_REMAP_SWJ_NOJTAG`，不会锁死） |
| 串口 | USART1，PA9(TX) / PA10(RX) |
| 波特率 | **9600, 8N1** |
| 从站地址 | `0x01`（`MODBUS_SLAVE_ADDRESS`） |
| 工具链 | Keil MDK 5.36 + **AC5** |

### 接线（TTL 直连，先不上 RS485）

```
USB-TTL         F103
  TXD    -->    PA10 (RX)
  RXD    <--    PA9  (TX)
  GND    ---    GND
  3V3    ---    3V3   （若 USB-TTL 有输出；也可独立供电，但必须共地）
```

波特率误差：

```
USARTDIV = 72,000,000 / (16 × 9600) = 468.75
BRR 整数部分 468，小数 0.75×16 = 12 → 精确表示，误差 0.00%
```

---

## 2. ⚠️ 首次编译必读：HAL 库不在这个仓库里

`.gitignore` 忽略了 `Drivers/`（CubeMX 生成的 HAL + CMSIS，68MB，其中 48MB 是
DSP 静态库和例程，本项目根本不链接）。**但这不影响编译**，三步恢复：

1. 用 **STM32CubeMX** 打开本目录下的 `F103_ModbusSlave.ioc`
2. 右上角 **Generate Code**（一路 yes）
3. 打开 `MDK-ARM/F103_ModbusSlave.uvprojx`，Build

前提：本机已装 **STM32F1 固件包 v1.8.x**（CubeMX → Help → Manage embedded software packages）。

> 为什么这么做：把 68MB 第三方库塞进版本库，会让 clone 从 2 秒变成 5 分钟，
> 而且 diff 里全是别人的代码。`.ioc` 才是这份工程真正的"源码"。

---

## 3. 目录结构

```
nodes/f103_slave/
├── F103_ModbusSlave.ioc        ← CubeMX 配置，改时钟/引脚从这里进
├── .mxproject
├── Core/
│   ├── Inc/
│   │   ├── modbus_slave.h      ← 从站对外接口
│   │   └── ...（CubeMX 生成）
│   └── Src/
│       ├── modbus_slave.c      ← ★ 核心：协议栈全部实现
│       └── ...（CubeMX 生成）
├── MDK-ARM/
│   ├── F103_ModbusSlave.uvprojx
│   └── startup_stm32f103xb.s
└── test/                        ← ★ PC 仿真测试台（见下）
```

---

## 4. 协议实现

`Core/Src/modbus_slave.c`，四个对外接口：

```c
void     modbus_init(void);
void     modbus_rx_byte(uint8_t b);   /* 在 USART RXNE 中断里调用 */
void     modbus_poll(void);           /* 主循环里调用 */
void     modbus_tick(void);           /* 周期性调用，更新心跳/运行时长 */
```

### 支持的功能码

| 功能码 | 含义 | 响应 |
|---|---|---|
| `0x03` | 读保持寄存器 | `addr 03 <字节数> <数据...> CRC` |
| `0x06` | 写单个寄存器 | 原样回显请求帧 8 字节 |
| 其它 | 不支持 | 异常帧 `addr <fc\|0x80> 01 CRC` |

### 五道校验，一道都不能少

| 检查 | 失败时的行为 | 为什么 |
|---|---|---|
| 帧长 `len != 8` | **沉默** | 畸形帧，字段都读不全 |
| CRC-16/MODBUS | **沉默** | CRC 错了连地址字段都不可信，回什么都可能是错的 |
| 从站地址 | **沉默** | RS485 半双工总线，插嘴会和对面的响应撞车 |
| 参数范围（数量 1~125、地址不越界） | 异常帧 `0x02`/`0x03` | 主站靠回包判超时，能回就别让它干等 |
| 功能码 | 异常帧 `0x01` | 同上 |

> **为什么"能回异常帧就别沉默"**：主站超时通常是 1 秒。你沉默，它就干等；
> 总线上 10 个从站就是 10 秒。回一个 5 字节异常帧，主站立刻知道结果。

### 寄存器表

```c
#define REG_COUNT 8
static uint16_t regs[REG_COUNT] = {100, 200, 0, 0, 0, 0, 0, 0};
/*                                  ↑    ↑   ↑  ↑
                                  静态  静态 心跳 运行时长(秒)  */
```

`modbus_tick()` 每 500ms 递增 `regs[2]`、把 `HAL_GetTick()/1000` 写进 `regs[3]`。
**用 Modbus Poll 能看到数字在跳**——这是"真在跑"的肉眼证据。

---

## 5. PC 仿真测试台（不用烧板子就能验证协议）

```bash
cd test
run.bat        # 编译 + 跑 18 条用例
mutate.bat     # 变异测试：故意改坏源码 6 处，确认测试抓得住
```

原理：把 `modbus_slave.c` 原封不动搬到 PC 上编译，用假的 `HAL_UART_Transmit`
把输出接进一个"虚拟线路"数组，然后喂字节、比对吐出来的字节。
**改协议先在 PC 上跑一遍，几秒钟出结果，不用反复烧板子。**

当前状态：**PASS = 18, FAIL = 0**；变异测试 **CAUGHT = 6, MISSED = 0**。

详见 `test/README.md`。

### 手工测试帧（串口助手 hex 发送 / hex 显示）

| 发送 | 期望响应 | 验的是什么 |
|---|---|---|
| `01 03 00 00 00 02 C4 0B` | `01 03 04 00 64 00 C8 BA 7A` | 正常读 2 个 |
| `01 03 00 00 00 01 84 0A` | `01 03 02 00 64 B9 AF` | 读 1 个 |
| `01 03 00 10 00 01 85 CF` | `01 83 02 C0 F1` | 地址越界 → 异常 02 |
| `01 03 00 00 00 7E C5 EA` | `01 83 03 01 31` | 数量 126 超限 → 异常 03 |
| `01 03 00 00 00 02 C4 0C` | **无响应** | CRC 改一位 → 沉默 |
| `02 03 00 00 00 01 84 39` | **无响应** | 地址不是本机 → 沉默 |
| `01 06 00 02 03 E8 28 B4` | 回显同样 8 字节 | 写寄存器 |
| `01 03 FF FF 00 09 85 E8` | `01 83 02 C0 F1` | 整数溢出攻击（见下） |
| `01 03 40 21` | **无响应** | 4 字节短帧 → 沉默 |

---

## 6. 已知缺口 / TODO

### ① 字符间隔 t1.5 未实现（规范容错）

Modbus 规范要求：**字符间隔 > 1.5 字符判帧错丢弃，> 3.5 字符算新帧**。
现在只在 `modbus_poll()` 里判断"最后一字节之后静默了多久"，
没判断**字节与字节之间**隔了多久。所以主站发得断断续续时，
本该丢掉的字节会被拼成一帧。

补法（在 `modbus_rx_byte()` 里）：

```c
static uint32_t last_byte_tick;
...
if (rx_len > 0 && (HAL_GetTick() - last_byte_tick) > MODBUS_T15_MS) {
    rx_len = 0;                 /* 间隔过大，前面那些字节作废 */
}
last_byte_tick = HAL_GetTick();
```

### ② 未实现 `0x10` 写多个寄存器

### ③ 未接 RS485

现在跑 TTL 直连。接 RS485 需要：
- 一个 DE/RE 控制引脚（如 PA8），发送前置高、发完置低
- 发送完成必须用 **TC 中断**（`HAL_UART_TxCpltCallback`）而不是 `HAL_UART_Transmit` 返回，
  否则最后一个字节还在移位寄存器里就把 DE 拉低了，会丢尾部

---

## 7. 别忘了

`modbus_slave.c` 里的 `MODBUS_DEBUG` 开关，上板联调前**必须是 0**。
置 1 时每帧会往 USART1 吐 `RX: ...` 几十个 ASCII 字节，主站收到的第一帧永远是垃圾。
（仿真台会自动抓这个：置 1 时 18 条用例全部变红。）

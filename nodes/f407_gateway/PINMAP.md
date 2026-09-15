# F407 探索者（正点原子）引脚规划表

> 数据来源：`Explorer STM32F4_V2.2_SCH.pdf` + `探索者IO引脚分配表.xlsx`（ALIENTEK 官方资料）
> 核对日期：2026-09-15。以下全部为**原理图确认值**，不是猜的。
> 板子版本：**V2.2**

---

## 1. 本项目的三路串口（互不冲突）

| 外设 | 引脚 | 板上资源 | 跳线 | 用途 |
|---|---|---|---|---|
| **USART1** | PA9 / PA10 | 板载 CH340G → PC | **P6** | printf / Shell / 调试输出 |
| **USART2** | PA2 / PA3 | 板载 SP3485 → RS485 | **P9 拨到 485** | Modbus RTU 下行，9600 |
| **USART3** | PB10 / PB11 | ATK-MODULE 接口 | **P10 拨到 MODULE** | ESP8266 AT 指令，115200 |

**RS485 方向控制：`PG8`（RS485_RE，DE/RE 合一）—— 发=1，收=0。**

---

## 2. 板载外设引脚

| 功能 | 引脚 | 备注 |
|---|---|---|
| W25Q128 (16MB SPI Flash) | SCK=**PB3** / MISO=**PB4** / MOSI=**PB5** / CS=**PB14** | OTA 下载区、参数区、环形日志 |
| I2C1 | SCL=**PB8** / SDA=**PB9** | MPU6050 + 24C02 + WM8978 共用，板上已 4.7K 上拉 |
| MPU6050 中断 | **PC0**（3D_INT） | 同时是 ATK-MODULE 的 LED 脚 |
| SD 卡 (SDIO) | D0=PC8 D1=PC9 D2=PC10 D3=PC11 SCK=PC12 CMD=PD2 | |
| LED 红 (DS0) | **PF9** | |
| LED 绿 (DS1) | **PF10** | |
| 蜂鸣器 | **PF8** | |
| 按键 | KEY2=PE2 / KEY1=PE3 / KEY0=PE4 / WK_UP=PA0 | |
| 光敏传感器 | **PF7** | |
| 外扩 SRAM 1MB (IS62WV51216) | FSMC，**片选 PG10 (NE3)** | 地址/数据线与 LCD 共用 FSMC |
| TFTLCD | FSMC，**片选 PG12 (NE4)** | |
| 以太网 LAN8720 | PA1 / PA7 / PC1 / PC4 / PC5 / PG11 / PG13 / PG14 + **PD3(RESET)** | ⚠️ 见下方冲突 2 |

---

## 3. 三个必须知道的冲突

### ⚠️ 冲突 1：PB3 / PB4 的 JTAG ↔ SPI1

```
PB3 = JTDO  = SPI1_SCK    ← W25Q128 用
PB4 = JTRST = SPI1_MISO   ← W25Q128 用
```

**CubeMX → SYS → Debug 必须选 `Serial Wire`（2 线），绝对不能选 `JTAG(5 pins)`。**

选成 JTAG 模式，PB3/PB4 会被占成调试口，**W25Q128 直接失效**，症状是"SPI 读回来全 0xFF"，
很容易误判成自己 SPI 配置写错了。

### ⚠️ 冲突 2：PA2 同时是 RS485_RX 和 ETH_MDIO

**RS485 和以太网不能同时用。** 本项目上行走 WiFi（ESP8266），正好避开。

如果以后要改成以太网做 MQTT 上行，RS485 就必须挪到别的串口（USART3 或软件串口）。

### ⚠️ 冲突 3：PG8 同时是 RS485_RE 和 NRF_IRQ

无线模块（NRF24L01）的中断脚和 485 方向控制共用。本项目不用 NRF24L01，无影响。

---

## 4. 跳线帽设置（当前项目）

| 编号 | 名称 | 应拨到 |
|---|---|---|
| P6 | 串口选择 | 接通（USART1 连 CH340，printf 才能出） |
| P9 | RS232 / 485 | **485 侧** |
| P10 | RS232 / 模块 | **MODULE 侧**（ESP8266 走 USART3） |
| P11 | CAN / USB | 无所谓（本项目不用） |

**症状对照**：跳线拨错时，程序"看着在跑但收不到任何东西"，99% 的人会先怀疑代码。

---

## 5. SWD 调试接线（20 针座，标准 ARM 定义）

原理图第 2 页确认的座子定义：

```
VDD   1   2  VDD
TRST  3   4  GND
TDI   5   6  GND
TMS/SWDIO 7   8  GND
TCK/SWCLK 9  10  GND
NC   11  12  GND
TDO/SWO 13  14  GND
RESET# 15  16  GND
NC   17  18  GND
NC   19  20  GND
```

**只接 4 根**：

| ST-Link | → | 20 针座 | MCU |
|---|---|---|---|
| 3.3V (VTref) | → | 1 脚 | — |
| SWDIO | → | 7 脚 | PA13 |
| SWCLK | → | 9 脚 | PA14 |
| GND | → | 4 / 20 脚 | — |

1 脚是**方形焊盘**（其余是圆的）。接反 SWDIO/SWCLK 不会烧板，只是连不上，换过来即可。

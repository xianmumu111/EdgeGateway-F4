# 09-20 · ESP8266 AT 打通（CP2 起步）

> 目标（今天的下限）：**F407 通过 USART3 收到 ESP8266 吐出的字节，并发 `AT\r\n` 拿到 `OK\r\n`。**
> 今天不做 MQTT，不做 WiFi 连接。只打通「物理层 + 行解析」这条最小闭环。

---

## 0. 动手前：仓库已经脏了 22 个文件

昨天（9/19）的成果**到此刻一个 commit 都没有**：

```
 M 17 个文件（含 freertos.c / modbus_slave.c / 两侧 .ioc / 三份文档）
 ?? nodes/f407_gateway/Core/Inc/dwt_us.h      ← 未跟踪
 ?? nodes/f103_slave/Core/Inc/dwt_us.h        ← 未跟踪
 ?? nodes/f407_gateway/Core/Inc/tim.h         ← 未跟踪（TIM2 的）
 ?? nodes/f407_gateway/Core/Src/tim.c         ← 未跟踪（TIM2 的）
 ?? Docs/09-19-rtos-observability.md          ← 未跟踪
```

**先收，再动。** 理由不重复了 —— 昨天为此已经吃过一次亏。

同时顺手清掉欠了三天的 **`defaultTask`**（它还在，白白占 512 字节栈 + 一个比 taskModbus 更高的优先级）。

有一个坑我现在才发现：**仓库里那个文档目录磁盘上的真名叫 `Docs`（大写 D）**，但 git 索引里已跟踪的文件全是小写 `docs/`。
Windows 不区分大小写所以你没感觉，但**别人 clone 下来会看到两个目录**。一会儿我用小写路径 add，把这个统一掉。

---

## 1. 为什么今天动 ESP8266

| 理由 | 说明 |
|---|---|
| **它是当前唯一的排期风险** | CP2（MQTT 上云）死线 **10/24**，只剩 34 天。Modbus 侧已经能讲很久了 |
| **不需要新硬件** | 你确认过 ESP8266 在手；USART3 走 ATK-MODULE 接口，板上现成 |
| **能复用已跑通的套路** | DMA + IDLE + ringbuf —— 你在 USART2 上踩过的坑，今天就全是预判而不是 debug |

而且今天能顺带验证一件事：**ringbuf 当初设计成「实例（`rb_t` 结构 + 指针）」而不是「单例（模块内一个 static 缓冲）」，是不是真的值。**
USART3 要第二份独立的 ringbuf —— 如果你的设计是对的，改动应该只等于多一行 `RB_STATIC_DEFINE`。这是设计能力的直接证据。

---

## 2. 硬件：接线前有两件事会烧东西

### ⚠️ 2.1 电压

ESP8266 **只能吃 3.3V，5V 上去就废**。ATK-MODULE 座子的电源脚是 3.3V 还是 5V，**我没查到原理图确认，不给结论**。

> 你自己验：万用表直流电压档，黑表笔接 GND，红表笔量座子上标 VCC 的脚。
> **必须读到 3.2~3.4V 才能插。** 读到 5V 就别插 —— 换 3.3V 脚，或者外接 AMS1117。

### ⚠️ 2.2 供电能力

ESP8266 发射瞬间峰值 **200~300mA**。探索者板上的 3.3V 一般是 AMS1117，标称 1A，但如果你的板子是**通过 ST-Link 或 USB 供电**，前面还串着一整个链路，压降可能让它起不来。

**供电不足的症状很有特点：模块不停复位 → 串口上周期性刷出乱码。**
如果你看到"每隔一两秒来一串垃圾"，先怀疑电源，别怀疑波特率。

> 稳妥做法：给 ESP8266 单独一路 3.3V（外部稳压），**GND 必须和 MCU 共地**。

### 2.3 引脚与跳线

| 项 | 值 |
|---|---|
| MCU 侧 | **USART3 PB10(TX) / PB11(RX)** |
| 跳线 | **P10 拨到 MODULE 侧**（见 `PINMAP.md` 第 4 节） |
| 接线 | MCU-TX → ESP-RX，**MCU-RX → ESP-TX**（交叉） |
| ESP-01S | 一般内部已带 EN/GPIO0 上拉，VCC/GND/TX/RX 四根就够 |
| ESP-01（老款，无 S） | 需要额外把 **EN(CH_PD)** 拉到 3.3V，否则不工作 |

---

## 3. Stage 1 · CubeMX（20 分钟）

打开 `nodes/f407_gateway/f407_gateway.ioc`：

1. **USART3** → Mode = Asynchronous
2. **Baud Rate = 115200**，8N1（默认）
3. GPIO 自动落 **PB10 / PB11**；如果不是，手动挪过去
4. **DMA Settings** → Add → `USART3_RX`，Mode = **Normal**，Increment address ✔
5. **NVIC** → 勾上 `USART3 global interrupt` + CubeMX 分配的那条 DMA stream IRQ
6. Generate Code

> DMA stream 具体是几号由 CubeMX 分配（USART2 已经占了 DMA1_Stream5），**不用你指定，生成后记下来写进 `PINMAP.md`**。

### ⚠️ Generate 后的必做清单（第 N 次重申，因为它每次都会冲掉）

- [ ] **文件组**：`mb_master.c` / `crc16.c` / `ringbuf.c` 加回 MDK-ARM 工程
- [ ] **IncludePath**：`../../Drivers/modbus`、`../../Drivers/ringbuf`、`../../Drivers/crc16`
- [ ] **`stm32f4xx_hal_conf.h` 的 `HSE_VALUE` 改回 `8000000U`**
- [ ] 新建的 `tim.c`（TIM2 那套）如果没自动进组，手动加上 —— 否则 TaskModbus 里的 `StartTimer01` 之类的外部引用会 undefined

---

## 4. Stage 2 · 透传探针（今天的硬指标）

**先不要写任何 AT 解析。** 第一步只做一件事：把 USART3 收到的字节原样转发到 USART1（你的调试口）。

为什么要先做这个？因为它把"打开的是不是对的串口、波特率对不对、模块有没有供电、跳线拨没拨对"这四个变量**一次性全部验证掉**。
如果跳过这一步直接写 AT 组包，失败了你会不知道是协议写错还是线根本没通。

### 4.1 新建 `Drivers/esp/esp_port_f4.c` / `.h`

结构照抄你自己的 `mb_port_f4.c` —— **不是照抄代码，是照抄它的分层思路**：

| `mb_port_f4.c` | `esp_port_f4.c` | 差异 |
|---|---|---|
| `RB_STATIC_DEFINE(s_rx_rb, 256)` | `RB_STATIC_DEFINE(s_esp_rb, 1024)` | AT 响应比 Modbus 帧长，**给 1024** |
| `s_dma_buf[128]` | `s_esp_dma_buf[128]` | 同 |
| `HAL_UARTEx_RxEventCallback` | 同一个函数里**加一个 USART3 分支** | 唯一回调，靠 `huart->Instance` 分流 |
| `mb_port_init()` 里启 DMA + 关 HT | `esp_port_init()` 完全相同 | **HT 必须关**，老坑第三次 |

接口先定这么四个，**函数体你自己写**：

```c
void     esp_port_init(void);                              /* 启 DMA+IDLE，关 HT */
uint32_t esp_port_send(const char *cmd);                   /* 见下方 TODO-1 */
uint32_t esp_port_read(uint8_t *dst, uint32_t cap);        /* 一次把 ringbuf 掏空 */
void     esp_port_flush(void);                             /* 丢弃残留，发命令前用 */
```

### ★ TODO-1（这个要想，不是体力活）

`esp_port_send("AT")` —— **`\r\n` 该由调用方补，还是由 `esp_port_send` 内部补？**

我把答案的一半给你：AT 指令**必须**以 `\r\n` 结尾，单独一个 `\n` 模块不认。所以要么每次调用都写 `esp_port_send("AT\r\n")`，要么函数内部拼。

另一半自己定，然后回答：**如果让函数内部拼，"函数给别人用"这层就是不可见的，好处和代价分别是什么？**

（提示：想想 caller 传进来的 `cmd` 有多长、如果不内部拼，`len` 怎么算。）

### 4.2 探针代码（放在 taskEsp 里，临时版）

```c
void StartTaskEsp(void *argument)
{
    (void)argument;
    uint8_t  b[64];
    uint32_t n;

    /* ⚠️ esp_port_init() 在这里调一次。和 mb_port_init() 一样，
       绝对不要在 main.c 里也调一遍 —— DMA 会被重启，行为诡异。 */

    for (;;)
    {
        n = esp_port_read(b, sizeof(b));
        if (n > 0U)
        {
            /* TODO-2: 把 n 个字节原样打到 USART1。
               要求：每个字节用 %02X 打印并带空格，末尾补 \r\n。
               为什么先不用 %s？——见下方 TODO-3 */
        }
        osDelay(10);
    }
}
```

任务栈给 **512 words**，优先级和 taskReport 同级或略低。

### ★ TODO-3（这里是今天第三个要想的点）

```
[RPT][S0] TX: ...        ← taskReport 在写 USART1
[ESP] 41 54 0D 0A ...    ← taskEsp 现在也在写 USART1
```

你之前立过一个很有说服力的论点：**「三个资源各有一个写者，所以一个互斥量都不需要」**。
现在第四根柱子冒出来了，而且和 taskReport **抢同一个串口**。

自己回答三个小问：
1. 两个任务同时 `uprintf`，最坏会发生什么？（提示：`HAL_UART_Transmit` 是阻塞的，一次调用内部不会被打断 —— 那交错会发生在什么粒度上？）
2. 今天可以先用"两个任务都直接打印"凑合吗？可以的话，理由够不够向面试官交代？
3. 真要治本，最小改动是什么？（提示：不用引入互斥量也行 —— 想想你已有的队列）

### 4.3 上电，看串口助手

**期望输出**（插电/复位模块时会来一串）：

```
[EPS] 54 FF 12 ...          ← 前几个字节可能是乱码，正常，见下
[EPS] 41 69 2D 54 68 ...    ← "Ai-Thinker Technology Co. Ltd." 的十六进制
[EPS] 72 65 61 64 79 0D 0A  ← "ready\r\n"
```

**三个可能的坏结果，各自对号入座：**

| 现象 | 大概率原因 |
|---|---|
| **一个字节都没有** | P10 跳线 / TX-RX 没交叉 / ESP 没供电（CH_PD 未拉高） |
| **稳定、周期性的乱码，看不到 ready** | 供电不足在反复复位，或者波特率不对 |
| **只有开头几个字节乱码，之后干净** | ✅ 正常。ESP8266 的 **ROM boot 日志是 74880 波特率**，AT 固件起来后才切到 115200。**上电乱码是正常的，不要去改波特率。** |

拿到 `ready` 或任何可辨认的文本 → **Stage 2 完成**，去洗碗休息十分钟。

---

## 5. Stage 3 · 行解析层（今天能做就做，做不完明天）

现在字节到了，但它们是**一条流**，不是一行一行来的。`\r\n` 可能落在两次 DMA 回调中间。

所以需要一个**跨调用保持状态的行缓冲** —— 这是和 Modbus 完全不同的一类问题：

| | Modbus RTU | ESP AT |
|---|---|---|
| 帧边界 | 3.5 字符静默（**时间**） | `\r\n`（**字符**） |
| 能不能靠"等一段时间"判定结束 | 能 | 不能（也不知道响应有几行） |
| 需要跨调用状态吗 | 不需要（长度可预算） | **需要**（半行） |

> 这就是 9/17 第 4 问的另一半：**当时我说"文本协议靠字符定界可以不用定时器"，但代价正好是今天这个 —— 你得自己维护行状态机。**

### 5.1 `Drivers/esp/esp_line.c` / `.h`（纯 C，**可以在 PC 上测**）

```c
#define ESP_LINE_MAX 160

typedef struct {
    char     buf[ESP_LINE_MAX];
    uint16_t len;
    uint16_t overrun;     /* 因为太长被丢掉的字节数 */
} esp_line_t;

void     esp_line_reset(esp_line_t *st);
int      esp_line_feed(esp_line_t *st, char c);   /* 返回 1 = 凑成完整一行 */
int      esp_line_is_ok(const char *line);        /* 严格等于 "OK" */
int      esp_line_is_err(const char *line);       /* "ERROR" 或 "FAIL" 之一 */
```

写的时候注意这四条，都是会被边界条件咬到的地方：

1. `c == '\n'` 才算结束；`'\r'` 直接丢弃（不要存进 buf）
2. 凑齐一行时 `buf[len] = '\0'` —— **忘这一步，后面所有字符串判断都会读越界**
3. `len` 到了上限还没换行怎么办？（提示：`overrun` 就是为这个准备的，别直接复位到 0，不然下一次会拿到半截脏行）
4. 空行（`\r\n` 紧挨着）要过滤掉 —— AT 响应开头经常有

### 5.2 单测（`test/test_esp_line.c`）

照你 `test_master.c` 的老规矩，**写完必做变异测试**：

- 正常：`"OK\r\n"` 逐字节喂 → 第 3 个字节喂进去时返回 1，`buf == "OK"`
- 半行：喂 `"O"` → 返回 0；再喂 `"K\r"` → 返回 0；再喂 `"\n"` → 返回 1
- 空行：`"\r\n"` → 返回 1 且 `buf` 为空串
- 超长：喂 200 个 `'A'` → 不能越界，`overrun > 0`
- 粘包：`"OK\r\nERROR\r\n"` → 两次返回 1，`buf` 依次是 `"OK"`、`"ERROR"`

变异测试至少做这三条（改实现，确认**一定会红**）：
- 删掉 `buf[len]='\0'`
- 把 `'\n'` 判据改成 `'\r'`
- `len` 上限判定改成 `> ESP_LINE_MAX`（应为 `>=`）

---

## 6. Stage 4 · 真正的握手

Stage 3 通了才有这一步。流程：

```
esp_port_flush();                  /* 丢掉残留 */
esp_port_send("AT\r\n");
循环: esp_port_read() → esp_line_feed() → 每凑成一行 uprintf 出来
      遇到 esp_line_is_ok()   → 握手成功
      遇到 esp_line_is_err()  → 失败
      超时 2000ms              → 无响应
```

**期望一行 `OK`。** 拿到之前别往下做 CWMODE/CWJAP。

> 如果完全没有响应（连 ERROR 都没有），三种可能按顺序排查：
> ① 线没交叉 / TX 方向；② 模块没有 AT 固件（那就得刷机，那是另一天的活，先用 `AT+GMR` 验一下）；③ 波特率不是 115200。

---

## 7. 验收清单

- [ ] 仓库干净：9/19 的成果已 commit，工作区无未跟踪文件
- [ ] `defaultTask` 删掉
- [ ] ESP8266 上电，USART1 上能看到 `ready`（哪怕前面带乱码）
- [ ] 发 `AT\r\n`，收回来一行 **`OK`**
- [ ] `test_esp_line` 全绿 + 变异测试全红（若做到 Stage 3）
- [ ] `PINMAP.md` 补上 USART3 的 DMA stream 号

---

## 8. 收尾

1. **DEVLOG 9/18 + 9/19 + 9/20** —— 9/18 那条还是空的。今天撞到的坑（irmware 乱码 / 供电 / DMA stream 冲突）就是现成的卡点素材，**自己写**
2. commit + push（代理抽风时用 60 秒退避，别密集重试）

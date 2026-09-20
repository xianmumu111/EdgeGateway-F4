# DWT CYCCNT 精确计时 —— 2026-09-19 下午作战单

> 目标：把计时分辨率从 **1ms** 提到 **6ns**（280 倍），并顺手修掉从站 t3.5 判据的规范偏差。
> 不需要任何新硬件。今天全是软件活。

---

## 0. 为什么必须做（昨天的痛）

| 现象 | 原因 |
|---|---|
| S0/S1/S2 帧长 17/16/15 字符，**实测全是 6ms** | 相邻差 260µs，`HAL_GetTick()` 分辨率只有 1000µs，整段被抹平 |
| 从站实际只等了 ~1.2ms，规范要求 ≥1750µs | `MODBUS_T35_MS = 2` 遇上 tick 向下取整，实际落在 (1, 2] ms |

**一句话**：你在用一把只有厘米刻度的尺子量毫米。

---

## 1. DWT 是什么

Cortex-M3/M4 内核自带一个 **Data Watchpoint and Trace** 单元，里面有个 32 位计数器 `CYCCNT`，
**每个 CPU 时钟周期 +1**，不占用任何定时器、不占外设、不需要初始化任何 GPIO。

| 芯片 | 主频 | 分辨率 | 溢出周期 |
|---|---|---|---|
| STM32F407 | 168 MHz | **5.95 ns** | 25.57 秒 |
| STM32F103 | 72 MHz | 13.9 ns | 59.65 秒 |

三个寄存器，记住名字就够：

```
CoreDebug->DEMCR   bit24 = TRCENA    总开关（必须先开）
DWT->CTRL          bit0  = CYCCNTENA 计数器使能
DWT->CYCCNT                          当前计数值
```

> ⚠️ **这次我把代码给全**，理由说清楚：DWT 是**测量工具**，不是你要证明自己会写的算法。
> 它就像游标卡尺 —— 你不需要先学会造卡尺再去量零件。
> 但下面「Part 6 三个必须懂的点」你得能讲出来，那是面试会问的。

---

## 2. F407 侧：新建 `nodes/f407_gateway/Core/Inc/dwt_us.h`

⚠️ **放在 `Core/Inc/` 是关键** —— CubeMX 的 IncludePath 里本来就有这个目录，
所以**不用改 Keil 的 Include Paths**，也不会被 Generate 删掉。
做成 header-only 的另一个好处：不用往 Keil 文件组里加新的 `.c`。

```c
#ifndef DWT_US_H
#define DWT_US_H

#include <stdint.h>

/* 由调用方保证 #include "main.h"（里面会带上 MCU 头，进而带上 CMSIS 的 core_cm4.h）。
 * 别在这个头文件里 include MCU 头文件 —— F1/F4 工程共用同一份实现，各自 include 各自的。
 *
 * TODO-A: 想想为什么这里必须写成 static inline 而不是普通函数？
 *   提示：这个 .h 会不会被两个以上的 .c 同时 include？
 *   static 意味着什么链接属性？如果写成普通函数会发生什么事（链接阶段）？
 */

__STATIC_INLINE void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;   /* ← 总开关，漏了这行后面全是 0 */
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

__STATIC_INLINE uint32_t dwt_cycles(void)
{
    return DWT->CYCCNT;
}

__STATIC_INLINE uint32_t dwt_us(void)
{
    /* TODO-B: SystemCoreClock 为什么可以直接用？
     *   提示：它是 CMSIS 提供的全局变量，谁在什么时候把它设对的？
     *   如果在 SystemClock_Config() 之前调 dwt_us()，会算出什么？ */
    return DWT->CYCCNT / (SystemCoreClock / 1000000UL);
}

/* 两个时刻之差。**永远用这个宏，不要手写 a - b 再判正负**。
 *
 * TODO-C: 为什么 (uint32_t)(now - last) 在溢出的情况下依然是对的？
 *   提示：和 ringbuf 的 head - tail 是同一个道理 —— 举个数自己算一遍：
 *         last = 0xFFFFFFF0, now = 0x00000010（溢出了），(uint32_t)(now-last) = ?
 */
__STATIC_INLINE uint32_t dwt_elapsed_us(uint32_t last)
{
    return (uint32_t)(dwt_us() - last);
}

#endif /* DWT_US_H */
```

### 必看的写法细节

`dwt_elapsed_us(last)` 里我传的是「起始时刻」而不是做了个宏「录一个全局变量」。
理由：**它可以被多处同时使用而互不干扰**。如果做成全局的 start 变量，
两个地方同时计时就会互相踩 —— 跟 `USE_NEWLIB_REENTRANT` 要解决的问题是同一类，
只不过这次是**用设计避开**而不是加开关兜底。

---

## 3. F407 侧：改 `freertos.c`（3 处）

### 3.1 include（`USER CODE BEGIN Includes`）

```c
#include "dwt_us.h"
```

### 3.2 初始化（`MX_FREERTOS_Init` 的 `USER CODE BEGIN Init`，最上面一行）

```c
dwt_init();
```

⚠️ **必须在这里而不是更早** —— `MX_FREERTOS_Init()` 是在 `main()` 里 `SystemClock_Config()` **之后**才调用的，
此时 `SystemCoreClock` 已经是 168000000。写早了除法分母不对，读数会错一整倍数。

### 3.3 结构体字段 + 计时（采集任务里）

**改结构体**：把 `elapsed_ms` 换掉

```c
    uint32_t elapsed_us;       /* 本步耗时，单位 µs（DWT 测得） */
```

**改计时**（现在的样子）：

```c
    ms = HAL_GetTick();
    e = mb_port_transfer(...);
    s.elapsed_ms = HAL_GetTick() - ms;
```

改成：

```c
    uint32_t t0 = dwt_us();
    e = mb_port_transfer(...);
    s.elapsed_us = dwt_elapsed_us(t0);
```

**改打印**：上报任务里搜索 `elapsed_ms`，连同 `s.reg_count` 那几行的 `%lu` 一起改：

```c
    uprintf("[RPT][S%u] OK  %luus", (unsigned)s.step_index, (unsigned long)s.elapsed_us);
```

> 注意 `elapsed_us` 是 `uint32_t`，Keil 下 ` %u` 也行，但统一写 `%lu` + cast `(unsigned long)` 更保险。

`ms` 这个变量如果不再用就删掉，别留着。

---

## 4. F103 侧：把 t3.5 判据换成微秒

同样先在 `nodes/f103_slave/Core/Inc/` 放一份 `dwt_us.h`（内容**完全一样**，它是 MCU 无关的）。
CMSIS 里 M3 的 `CoreDebug` / `DWT` 结构体也在，`__STATIC_INLINE` 一样适用。

### 4.1 `modbus_slave.c` 顶部宏（`MODDBUS_BAUD` 那段）

```c
#if MODBUS_BAUD > 19200
#define MODBUS_T35_US  1750UL        /* 规范固定值，不再写 2ms */
#else
/* 35000000 / baud，向上取整到 µs：分子先加 (baud-1) 再除 */
#define MODBUS_T35_US  ((35000000UL + MODBUS_BAUD - 1UL) / MODBUS_BAUD)
#endif
```

> ⚠️ **我第一版写错了，你改对了，记一笔**：
> 我原来写的是 `((35000000UL / MODBUS_BAUD) + 999UL)` —— 那是"向上取整到**毫秒**"的写法，
> 套在 µs 上会得到 911+999 = **1910µs**，比规范值凭空多 1000µs。
> 正确的整数向上取整是 `(a + b - 1) / b`，不是 `a/b + 常数`。
> （幸好 38400 走的是 >19200 分支，这个错误没影响今天的结果 —— **没跑到的代码里的 bug 不算 bug，但它迟早会跑到**。）

> **对比一下老写法**：`(35000UL / MODBUS_BAUD) + 1` 毫秒。
> 9600bps 下：老写法 = 3+1 = **4ms**；规范值其实是 3.6458ms → 新写法 **3646µs**。
> 老写法 4ms 是对的（向上取整到毫秒），但**白白多等了 354µs**。
> 38400 下更夸张：老写法 2ms vs 规范 1750µs，多等 250µs。**每帧都多等，吞吐就下来了。**

### 4.2 改变量类型

```c
static volatile uint32_t last_rx_us = 0;      /* 原 last_rx_tick */
```

`modbus_init()` 里同名的那句清零也改。

### 4.3 `modbus_rx_byte()` （原 `:105`）

```c
    last_rx_us = dwt_us();
```

### 4.4 `modbus_poll()` 判据（原 `:125`）

```c
    if (dwt_elapsed_us(last_rx_us) < MODBUS_T35_US)
```

### 4.5 别忘了 `dwt_init()`

在 `main.c` 里、`SystemClock_Config()` 之后、`while(1)` 之前加一行。

---

## 5. 验收：这次的数据要能对上推理

### 5.1 先算理论值（我已经算好了，38400bps）

发送和接收都在线上走，每字符 260.4 µs。主站测到的耗时应该 = 下面几段之和：

```
elapsed ≈ (tx_len + rx_len) × 260.4       线路传输
        + 1750                            从站等够 t3.5 才认为帧结束
        + 260                             主站侧 IDLE（1 个字符）才认为响应到齐
        + <100                            两边软件处理
```

| 步 | 字符数 | 线路 | +t3.5+IDLE | **预测 µs** | 昨天 ms 实测 |
|---|---|---|---|---|---|
| S0 | 8+9=17 | 4427 | +2010 | **~6440** | 6ms ✓ |
| S1 | 8+8=16 | 4167 | +2010 | **~6180** | 6ms ✓ |
| S2 | 8+7=15 | 3906 | +2010 | **~5920** | 6ms ✓ |
| S3 | 8+5=13 | 3385 | +2010 | **~5400** | 5ms ✓ |
| S4 | 8（无响应） | 2083 | + 200000 软件超时 | **~202300** | 202ms ✓ |

**昨天那批 ms 读数全部落在预测区间（向下取整后）** —— 说明这个模型是对的。

### 5.2 三条硬性验收

1. **S0/S1/S2/S3 必须两两差得开**：相邻理论差 260µs，读数差的绝对值应该在 **200~350µs** 之间。
   如果四个数还是一样（比如全是 6200），说明 `dwt_us()` 没干活 —— 回去查 DEMCR 那行。
2. **S4 必须还是 ~202ms**（±1ms）。变了说明你动到了超时逻辑。
3. **改完从站 t3.5 后，五步结果与改之前完全一致**（9 帧字节不变）。
   这一条证明：**从保守等待改成精确等待没有破坏兼容性**。

### 5.3 顺手还债（昨天那三件）

- TX 行末尾补 `\r\n`
- `:334` 的 `if` 加大括号
- 删掉 `defaultTask`（CubeMX → Tasks and Queues → 删行）

---

## 6. Part 6：三个必须懂的点（面试会问）

### ① DWT 测的是「墙钟时间」，不是「CPU 时间」

这是今天最容易被忽略、但最重要的一条。

你的 `taskModbus` 在测量期间如果被 `taskReport` 抢占，`dwt_elapsed_us` 会把
**别人的执行时间也算进去**。

- 现在队列里几乎总是空的（采集 500ms 一条，上报立刻取走），所以影响极小
- 但如果哪天你测出来 S0 偶发 8ms（正常 6.4ms），别怀疑硬件 —— **先怀疑调度**

想测纯 CPU 时间怎么办：**测量前后 `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()`**。

> 但注意：这里**不能**用临界区 —— 你要测的恰恰是「等在串口上」的时间，
> 把抢占关掉反而不真实。什么时候用哪个，取决于你想回答什么问题。

### ② 溢出回绕 —— 和 kfifo 是同一个套路

`ringbuf` 里 head/tail 单调递增永不取模，`used = head - tail`，32 位溢出时差值依然正确。
DWT 完全一样。区别只有一个：**ringbuf 里你自己管两个变量，DWT 里硬件替你数。**

串起来讲，比分开讲值钱。

### ③ `__disable_irq()` 期间 CYCCNT 照跑

从站 `modbus_poll()` 的临界区里 `HAL_GetTick()` 依赖 SysTick 中断 —— 关了中断就不前进。
而 CYCCNT 是**纯硬件计数器，不需要任何中断**。

这反而更适合在临界区里用。**刚好补上了原来那个方案的短板。**

---

## 7. 其他坑（排错先看这张表）

| 症状 | 九成是这个原因 |
|---|---|
| `dwt_us()` 恒返回 0 | 漏了 `CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk` |
| 读数差 168 倍 | `SystemClock_Config()` 之前就调了 `dwt_init()` / 时钟没换成 PLL |
| HardFault | `CoreDebug` 地址访问需要 `TRCENA`；极少数情况是头文件没包含到 CMSIS |
| 串口打印出 `lu` 或乱码数字 | `%lu` + `(unsigned long)` cast，别直接喂 `uint32_t` 给 `%u` 乱试 |
| 调试退出后再进，读数乱跳 | Keil Trace 配置动过的话，复位一次板子 |
| 程序跑 30 秒以上某个差值突然巨大 | 一定是有人写了 `if (now > last)` 这种比较而不是 `(uint32_t)(now-last)` |

> ✅ 顺一记：从站 `modbus_poll()` 的临界区里 `HAL_GetTick()` 依赖 SysTick 中断，
> 关中断期间它**不前进**；而 CYCCNT 是纯硬件计数，**关中断照跑**。这次换尺子白赚一个好处。

---

## 8. 今天的顺序（别一口气写完再编译）

```
1. 新建 dwt_us.h（两工程各一份）                    ← 纯粘贴，无风险
2. F407 侧改 freertos.c：include + dwt_init + 字段 + 计时 + 打印
3. 编译、烧录、看串口        ← 这时应该已经能看到 µs 了
4. 对照 5.1 的表，看四步是不是差得开
5. F103 侧改 modbus_slave.c：宏 + 变量 + 赋值 + 判据 + dwt_init
6. 编译、烧录、复测五步      ← 帧字节必须和昨天一模一样
7. 三件小活（TX 换行 / 大括号 / 删 defaultTask）
8. DEVLOG（卡点你自己写）+ commit + push
```

**commit message 建议**：
`perf(modbus): DWT CYCCNT 替换 ms tick，从站 t3.5 改 1750us 规范值`

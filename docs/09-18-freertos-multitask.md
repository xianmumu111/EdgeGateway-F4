# 2026-09-18 作战单：FreeRTOS 三任务解耦（CP0 收尾）

> 骨架 + 步骤。函数体留空，自己填。填完照「验收」跑一遍，用数据说话。

---

## 0. 为什么今天做这个

| 理由 | 说明 |
|---|---|
| CP0 死线 9/26 | 「环形缓冲 + 多任务跑通」，ringbuf 昨天已真机落地，**差的就是多任务** |
| 给 CP2 铺路 | MQTT 上云（10/24 死线）必须跑在独立任务里。现在不搭好，到时候就是往裸机主循环里打补丁 |
| 不依赖新硬件 | RS485 模块、SD 卡、ESP8266 都还没到，今天不会被硬件卡住 |
| 简历含金量 | 「裸机主循环」→「FreeRTOS 多任务 + 队列解耦」，这是两个档次 |

**今天结束后，CP0 就算完成。** 项目表里可以划掉一行。

---

## 1. 先补一件事（2 分钟）

昨天的 `4f96832`（.gitignore 忽略 .uvoptx）push 被代理掐了，还在本地。

```bash
git -c http.sslVerify=false -c http.sslBackend=schannel push origin main
```

报 `502` / `close_notify` / `Connection reset` 就**原样重跑一两次**，是代理抽风不是你的问题。

---

## 2. 阶段一：CubeMX 开 FreeRTOS（约 20 分钟）

### ⚠️ 开工前先备份

`cp nodes/f407_gateway/Core/Src/main.c nodes/f407_gateway/Core/Src/main.c.bak`

**原因**：切到 FreeRTOS 后 CubeMX 会**删掉 main() 里的 `while(1)`**，
写在 `/* USER CODE BEGIN 3 */ ... END 3` 里的五步轮询会一起消失。
先备份，等下从备份里把逻辑搬进任务函数。

### 配置清单

| 位置 | 改什么 | 为什么 |
|---|---|---|
| Middleware → FREERTOS | Interface = **CMSIS_V2** | V1 已废弃；V2 用 `osThreadNew` / `osMessageQueueNew` |
| System Core → SYS | Timebase Source = **TIM6** | ⚠️**必须改**。默认 SysTick 会和 FreeRTOS 抢 SysTick，CubeMX 会弹 warning。改完 `HAL_Delay`/`HAL_GetTick` 由 TIM6 供时基，FreeRTOS 继续用 SysTick |
| Config parameters | `configUSE_PREEMPTION` = Enabled | |
| Config parameters | `configCHECK_FOR_STACK_OVERFLOW` = **2** | 配 `vApplicationStackOverflowHook`，栈炸了能看见，不是默默跑飞 |
| Config parameters | `configUSE_IDLE_HOOK` = Disabled | |
| Config parameters | `configUSE_TICKLESS_IDLE` = Disabled | 以后要做低功耗再开，现在开会让 `HAL_GetTick` 计时失真 |
| Config parameters | `configTOTAL_HEAP_SIZE` = 15360（默认） | 够用 |
| Tasks and Queues | 建 3 个任务，见下表 | |

| 任务名 | 优先级 | 栈 (words) | 干什么 |
|---|---|---|---|
| `taskModbus` | osPriorityNormal | **512** | 5 步轮询 + 收发 + 组包，结果丢进队列 |
| `taskReport` | osPriorityNormal | **512** | 从队列取，打印。**以后 MQTT 就写在这** |
| `taskLed` | osPriorityLow | **128** | 绿灯心跳，肉眼证明调度器活着 |

⚠️ 栈给 512 的理由：`uprintf` 走 `vsnprintf`，一次能吃掉 1KB 以上栈。
128 words = 512 字节，打一行日志就溢出。

### Generate 之后必做的两件事（每次 Generate 都要做，记忆里的老坑）

`.mxproject` 的 `HeaderPath` 只含 CubeMX 自己生成的路径，
**Generate 重写 `.uvprojx` 时手动加的东西会被冲掉**：

1. 文件组重新 Add：`mb_master.c` / `crc16.c` / `mb_port_f4.c` / `ringbuf.c`
2. Include Paths 重新加：`..\..\..\Drivers\modbus` / `..\..\..\Drivers\crc16` / `..\..\..\Drivers\ringbuf`

加完先编译一次（此时还没写业务代码），确认 **0 Error**，再往下走。

---

## 3. 阶段二：任务之间怎么传数据

采集任务和上报任务之间用**消息队列**，元素是一个结构体：

```c
typedef struct {
    uint8_t   step;
    uint8_t   slave;
    uint16_t  addr;        /* 本次访问的起始寄存器地址 */
    mb_m_err_t err;        /* MB_M_OK / MB_M_EXCEPTION / MB_M_TIMEOUT ... */
    uint8_t   exc;         /* 从站异常码，仅 err == MB_M_EXCEPTION 时有效 */
    uint32_t  ms;          /* 本步耗时 */
    uint8_t   tx_len;
    uint8_t   rx_len;
    uint8_t   tx[8];       /* 定长内嵌，不放指针 */
    uint8_t   rx[16];
    uint16_t  n;           /* 解析出的寄存器个数 */
    uint16_t  val[8];
} mb_sample_t;
```

### ★ 为什么传结构体（值拷贝），不传指针

队列传指针的话，指针指向的是**采集任务栈上的局部变量**。
生产者 `osMessageQueuePut` 完就 `osDelay` 让出 CPU，那个栈帧的内容随时可能被改写，
消费者取到指针时读到的已经是别人的数据 —— 经典野指针，而且**时好时坏最难查**。

值拷贝多花几十字节，换来「拿到手的数据一定是发出去那一刻的快照」。
**这是面试能讲的一条：嵌入式里用空间换确定性。**

---

## 4. 阶段三：骨架（写在 `freertos.c` 的 USER CODE 块里）

⚠️ 必须写在 `/* USER CODE BEGIN xxx */` 和 `/* USER CODE END xxx */` **之间**，
CubeMX  regenerate 时这些块会被保留，写在外面会被冲掉。

### 4.1 顶部（`USER CODE BEGIN Includes`）

```c
#include "mb_master.h"
#include "mb_port_f4.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
```

### 4.2 全局（`USER CODE BEGIN Variables`）

```c
typedef struct { /* ... mb_sample_t，见第 3 节 ... */ } mb_sample_t;

static osMessageQueueId_t qSample = NULL;
static uint32_t g_dropped = 0;      /* 队列满被丢掉的条数 */
```

### 4.3 uprintf（`USER CODE BEGIN 0`）

从 `main.c.bak` 里把 `uprintf` 整段搬过来（`stdarg + vsnprintf + HAL_UART_Transmit`）。
**删掉 main.c 里那份**，避免两份实现。

> 取舍：也可以新建 `log.c/log.h` 做成模块。今天先放 `freertos.c` 里，
> 少一个文件就少一次改 Keil 工程组；等 MQTT 任务也要打印时再抽出去。

### 4.4 队列创建（`MX_FREERTOS_Init` 的 `USER CODE BEGIN Init` 块）

```c
qSample = osMessageQueueNew(8U, sizeof(mb_sample_t), NULL);

/* TODO-9: 返回值为什么要判空？创建失败你怎么让人知道？ */
```

### 4.5 采集任务

```c
void StartTaskModbus(void *argument)
{
    (void)argument;
    uint8_t  step = 0;
    uint8_t  req[8];
    uint8_t  rsp[256];
    uint16_t out[8];

    mb_port_init();     /* ⚠️ 只在这里调一次。main.c 里那句删掉 */

    for (;;)
    {
        mb_sample_t s;

        memset(&s, 0, sizeof(s));
        /* TODO-1: 为什么要 memset？不清零会出什么事？
           提示：tx/rx/val 是定长数组，本帧只填了前几个字节 */

        s.step = step;

        /* TODO-2: 按 step 组帧。逻辑从 main.c.bak 的 switch(g_step) 搬过来，
           但只做两件事：填 s.slave / s.addr，把帧填进 s.tx 并记 s.tx_len。
           不在这里打印。 */

        /* TODO-3: 计时 + mb_port_transfer + mb_master_parse。
           err / exc / ms / rx / rx_len / n / val 全部填进 s。
           计时用 HAL_GetTick()（现在由 TIM6 供时基，仍然准）。 */

        /* TODO-4: 入队。
           if (osMessageQueuePut(qSample, &s, 0U, 0U) != osOK) { g_dropped++; }
           为什么超时写 0（不等）而不是 osWaitForever？
           提示：采集任务有时间纪律，宁可丢一条也不能卡死整条总线轮询。 */

        step = (uint8_t)((step + 1u) % 5u);
        osDelay(500);
    }
}
```

### 4.6 上报任务

```c
void StartTaskReport(void *argument)
{
    (void)argument;
    mb_sample_t s;

    for (;;)
    {
        /* TODO-5: osMessageQueueGet(qSample, &s, NULL, osWaitForever) */

        /* TODO-6: 打印。格式沿用 main.c.bak，两点改动：
           (a) 前缀统一成 [RPT][S%d]
           (b) TX 字节之间补空格 —— 昨天那个小瑕疵顺手修掉
               期望：[RPT][S0] TX: 01 03 00 00 00 02 C4 0B */

        /* TODO-7: 留一行注释占位：
           "CP2: MQTT publish 就写在这里 —— 采集侧一个字都不用改"
           这就是分层 + 队列带来的回报，先占好位置。 */
    }
}
```

### 4.7 LED 心跳

```c
void StartTaskLed(void *argument)
{
    (void)argument;
    for (;;)
    {
        /* TODO-8: HAL_GPIO_TogglePin(GPIOF, GPIO_PIN_10);  osDelay(500);
           绿灯 PF10（低电平点亮）。
           思考：为什么要单独开一个任务干这事？
           答：它是"调度器还活着"的肉眼证据 —— 
           哪天程序卡死在某个阻塞调用里，灯就停了，一眼看出来。 */
    }
}
```

### 4.8 栈溢出钩子

```c
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask; (void)pcTaskName;
    taskDISABLE_INTERRUPTS();
    HAL_GPIO_WritePin(GPIOF, GPIO_PIN_9, GPIO_PIN_RESET);   /* 红灯常亮 = 栈炸了 */
    for (;;) { }
}
```

⚠️ 这里**不能用 `HAL_Delay`**：中断关了以后 TIM6 时基进不来，`HAL_Delay` 永远不返回。
只能死等。用红灯当「墓碑」。

---

## 5. 阶段四：验收（跑不出来就等于没做）

| # | 检查项 | 期望 |
|---|---|---|
| 1 | 命令行编译 | `0 Error(s), 0 Warning(s)` |
| 2 | S0 帧 | `[RPT][S0] TX: 01 03 00 00 00 02 C4 0B` / RX `01 03 04 00 64 00 C8 BA 7A`，`reg[0]=100 reg[1]=200` |
| 3 | 五步耗时 | S0 约 21~23ms，S3 约 17~18ms，**S4 约 208ms**。和昨天 DMA 版对齐 |
| 4 | 前缀 | 每一行都以 `[RPT]` 开头 → 证明**确实绕了队列**，不是采集任务直接打印 |
| 5 | 绿灯 | PF10 以 500ms 稳定闪烁 |
| 6 | 连续跑 5 分钟 | S0→S4 循环不断，无乱码、无丢行、无卡死 |

### ★ 第 7 项：故意把队列缩到 1，验证丢包分支不是摆设

把 `osMessageQueueNew(8U, ...)` 改成 `1U`，然后**把 `osDelay(500)` 临时改成 `osDelay(0)`**
（采集疯跑，上报跟不上），跑 30 秒，把 `g_dropped` 打出来。

- 若 `g_dropped > 0` → 丢包逻辑真的在跑 ✅
- 若 `g_dropped == 0` → 说明你的 `TODO-4` 判断写错了，回去查

**验完改回 8 和 500。** 这招和变异测试一个道理：
**主动制造故障，确认自己写的防御代码真的会亮。**

---

## 6. 坑清单（照着对一遍）

| 坑 | 症状 | 解法 |
|---|---|---|
| Timebase 没改 TIM6 | CubeMX 警告；`HAL_Delay` 和 RTOS tick 打架，延时乱跳 | SYS → TIM6 |
| 任务栈太小 | 打日志时 HardFault | 512 words 起步；开 `configCHECK_FOR_STACK_OVERFLOW=2` |
| 在 DMA 回调里 uprintf | 打印半行、丢数据、中断里阻塞 | 回调只做 `rb_write` + 重启 DMA（你已经做对了） |
| uprintf 两份实现 | 行为不一致、Flash 多占 | main.c 里那份删掉 |
| `mb_port_init()` 调两次 | DMA 重启、ringbuf 状态乱 | 只在 `StartTaskModbus` 开头调一次 |
| 中断里调 RTOS API | HardFault / assert | 优先级必须 ≥ `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`（默认 5）。今天还没用到，做 MQTT 时会碰到 |
| Generate 后文件组/包含路径被冲 | 一堆 `undefined symbol` | 每次 Generate 后重新 Add 并重加 IncludePath |

---

## 7. 今天做完要落的盘

- `DEVLOG.md` 三行：**今天 / 卡点 / 明天** —— 卡点那行必须自己写，
  写"没遇到"也比空着强，空着等于没复盘
- 一个 commit：`feat(f407): FreeRTOS 三任务 + 队列解耦（CP0 完成）`
- push（代理抽风就重试）

---

## 8. 明天往后看

- RS485 模块到货 → 换真 A/B 差分，**第一次真正验证 PG8 方向控制**
  （TTL 直连时 SP3485 被旁路，DE 写反也照样通）
- IDLE = 1 字符 < Modbus 要求的 3.5 字符 → 用 TIM 做 t3.5 超时兜底
- `modbus_slave.c:37` 高波特率分支 t3.5 写死 2ms，规范是 1750µs
- 两套从站实现（`Drivers/modbus/modbus_rtu.c` vs `nodes/f103_slave/.../modbus_slave.c`）决定合并还是删

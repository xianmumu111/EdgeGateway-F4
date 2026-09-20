# RTOS 可观测性：栈水位 + CPU 占用率 —— 2026-09-19 晚

> 目标：把「栈给 512 应该够吧」变成「高水位实测剩余 xxx words」。
> 不需要新硬件，且**正好把今天上午装的 DWT 用第二次**。
> 这是 CP0 的最后一块 —— 之前只证明了"能跑"，这次证明"跑得有多余量"。

---

## 0. 为什么值得花这一小时

**面试官问「你栈为什么给 512 words」，两种答法：**

| 答法 | 分量 |
|---|---|
| 「`uprintf` 走 `vsnprintf` 比较吃栈，我怕不够就给了 512」 | 靠猜 + 靠怕 |
| 「我实测过**栈高水位**，峰值用了 xxx words，512 留了 yy% 余量」 | **有数据** |

第二种才是工程师的答案。而且它顺带回答另一个问题：

> **「你的系统还剩多少算力给 MQTT / SD 卡 / OTA？」**
> 看下 IDLE 任务占比就知道了。这个数字现在是空的，CP2 排期靠拍脑袋。

顺带补上 9/18 作战单里**一直没做的验收第 7 项**（队列丢包计数）。

---

## 1. 栈水位（`uxTaskGetStackHighWaterMark`）

### 1.1 前提：**已经开好了，不用改配置**

`FreeRTOSConfig.h:117` → `#define INCLUDE_uxTaskGetStackHighWaterMark 1` ✅

### 1.2 API

```c
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask);
/* 传 NULL = 查自己 */
```

**返回值 = 历史最小剩余量**（不是已用量）。

> ⚠️ **单位是 words（`StackType_t`），不是 bytes。**
> Cortex-M 上 1 word = 4 bytes。所以返回值 300 = 还剩 1200 字节。
> 这是最常搞错的一点 —— 搞错了会以为栈还剩 300 字节，实际剩 1200。

**"高水位"这个名字的来源**：栈是从高地址往低地址长的，用得越多水位越低。
所以**历史上最低的那次剩余 = 峰值用量**。

### 1.3 在哪打印

在 `taskReport` 里加一个计数器，每 20 条打一次（不要每条都打，会刷屏）：

```c
/* TODO-1: 在 taskReport 里加 static 计数，每 20 条触发一次下面的打印。
   思考：为什么不要放在 taskModbus 里？
   提示：taskModbus 只干活不打印，这是架构约定。别破坏它。 */

uprintf("[STAT] stack hwm (words): modbus=%lu report=%lu led=%lu\r\n",
        (unsigned long)uxTaskGetStackHighWaterMark(taskModbusHandle),
        (unsigned long)uxTaskGetStackHighWaterMark(taskReportHandle),
        (unsigned long)uxTaskGetStackHighWaterMark(taskLedHandle));
```

> `taskModbusHandle` / `taskReportHandle` / `taskLedHandle` 这三个全局变量
> CubeMX 已经在 `freertos.c` 顶部声明好了，直接用。

### 1.4 怎么判读

| hwm 值 | 结论 |
|---|---|
| 接近 0（< 20） | **危险**，下次就该 HardFault 了，赶紧加栈 |
| 剩余 30%~50% | 健康，有突发余量 |
| 剩余 > 70% | **给多了**，可以缩栈省 RAM |

---

## 2. CPU 占用率（`vTaskGetRunTimeStats`）

### 2.1 ★ 时基用 DWT —— 今天上午装的，现在派上第二个用场

FreeRTOS 的 run-time stats 需要一个**比 tick 快 10~100 倍**的自由运行计数器。
标准做法是开一个 TIM；**你有现成的 CYCCNT，不用占任何定时器**。

```c
/* 1 MHz 计数（1 µs），168MHz 下 = CYCCNT / 168 */
uint32_t ulGetRunTimeCounterValue(void)
{
    return DWT->CYCCNT / (SystemCoreClock / 1000000UL);
}
```

> **为什么除以 168 而不是直接用 CYCCNT？**
> TODO-2：想想 32 位 CYCCNT 在 168MHz 下多久溢出？1MHz 呢？
> （答案在本文第 4 节，但先自己算 —— 你已经见过这个数字两次了）

### 2.2 改 `FreeRTOSConfig.h`（`USER CODE BEGIN Defines` 区块，**Generate 不会冲掉**）

```c
/* USER CODE BEGIN Defines */
#define configGENERATE_RUN_TIME_STATS            1
#define configUSE_STATS_FORMATTING_FUNCTIONS     1

extern void vConfigureTimerForRunTimeStats(void);
extern uint32_t ulGetRunTimeCounterValue(void);

#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()  vConfigureTimerForRunTimeStats()
#define portGET_RUN_TIME_COUNTER_VALUE()          ulGetRunTimeCounterValue()
/* USER CODE END Defines */
```

> `configUSE_TRACE_FACILITY` 已经是 1 了（`:73`），不用改。

### 2.3 在 `freertos.c`（`USER CODE BEGIN 4`，即钩子函数那块）实现两个函数

```c
void vConfigureTimerForRunTimeStats(void)
{
    dwt_init();          /* 幂等，重复调无害 */
    DWT->CYCCNT = 0;     /* 从调度器启动这一刻开始算 */
}

uint32_t ulGetRunTimeCounterValue(void)
{
    return DWT->CYCCNT / (SystemCoreClock / 1000000UL);
}
```

`vConfigureTimerForRunTimeStats()` 由 `vTaskStartScheduler()` **自动调用**，你不用手动调。

⚠️ **坑**：`dwt_init()` 会**清零 CYCCNT**。你上午在 `MX_FREERTOS_Init` 里已经调过一次，
这里再调一次等于重新开始计时 —— 对 elapsed 测时**没有影响**（差值法），但要知道有这件事。

### 2.4 打印 —— ⚠️ 这里有个真坑

```c
static char s_stats_buf[512];

static void print_run_time_stats(void)
{
    vTaskGetRunTimeStats(s_stats_buf);
    uprintf("%s\r\n", s_stats_buf);
}
```

**❌ 上面这样写会截断。** 为什么？

> TODO-3：`uprintf` 内部 `char buf[128]`。而 `vTaskGetRunTimeStats` 的输出有多长？
> 三个任务 + IDLE + 表头，每行 ~50 字符 → 总共 **200~300 字节**。
> `vsnprintf` 会截断到 128，你只能看到表头和第一行的一半。

**两种改法，选一个：**

- **A（快）**：新写一个 `uprintf_raw(const char *s)`，直接 `HAL_UART_Transmit(&huart1, s, strlen(s), 100)`，不走 `vsnprintf`
- **B（正统）**：把 `uprintf` 的 `buf[128]` 改成 512，代价是**每次调用多吃 384 字节栈**（会影响 hwm 读数！）

> TODO-4：为什么 B 会影响 hwm 读数？这算不算"观测行为改变了被观测对象"？
> （提示：量子力学那只猫。软件里这叫 **observer effect**。
> 想清楚这个，你就知道为什么生产环境的 profiling 都要单独分配内存）

### 2.5 输出长什么样

```
Task            Abs Time      %Time
*******************************************
taskModbus      12345         <1%
taskReport      3456          <1%
taskLed         12            <1%
IDLE            9876543       99%
```

⚠️ **FreeRTOS 10.3.1 有个已知毛病**：占比用整数算，小于 1% 的任务会显示 `<1%`。
这不是你配错了，别去查。想知道精确值就自己算 `abs_time / total`。

---

## 3. 验收第 7 项（9/18 欠到现在，今天补上）

**主动制造故障，确认防御代码真的会亮。**

| 步骤 | 期望 |
|---|---|
| ① 队列长度从 8 改成 **1** | — |
| ② `osDelay(500)` 临时改成 **0** | 采集全速跑，上报跟不上 |
| ③ 跑 10 秒 | `s_dropped` **必须 > 0** |
| ④ 改回 8 / 500 | `s_dropped` 停止增长 |

如果 ③ 的 `s_dropped` 还是 0，说明你的丢包计数**根本没生效** —— 这段防御代码等于没写。

> 这跟变异测试是一个道理：**没验证过的防御代码等于没有防御代码。**

现在 `s_dropped` 只在 `taskModbus` 里自增，但**从来没有地方打印它**。
先加一行打印，不然你根本看不到它。

---

## 4. 三个数字（自己先算，再往下看）

| 量 | 值 |
|---|---|
| 32 位 CYCCNT @168MHz 溢出周期 | **25.57 秒** |
| 32 位 1MHz 计数器溢出周期 | **4294 秒 ≈ 71.6 分钟** |
| 你的统计周期 | 10 秒 |

⇒ 直接用 CYCCNT 会有溢出风险（统计跑超过 25 秒就废了），除以 168 变成 µs 后**可以用 71 分钟**，够了。

**这跟 ringbuf 的 head/tail、跟今天的 `dwt_elapsed_us` 是同一个套路 —— 第三次出现了。**

---

## 5. 顺带把欠账清了（都是 5 分钟的事）

- [ ] `freertos.c:334` 的 `if` 加大括号
- [ ] 删掉 `defaultTask`（CubeMX → Tasks and Queues → 删行；别忘了 `defaultTaskHandle` 和 `.ioc` 同步）
- [ ] `.ioc` 里 `configMAX_PRIORITIES` **56 → 8**（3 个任务够了；调度器找最高优先级要扫这个数）
- [ ] 两个 `dwt_us.h` 还是未跟踪状态，**收工前 `git add`**

---

## 6. 收工清单

```
1. FreeRTOSConfig.h 的 USER CODE BEGIN Defines 加 4 个宏
2. freertos.c 实现两个函数（USER CODE BEGIN 4）
3. taskReport 每 20 条打印 hwm + 每 100 条打印 run-time stats
4. 编译 → 烧录 → 看输出
5. 验收第 7 项（队列缩到 1 + osDelay(0)，确认 s_dropped > 0）
6. 清欠账 4 项
7. DEVLOG（卡点自己写）+ git add（含 dwt_us.h）+ commit + push
```

**commit 建议**：
`feat(f407): RTOS 可观测性 —— 栈高水位 + DWT 时基的 CPU 占用率统计`
`perf(f407): configMAX_PRIORITIES 56→8，删 defaultTask`

---

## 7. 做完之后回答我三个问题

1. **taskModbus 的 hwm 是多少 words？** 512 给多了还是给少了？
2. **IDLE 占比是多少？** 剩下的算力够 CP2（MQTT）+ CP3（SD 卡断网续传）用吗？
3. **为什么 `vTaskGetRunTimeStats` 的时基不能用 `HAL_GetTick()`？**

第三题答案想清楚，你就明白"可观测性工具本身的精度必须高于被观测对象"这件事了。

---

# 8. ⚠️ 实测踩坑（18:48）：DWT CYCCNT 作 run-time stats 时基会炸

## 8.1 现象

```
taskReport     	12671987		658%
taskLed        	4118		<1%
IDLE           	434900088		22588%     ← 百分比总和远超 100%
taskModbus     	38243636		1986%
defaultTask    	1826243		94%
Tmr Svc        	6		<1%
```

## 8.2 根因：分母用的是「绝对当前值」，而 CYCCNT 25.57 秒就回绕

扒 `tasks.c` 源码，`vTaskGetRunTimeStats` 的分母来自：

```c
uxArraySize = uxTaskGetSystemState( pxTaskStatusArray, uxArraySize, &ulTotalTime );
ulTotalTime /= 100UL;
ulStatsAsPercentage = pxTaskStatusArray[x].ulRunTimeCounter / ulTotalTime;
```

而 `uxTaskGetSystemState()` 里：

```c
*pulTotalRunTime = portGET_RUN_TIME_COUNTER_VALUE();   /* ← 当前时基的「瞬时值」，不是各任务累加和 */
```

**分母 = 系统从上电到现在的总时长**（前提：时基单调不回绕）。
**分子 = 各任务累计运行时间**。

CYCCNT @168MHz 回绕周期 = 2^32 / 168e6 = **25.5653 秒**。跑过这个时间后：

- **分母** 跳回一个很小的值 → 百分比爆炸
- **分子** 也被污染：`counter += (uint32_t)(now_us - switchedIn_us)`，
  而 µs 值在 **25565282** 处回绕（不是 2^32！）→ uint32 减法**失效**，
  一次跨回绕的切换会凭空加出 ~4.27e9 µs

## 8.3 用数据反推验证（四个任务互相印证）

| 任务 | counter | 显示% | 反推分母 |
|---|---|---|---|
| taskReport | 12,671,987 | 658% | 1,925,834 |
| IDLE | 434,900,088 | 22588% | 1,925,359 |
| taskModbus | 38,243,636 | 1986% | 1,925,661 |
| defaultTask | 1,826,243 | 94% | 1,942,812 |

四个数一致（差异仅来自百分比向下取整），分母 ≈ **1.93 秒**。

而采样 100 条 × 500ms = 50 秒。回绕次数反推：

```
回绕 1 次 → 27.50 秒
回绕 2 次 → 53.06 秒   ← 与「50 秒采样 + 启动」吻合 ✅
回绕 3 次 → 78.63 秒
```

## 8.4 ★ 为什么 `dwt_elapsed_us()` 用同一个 CYCCNT 却没事

| 用法 | 取的是 | 回绕安全？ |
|---|---|---|
| `dwt_elapsed_us()` 测时间差 | **裸 CYCCNT**，回绕点在 2^32 | ✅ `(uint32_t)(now-last)` 天然正确 |
| run-time stats 时基 | **CYCCNT/168** 的绝对当前值 | ❌ 回绕点在 25565282，且分母用绝对值 |

**同一个计数器，两种用法，一个对一个错** —— 区别就在于**差值 vs 绝对值**。
这和 ringbuf 的 head/tail 是同一个套路：**只有在 2^32 处自然回绕的裸计数器，
无符号减法才成立；一旦除以常数，回绕点就变了，套路失效。**

## 8.5 修复方案

### ❌ 方案 C（最容易想错，无效）：把时基分频得更低

```c
return DWT->CYCCNT / (SystemCoreClock / 10000UL);  /* 100µs 分辨率 */
```

**没用。** CYCCNT 的回绕周期恒为 25.5653 秒，**与除数无关** —— 除数只改变
商的峰值（25,569,571 → 255,652），不改变 CYCCNT 多久转一圈。

### ✅ 方案 A（推荐）：换 32 位定时器 TIM2/TIM5 @1MHz

F407 的 **TIM2 / TIM5 是 32 位**（TIM3/TIM4 是 16 位，别选错）。
回绕周期 = 2^32 µs = **4294 秒 ≈ 71.6 分钟**，远大于任何统计周期。

步骤：
1. CubeMX → TIM2 → Clock Source = Internal Clock
2. Prescaler：APB1 定时器时钟 84MHz → **PSC = 83**（得 1MHz）
3. Counter Period = **0xFFFFFFFF**（拉满 32 位）
4. Generate 后补文件组 / IncludePath / HSE_VALUE
5. 改 `freertos.c`：

```c
void vConfigureTimerForRunTimeStats(void)
{
    __HAL_TIM_SET_COUNTER(&htim2, 0);
    HAL_TIM_Base_Start(&htim2);          /* 注意：不开中断，只跑计数器 */
}
uint32_t ulGetRunTimeCounterValue(void)
{
    return __HAL_TIM_GET_COUNTER(&htim2);   /* 32 位，71.6 分钟才回绕 */
}
```

⚠️ SYS Timebase 已经占了 **TIM6**，别动它；TIM2 是另一个，不冲突。

### ✅ 方案 B（不占硬件，但要自己算）：裸 CYCCNT + 差值法

时基改成返回**裸 CYCCNT**（不除 168）→ 分子的 uint32 减法回绕安全。
但分母问题仍在，所以**必须弃用 `vTaskGetRunTimeStats`**，改用：

```c
uxTaskGetSystemState(arr0, n, &t);        /* 第一次采样 */
vTaskDelay(pdMS_TO_TICKS(10000));         /* 等 10 秒 */
uxTaskGetSystemState(arr1, n, &t);        /* 第二次采样 */
uint32_t d = (uint32_t)(arr1[i].ulRunTimeCounter - arr0[i].ulRunTimeCounter);
pct = d * 100U / total_delta;             /* total_delta = 各任务 d 之和 */
```

两次采样间隔必须 **< 25.57 秒**（counter 的回绕周期）。

### 🕐 临时验证法（今天就能拿到可信数据）

分母只在**上电后 25.57 秒内**正确。把打点从 `% 100U` 改成 `% 20U`（10 秒一次），
看**前两次**读数（10 秒 / 20 秒处）—— 这两组是可信的。

## 8.6 顺带：hwm 读数会随时间下降

第二次读数 `led=97`，第一次 `led=105`。hwm 是**历史最小剩余**，只会降不会升。
说明第一次（10 秒时）还没覆盖最坏路径 —— **栈水位要跑够久才可信**，
尤其要让 S4 超时、异常帧等分支都跑到。

---

# 9. ✅ 19:10 拿到可信数据（打点改 `% 20U`，10 秒读数）

```
taskReport     	267463		2%
taskLed        	87		<1%
IDLE           	9194395		88%
taskModbus     	849671		8%
defaultTask    	41385		<1%
Tmr Svc        	6		<1%
```

## 9.1 可信性验证（先验证，再采信）

| 任务 | 显示% | 反推分母区间（µs） |
|---|---|---|
| taskReport | 2% | [8,915,433, 13,373,150] |
| IDLE | 88% | [10,330,781, 10,448,176] |
| taskModbus | 8% | [9,440,789, 10,620,888] |

三区间交集 = **[10.33, 10.45] 秒**，与各任务 counter 之和 10.353 秒差 **0.22%**
（中断不计入任务 + 百分比取整误差）。

**分母 10.4 秒 < 25.5653 秒回绕周期 → 未回绕，数据可信** ✅

## 9.2 CPU 占用（用 counter 之和作分母，比取整过的百分比准）

| 任务 | 占用 |
|---|---|
| **IDLE** | **88.81%** |
| taskModbus | 8.21% |
| taskReport | 2.58% |
| defaultTask | 0.40% |
| taskLed / Tmr Svc | ~0% |

## 9.3 ★ 这 11.2% 忙的是什么？—— 几乎全是忙等，不是计算

**taskModbus 的 8.21% 拆解**（用 115200 实测五步耗时）：

```
一轮周期 = Σ(3.368+3.267+3.192+3.013+200.352) + 5×500 = 2713.2 ms

  S4 掉线超时（忙等）   200.4 ms  → 占 CPU 7.38%   ← 90%
  S0~S3 真实通信        12.8 ms   → 占 CPU 0.47%   ←  6%
```

**8.21% 里 90% 是在等一个不存在的从站（slave=02）** —— S4 是测试代码人为设计的
掉线场景，真实系统从站全在线时这 200ms 根本不存在。

**taskReport 的 2.58%**：`uprintf` 用 `HAL_UART_Transmit` **阻塞发送**，
每 500ms 打 ~160 字节 @115200 → 阻塞 ~14ms → 约 2.6%，CPU 空转等移位寄存器。

> **真正做计算的 < 0.5%。**

## 9.4 结论：算力完全不是瓶颈

**IDLE 88.8%，剩余算力 ~89%。** CP2（MQTT）/ CP3（SD 卡断网续传）/ CP4（OTA）
在算力上**随便加**。

**现在不需要优化。** 等 CP2 加了任务，若发现 MQTT 心跳被 Modbus 的 200ms 忙等
卡住（表现为心跳延迟 / 超时重传），再改：

1. `mb_port_transfer` 的等待循环用 `osDelay(1)` 代替忙等
   → taskModbus 占用 8.2% → ~0.5%；代价是响应最多延迟 1ms（t3.5=1.75ms，可接受）
2. `uprintf` 改 DMA 发送 → taskReport 占用 2.6% → ~0.1%

⚠️ 样本量提醒：这次只跑了 10.4 秒，S4 只经历约 4 次。建议看第二次读数（20 秒处）
确认稳定。**栈水位同理，要跑够久才可信。**

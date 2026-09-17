# DEVLOG · EdgeGateway-F4

> 每天收工写「断点三行」：今天 / 卡点 / 明天。
> 写它的唯一目的：时间不固定，下次坐下时不用花 20 分钟回忆自己做到哪儿了。

---

## 2026-09-13（周日）

**今天**
- 完成 `ringbuf.c` —— SPSC 无锁环形缓冲，参照 Linux kfifo 的「单调 head/tail + 掩码」方案，
  容量必须是 2 的幂，size 个格子能装 size 个字节（一格不浪费）。
- gcc 单元测试 `test_ringbuf.c`：99 项检查全部通过，`-Wall -Wextra` 零警告。
  含 1MB 数据流过 1KB 缓冲的零丢帧验证（47.5 字节/轮，共 22067 轮）。
- 顺手把仓库结构理顺：`Drivers/crc16/`、`Drivers/ringbuf/`、`test/`，干掉双目录分裂。
- 修了 `test_crc.c` 里的 `ok &= check(...)` —— 这个 bug 会让测试**永不失败**。
- git 首次提交，`feat(init)`。提交记录的时间跨度从今天开始算。

**卡点**
- Git Bash 又坏了（`ls` / `gcc` 都 command not found）。解药：`export PATH="/usr/bin:/bin:$PATH"`。
  不是环境真坏，是 PATH 被 shim 冲掉了。以后每次用 Bash 第一行就写这句。
- `cmd //c build.bat` 在本会话里跑不动，只能用 bash 逐条 gcc 验证；
  build.bat 留给用户自己双击。
- 第一版测试写完是「全绿」，但**全绿可能意味着测试是死的**。所以补了变异测试：
  故意把实现改坏 3 处（容量计算失效 / 跨回绕拷贝丢失 / head 每次 +2），
  3 次测试都红了才算数。

**明天**
- `modbus_rtu.c`：ADU 组包 / 解包 + 请求-响应配对的状态机。
- ringbuf 已经能用了：ISR 里 `rb_write`，任务里 `rb_peek` 先看够不够一帧长，够才 `rb_read`。
- 板子预计 9/16~9/19 到。到之前把三件套写完，到之后直接移植到 Keil。

---

## 三件套进度

| 模块 | 状态 | 位置 |
|---|---|---|
| `crc16.c` | ✅ 手写完成，gcc 实测 PASS | `Drivers/crc16/` |
| `ringbuf.c` | ✅ 完成，99 项单测 PASS + 变异测试验证 | `Drivers/ringbuf/` |
| `modbus_rtu.c` | ✅ **从站协议层完成，14/14 单测 + 4 变异全捕获** | `Drivers/modbus/` |
| `mb_port_f1.c` | ✅ 已写（HAL 适配层，F103 上板后用），PC 不编译 | `Drivers/modbus/` |

---

## 2026-09-13 晚 · Modbus RTU 从站协议层

**今天**
- 写完 `modbus_rtu.h/.c`：**纯协议层，零硬件依赖**，签名是
  `mb_slave_handle(slave, req, req_len, rsp, rsp_max, &rsp_len)`。
- 支持功能码 `0x03`（读保持寄存器）、`0x06`（写单个寄存器），
  异常响应 `0x83/0x86 + 异常码` 齐全；`0x10`（写多个）留作练习。
- `test/test_modbus.c` —— **14 项全过，零警告**。

**核心测试：用 Modbus 官方 spec 示例帧逐字节对拍**
```
请求: 11 03 00 6B 00 03 76 87
响应: 11 03 06 AE 41 56 52 43 40 49 AD     ← 11 字节全比对，含 CRC
```
期望值来自标准文档，不是自算自验。这是整个三件套里最有说服力的一条证据。

**变异测试（4 个全部 RED，证明测试是活的）**
| 变异 | 结果 |
|---|---|
| CRC 低高字节顺序颠倒 | RED ✅ |
| 去掉地址比对（谁都应答） | RED ✅ |
| 去掉读寄存器越界检查 | RED ✅ |
| 异常码 0x02 误写成 0x03 | RED ✅ |

**关键设计决策（面试要能讲）**
1. **CRC 错了必须沉默**，不能回异常帧 —— 帧被干扰破坏时连地址字段都不可信。
2. **地址不是自己必须沉默** —— RS485 是半双工总线，插嘴就和对面的响应撞车。
3. 主站靠"有没有回包"判超时，所以能回异常帧就不要沉默（否则一秒超时 × 从站数量）。
4. 越界检查用 `(uint32_t)start + qty > regs_n`，**防止 16 位加法溢出绕过检查**。
5. Modbus 是**大端**，STM32 是小端，所有 16 位必须走 `mb_put_be16/get_be16`。

**已知限制（诚实写下，别装完美）**
- **IDLE 中断 = 空闲 1 个字符**，而 Modbus 要 3.5 个字符。实验室够用，
  真实 RS485 现场要补 t3.5 定时器（9600bps 约 4ms）才能算工业级。
- 不支持广播写（收到广播直接静默）。
- 不支持 0x10 写多个寄存器。

**明天（F103 上板）**
1. CubeMX 建 F103C8 工程：USART1 异步 9600 + DMA Rx + 开中断，SYS=Serial Wire。
2. 把 `Drivers/` 三个模块加进 Keil 工程，include 路径加上三个目录。
3. `main.c` 里：`mb_port_init(&huart1, 1)` + 主循环 `mb_port_poll()`。
4. 在 `HAL_UARTEx_RxEventCallback` 里调 `mb_port_rx_event(huart, Size)`。
5. PC 用串口助手发 `11 03 00 6B 00 03 76 87`，看板子回 `11 03 06 AE 41 ...`。
   —— 和 PC 单测用**同一个 vector**，形成闭环。

## CP0 死线自查（9/26）

- [x] ringbuf 通过运输级压力测试（1MB 数据 / 1KB 缓冲，byte-exact 比对）
- [x] 单元测试可用且**已被证实会失败**（变异测试）
- [ ] 单测覆盖率 / cppcheck 扫描（Phase 6 加分项）
- [ ] FreeRTOS 三任务跑通 + 打印栈水位
- [ ] README 架构图
- [ ] 投第一批简历（不等这些打勾）

## 2026-09-14 · 从站协议栈打通 + PC 仿真台 + 合入 GitHub

**今天**

- 手写 0x03/0x06 解析 + 异常帧，五道校验全上（帧长 / CRC / 地址 / 参数范围 / 功能码）
- 修掉整数溢出越界：`(uint32_t)addr + qty`。`addr=0xFFFF qty=9` 攻击用例实测
  16 位版直接段错误 —— 在板子上就是 HardFault 死机
- 波特率 115200 → 9600，t3.5 改为按波特率算（35000/9600+1 = 4ms）
- PC 仿真台固化进工程 `test/`：18 条用例 + 6 个变异测试全捕获
- 整个工程合入 GitHub（90MB → 216KB，忽略 HAL/CMSIS，保留 .ioc）

**卡点**

- MinGW 的 printf 是 static inline，覆盖不了 → 编译源码要加 `-D_INC_STDIO`
- bat 必须纯 ASCII，UTF-8 中文会被 cmd 按 GBK 切成乱码命令
- git push 得用 schannel 后端（openssl 报 unexpected eof）

**明天**

1. 补 t1.5 字符间隔检测（`modbus_rx_byte` 里记 last_byte_tick，超 t1.5 就清 rx_len）
2. F407 到货后跑验片 7 步
3. 加 0x10 写多个寄存器（自己写，先在 PC 仿真台补用例）

## 2026-09-15 · 探索者到货验片：Flash 全片 + 三颗外设芯片全过，判定「好片」

**今天**

- Flash 全片测试真机跑通（`f4_flash_test`）：Sector 5~11 / 896KB / `fail=0` / 9755ms
- 自己用 CubeMX 建了 F407 HAL 工程 `f407_gateway`，
  SYS 选了 **Serial Wire**（保住 PB3/PB4 给 SPI1，选 JTAG 会废掉 W25Q128）
- 板级体检程序一次烧通：
  - W25Q128 JEDEC ID = `EF 40 18`（Winbond 16MB）
  - W25Q128 Unique ID = `D2657048271D1C26` —— 非全 00/FF，**真硅片不是白片**
  - 24C02 写读比对 OK（`A5 3C 5A C3`，测前备份测后写回，无损）
  - MPU6050 WHO_AM_I = `0x68`
  - → `RESULT : ALL OK -> GOOD BOARD`
- 时钟从 HSI 换到 **HSE 8MHz + PLL**（PLLM=8/N=336/P=2/Q=7 → 168/168/42/84M）

**卡点**

（TODO）

**明天**

（TODO）

## 2026-09-16 · Modbus 主站协议层建成 + 第一次真机对话

**今天**

- 主线工程收进 `nodes/f407_gateway`（只搬 .ioc，57MB HAL 留给 Generate 重建；编译 0 Error）
- 主站协议层 `Drivers/modbus/mb_master.c`：4 个 TODO 自己填完
  → PC 测试 **45/45 全绿**，变异测试 **6 个变异体 6 个全捕获**
- port 层 `mb_port_f4.c` 自己写出（RS485 方向控制 PG8 = RE/DE 并联，高=发低=收）
- **第一次真机对话成功**（TTL 直连 F103 从站）：
  `TX 01 03 00 00 00 02 C4 0B` → `RX 01 03 04 00 64 00 C8 BA 7A`，reg[0]=100 reg[1]=200

**卡点**

（TODO）

**明天**

（TODO）

## 2026-09-17 · 五步轮询：读/写/读回/异常/超时/掉线恢复 全路径真机验证

**今天**

- 主站改成五步 round-robin，一次烧录验完五条路径：
  - S0 读 reg0..1        OK      22ms   reg=100,200
  - S1 写 reg4=0x037F    OK      21ms   回显 895
  - S2 读回 reg4         OK      19ms   **895 —— 证明真写进去了**
  - S3 读 reg200 越界    EXC     17ms   exception 0x02
  - S4 读不存在从站 0x02 TIMEOUT 209ms
  - 回到 S0 仍 OK —— **掉线不锁总线**
- 耗时对标理论值（9600bps = 1.042ms/字节），差值稳定 3.4~4.3ms
- 整理 `docs/API-cheatsheet.md`（全项目函数速查 + 一帧 0x03 完整生命周期 + 10 题自检）
- 「讲得出」五问自测，约 63 分

**卡点**

（TODO）

**明天**

（TODO）

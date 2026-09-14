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

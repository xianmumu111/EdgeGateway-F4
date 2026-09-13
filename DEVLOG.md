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
| `modbus_rtu.c` | ⬜ 下一个 | `Drivers/modbus/`（待建） |

## CP0 死线自查（9/26）

- [x] ringbuf 通过运输级压力测试（1MB 数据 / 1KB 缓冲，byte-exact 比对）
- [x] 单元测试可用且**已被证实会失败**（变异测试）
- [ ] 单测覆盖率 / cppcheck 扫描（Phase 6 加分项）
- [ ] FreeRTOS 三任务跑通 + 打印栈水位
- [ ] README 架构图
- [ ] 投第一批简历（不等这些打勾）

# Modbus 从站 · PC 仿真台

把 `Core/Src/modbus_slave.c` **原封不动**搬到 PC 上编译运行：喂字节流进去，
检查吐出来的字节流对不对。

意义：**不用烧板子、不用接串口、不用开串口助手**，改一行代码几秒钟就知道对不对。
以后加 0x10（写多个寄存器）、加主站、改波特率，都能先在 PC 上验一遍。

---

## 1. 怎么跑

双击 `run.bat`。

或者手动（Git Bash / cmd 都行，在 `test` 目录下）：

```bash
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -finput-charset=UTF-8 \
    -Dprintf=mbtest_printf -D_INC_STDIO -I. -I../Core/Inc \
    -c ../Core/Src/modbus_slave.c -o mb.o
gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -finput-charset=UTF-8 \
    -I. -I../Core/Inc -c harness.c -o harness.o
gcc mb.o harness.o -o mbtest.exe
./mbtest.exe
```

全绿长这样（`FAIL` 数是退出码，`0` 表示全过）：

```
===============  PASS = 18   FAIL = 0  ================
```

> ⚠️ 第一行的两个宏 **只能加给从站源码**，绝不能加给 `harness.c`：
>
> - `-Dprintf=mbtest_printf` —— 把源码的调试打印接到虚拟线路上
> - `-D_INC_STDIO` —— 跳过 `stdio.h`。MinGW 的 `printf` 是 `static inline`
>   定义，一旦展开就会和 `mbtest_printf` 打架；`MODBUS_DEBUG=1` 时源码会
>   include `stdio.h`，不挡掉直接编译失败。
>
> 详细注释在 `hal_stub.h` 里。

### 顺带：这仿真台能抓到"忘了关调试开关"

把 `MODBUS_DEBUG` 改成 `1` 再跑一遍，18 条会**全部变红** —— 因为每帧前面
多出几十字节的 `RX: ...` ASCII 文本，而这正是接主站时"第一帧永远是垃圾"的原因。
改回 `0` 就全绿。不用接串口助手就能验证这件事。

---

## 2. 文件都是干嘛的

| 文件 | 作用 | 能动吗 |
|---|---|---|
| `harness.c` | 虚拟串口线路 + 18 条用例 + 判定逻辑 | ✅ 加用例就改这里 |
| `hal_stub.h` | HAL 替身（`HAL_UART_Transmit` / `HAL_GetTick` / 关中断宏） | 一般不用动 |
| `usart.h` | **屏蔽层**，和 `Core/Inc/usart.h` 同名 | ❌ 删了就编译不过 |
| `run.bat` | 一键编译运行（**双击它**） | ❌ 必须纯 ASCII |
| `mutate.py` | 变异测试，检验测试本身有没有在测东西 | |
| `mutate.bat` | 一键跑变异测试（**双击它**） | ❌ 必须纯 ASCII |
| `mb.o` `harness.o` `mbtest.exe` | 编译产物 | ❌ 别提交 git |

为什么要 `usart.h` 这个屏蔽层：真实的 `Core/Inc/usart.h` 会拖进 `main.h` → 整个 HAL 库，
PC 上编译不过。编译时 `-I.` 排在 `-I..\Core\Inc` 前面，所以源码里 `#include "usart.h"`
会命中本目录下这个假货。

---

## 3. 期望值怎么写

`harness.c` 里每条用例的第二个参数就是期望：

| 写法 | 含义 |
|---|---|
| `"01 03 04 00 64 00 C8 BA 7A"` | 输出必须**逐字节完全等于**这串 |
| `"SILENT"` | 必须**一个字节都不回**（沉默，比回错更重要） |
| `"01 03 10 ."` | 带 `.` = 只验前缀，`.` 后面的不管（CRC 懒得算时用） |

---

## 4. 18 条用例覆盖了什么

```
01  读 2 个寄存器            → 正常响应
02  读 1 个寄存器            → 正常响应
03  起始地址 16 越界          → 异常码 02（非法地址）
04  数量 126 超上限          → 异常码 03（非法值）
05  CRC 改一位               → 沉默 ← 关键
06  地址 0x02 不是本机        → 沉默 ← 关键
07  非法功能码 0x04           → 异常码 01（非法功能）
08  写 regs[2] = 1000        → 原样回显
09  回读 regs[2]             → 验写入真的落进数组了（8→9 有依赖，别调换顺序）
10  短帧 0x03（4 字节）       → 沉默 ← 防止读到帧外
11  短帧 0x06（4 字节）       → 沉默
12  写越界地址               → 异常码 02
13  读全部 8 个寄存器         → 前缀匹配
14  写 regs[0] = 10          → 原样回显
15  数量 = 0                 → 异常码 03
16  溢出攻击 0xFFFF + 9      → 异常码 02 ← 最重要的一条，见下
17  慢速发送（字节间隔 5ms）   → 已知缺口，见第 5 节
18  连发两帧                 → 不串帧、状态不残留
```

**第 16 条值得单独说**：`addr = 0xFFFF, qty = 9`，16 位下 `0xFFFF + 9` 回绕成 `8`，
`8 > 8` 为假 → 越界检查被绕过 → 去读 `regs[65535]`，PC 上段错误，**STM32 上就是 HardFault**。
先把 `addr` 强转 `uint32_t` 再相加才能拦住。这是面试能讲两分钟的一段。

---

## 5. 已知缺口（用例 17）

Modbus 规范规定：字符间隔 **> 1.5 字符判帧错**、**> 3.5 字符算新帧开始**。

当前实现只在 `modbus_poll()` 里判断"**最后一个字节之后**静默了多久"，
不判断"**字节和字节之间**隔了多久"。所以主站如果发得断断续续，
本该丢弃的字节会被拼成一帧，还能正常应答。

用例 17 现在锁的是"当前实际行为"（拼起来、正常回），不是规范要求的行为。

补法（以后做）：在 `modbus_rx_byte()` 里记 `last_byte_tick`，进来先判断

```c
if (HAL_GetTick() - last_byte_tick > MODBUS_T15_MS) rx_len = 0;  /* 帧内断流 → 丢弃重来 */
last_byte_tick = HAL_GetTick();
```

补完把用例 17 的期望从 `"01 03 04 ."` 改成 `"SILENT"`。

---

## 6. 变异测试：证明绿灯不是假的

测试全绿只能说明"没发现错"，不能说明"测试有效"。
`mutate.py` 故意把源码改坏 6 处，看测试抓不抓得到：

双击 `mutate.bat`，或者命令行 `python mutate.py`。

当前结果：

```
[CAUGHT] resp byte count qty*2 -> qty           FAIL 0 -> 6
[CAUGHT] remove len!=8 length check             FAIL 0 -> 2
[CAUGHT] downgrade bounds check to 16-bit       program CRASHED (HardFault on STM32)
[CAUGHT] remove CRC check                       FAIL 0 -> 1
[CAUGHT] remove slave address filter            FAIL 0 -> 1
[CAUGHT] swap start-addr endianness             FAIL 0 -> 2

CAUGHT = 6   MISSED = 0
```

（脚本输出刻意用英文：cmd 默认按 GBK 显示，Python 打印 UTF-8 中文会变乱码。）

`MISSED` 出现就说明用例集有盲区，得补用例。**以后每加一个功能码，都跑一遍。**

---

## 7. 加新用例

在 `harness.c` 的 `main()` 里照抄一行 `feed(...)`：

```c
feed ("19 write multiple regs 0x10", "01 90 01 .", t19, 8, 1);
```

CRC 用这个算（Python）：

```python
def crc16(b):
    c = 0xFFFF
    for x in b:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c
b = bytes.fromhex("011000000002")
print("%02X %02X" % (crc16(b) & 0xFF, crc16(b) >> 8))   # 低字节在前
```

---

## 8. 翻车了怎么办

| 现象 | 原因 |
|---|---|
| `gcc 不是内部或外部命令` | MinGW 没装或没进 PATH，把 `D:\MinGW\mingw64\bin` 加进去 |
| 一堆 `stm32f1xx_hal.h: No such file` | `test/usart.h` 被删了，或者 `-I.` 没排在 `-I..\Core\Inc` 前 |
| `redefinition of 'printf'` | `-Dprintf=mbtest_printf` 加给 `harness.c` 了，只能加给从站源码 |
| `static declaration of 'mbtest_printf' follows non-static declaration` | 编译从站源码时漏了 `-D_INC_STDIO` |
| `undefined reference to 'mbtest_printf'` | 链接时没把两个 `.o` 都带上 |
| bat 一跑就报 `'xxx' 不是内部或外部命令`，乱码像 `庣珯` | **bat 文件必须是纯 ASCII**。cmd 按 GBK 读 `.bat`，UTF-8 的中文注释会被切成乱码命令。中文说明写进 README，别写进 bat |
| `python mutate.py` 输出中文乱码 | 已知，脚本输出已改成英文。看 `CAUGHT / MISSED` 标记就行 |
| `python 不是内部或外部命令` | Python 没进 PATH。本机在 `C:\Users\Administrator\.workbuddy\binaries\python\versions\3.13.12\python.exe` 和 `D:\python\python.exe` |
| 所有用例一起红，输出里还有 `RX:` | `MODBUS_DEBUG` 忘了改回 0（这正是仿真台的价值） |
| 编译报 `conflicting types` | 检查 `hal_stub.h` 里是否有人手贱 include 了 `<stdio.h>` |

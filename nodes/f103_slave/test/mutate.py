# -*- coding: utf-8 -*-
"""
变异测试（mutation testing）—— 检验"这套测试到底有没有在测东西"。

原理：
    故意把 Core/Src/modbus_slave.c 改坏一处（这叫一个"变异体"），
    重新编译跑一遍。如果测试仍然全绿，说明测试是死的 —— 漏网之鱼。
    如果测试变红（或者程序直接崩），说明这个变异体被捕获了，测试有效。

    **原文件绝对不会被修改**，改的是 test/_mut/ 下的临时副本。

用法（在 test 目录下）：
    python mutate.py

输出：
    CAUGHT  = 被抓到了，测试有效
    MISSED  = 漏网，说明你的用例集有盲区，要补用例
    INVALID = 这个变异体编译不过，是我写错了，不是你的问题
"""
import os
import re
import shutil
import subprocess
import sys

# 屏幕输出一律用 ASCII 英文：
# cmd 默认按 GBK 显示，Python 输出 UTF-8 中文会变乱码。
# 中文说明请看 README.md 第 6 节。

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "Core", "Src", "modbus_slave.c"))
INC = os.path.normpath(os.path.join(HERE, "..", "Core", "Inc"))
MUT = os.path.join(HERE, "_mut")

# (说明, 原文片段, 替换成)
MUTANTS = [
    ("resp byte count qty*2 -> qty",
     "(uint8_t)(qty * 2)", "(uint8_t)(qty)"),

    ("remove len!=8 length check",
     "if (len != 8) return;", ""),

    ("downgrade bounds check to 16-bit",
     "(uint32_t)addr + qty > REG_COUNT", "(uint16_t)(addr + qty) > REG_COUNT"),

    ("remove CRC check",
     "if(!modbus_crc_ok(req, len)) return;", ""),

    ("remove slave address filter",
     "if(req[0] != MODBUS_SLAVE_ADDRESS) return;", ""),

    ("swap start-addr endianness",
     "((req[2] << 8) | req[3])", "((req[3] << 8) | req[2])"),
]


def build_and_run(src_path, work_dir):
    """编译 + 运行，返回 (退出码, 输出文本)。退出码 -1 表示编译失败。"""
    cflags = ["-std=c11", "-w", "-finput-charset=UTF-8",
              "-I", HERE, "-I", INC]
    mb = os.path.join(work_dir, "mb.o")
    hr = os.path.join(HERE, "harness.c")
    ho = os.path.join(work_dir, "harness.o")
    exe = os.path.join(work_dir, "mbtest.exe")

    # 从站源码专用：-Dprintf=... 接调试打印，-D_INC_STDIO 挡掉 static inline 的 printf
    r = subprocess.run(["gcc"] + cflags + ["-Dprintf=mbtest_printf", "-D_INC_STDIO",
                                           "-c", src_path, "-o", mb],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        return -1, r.stderr

    r = subprocess.run(["gcc"] + cflags + ["-c", hr, "-o", ho],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        return -1, r.stderr

    r = subprocess.run(["gcc", mb, ho, "-o", exe],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        return -1, r.stderr

    r = subprocess.run([exe], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=30)
    return r.returncode, r.stdout


def main():
    if not os.path.exists(SRC):
        print("[ERROR] source file not found:", SRC)
        return 1

    with open(SRC, "r", encoding="utf-8", errors="replace") as f:
        original = f.read()

    os.makedirs(MUT, exist_ok=True)
    tmp = os.path.join(MUT, "mutant.c")

    print("\n===== mutation testing =====\n")

    # 先跑一遍原版，确认基准是绿的
    base_rc, base_out = build_and_run(SRC, MUT)
    m = re.search(r"PASS = (\d+)\s+FAIL = (\d+)", base_out)
    if not m:
        print("[ERROR] baseline does not build. Fix run.bat first.")
        shutil.rmtree(MUT, ignore_errors=True)
        return 1
    base_fail = int(m.group(2))
    print("baseline   : PASS=%s FAIL=%s\n" % (m.group(1), m.group(2)))

    caught = missed = invalid = 0
    for name, old, new in MUTANTS:
        if old not in original:
            print("  [INVALID] %-38s (pattern not found in source, mutant is outdated)" % name)
            invalid += 1
            continue

        mutated = original.replace(old, new)
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(mutated)

        try:
            rc, out = build_and_run(tmp, MUT)
        except subprocess.TimeoutExpired:
            rc, out = -99, ""

        m = re.search(r"PASS = (\d+)\s+FAIL = (\d+)", out)
        if rc < 0:
            print("  [INVALID] %-38s (does not compile)" % name)
            invalid += 1
        elif m is None:
            # 没跑到结尾 = 中途崩了（比如越界访问 -> SIGSEGV / HardFault）
            print("  [CAUGHT ] %-38s program CRASHED (exit=%d, on STM32 this is a HardFault)" % (name, rc))
            caught += 1
        elif int(m.group(2)) > base_fail:
            print("  [CAUGHT ] %-38s FAIL %s -> %s" % (name, base_fail, m.group(2)))
            caught += 1
        else:
            print("  [MISSED ] %-38s tests still all green -- your cases have a blind spot!" % name)
            missed += 1

    shutil.rmtree(MUT, ignore_errors=True)

    print("\n================  CAUGHT = %d   MISSED = %d   INVALID = %d  ================\n"
          % (caught, missed, invalid))
    if missed:
        print("Some mutants survived: add more cases in harness.c.\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

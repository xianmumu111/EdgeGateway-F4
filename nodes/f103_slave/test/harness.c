/* =========================================================================
 * harness.c —— Modbus 从站 PC 仿真台
 *
 *   把 Core/Src/modbus_slave.c 原封不动搬到 PC 上编译，
 *   喂字节流进去，检查它吐出来的字节流对不对。
 *   不用烧板子、不用接串口，几秒钟出结果。
 *
 * 编译（run.bat 里已经写好）：
 *   gcc -std=c11 -Wall -Wextra -Wno-unused-parameter -finput-charset=UTF-8 ^
 *       -I. -I..\Core\Inc ^
 *       ..\Core\Src\modbus_slave.c harness.c -o mbtest.exe
 *
 * 四条约定：
 *   1) 期望值写 "SILENT" 表示"必须一个字节都不回"
 *   2) 期望值写 hex 串，遇到 '.' 就停止比对（只验前缀，CRC 懒得算时用）
 *   3) 仿真台自己实现了 printf，源码的调试打印也进虚拟线路 ——
 *      这就是为什么 MODBUS_DEBUG 忘了关时，所有用例会一起变红
 *   4) 仿真台往屏幕输出一律走 out()/fputs，绝不用 printf（会打到自己）
 * ========================================================================= */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include "hal_stub.h"
#include "modbus_slave.h"

/* ---------------- 虚拟串口线路 ---------------- */
#define WIRE_MAX 4096
static uint8_t  wire[WIRE_MAX];
static int      wn = 0;
static uint32_t g_tick = 1000;          /* 虚拟时基，手动推进 */
static int      g_pass = 0, g_fail = 0;

UART_HandleTypeDef huart1;

uint32_t HAL_GetTick(void) { return g_tick; }

int HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *p, uint16_t n, uint32_t t)
{
    (void)h; (void)t;
    for (int i = 0; i < n; i++) if (wn < WIRE_MAX) wire[wn++] = p[i];
    return 0;                            /* HAL_OK */
}

/* 从站源码的 printf 被 -Dprintf=mbtest_printf 重定向到这里。
   调试打印同样占用 USART1，必须计入虚拟线路。 */
int mbtest_printf(const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    for (int i = 0; i < k; i++) if (wn < WIRE_MAX) wire[wn++] = (uint8_t)tmp[i];
    return k;
}

/* ---------------- 屏幕输出（不用 printf） ---------------- */
static void out(const char *s) { fputs(s, stdout); }

static void out_hex(uint8_t v)
{
    char b[8];
    snprintf(b, sizeof(b), "%02X ", v);
    out(b);
}

static void out_dec(int v)
{
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    out(b);
}

static void wire_reset(void) { wn = 0; }

static void wire_print(void)
{
    if (wn == 0) { out("(silent)"); return; }
    for (int i = 0; i < wn; i++) out_hex(wire[i]);
}

/* 解析期望 hex 串 -> 字节数组。遇到非 hex 字符（'.'）停止，返回已解析长度 */
static int parse_hex(const char *s, uint8_t *outbuf, int max)
{
    int n = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '.') break;          /* 通配符：到此为止，只验前缀 */
        unsigned v;
        if (sscanf(s, "%2X", &v) != 1) break;
        if (n >= max) break;
        outbuf[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

/* 把一帧喂进去。gap_ms = 字节之间的间隔（毫秒） */
static void pump(const uint8_t *f, int n, int gap_ms)
{
    for (int i = 0; i < n; i++) { modbus_rx_byte(f[i]); g_tick += gap_ms; }
    g_tick += 10;                        /* 帧后静默，超过 t3.5 */
    modbus_poll();
}

static int cmp_wire(const char *exp)
{
    if (strcmp(exp, "SILENT") == 0) return (wn == 0);

    uint8_t e[64];
    int en = parse_hex(exp, e, 64);
    int wildcard = (strchr(exp, '.') != NULL);   /* 带 '.' = 只验前缀 */

    if (wildcard) { if (wn < en) return 0; }
    else          { if (en != wn) return 0; }

    for (int i = 0; i < en; i++) if (e[i] != wire[i]) return 0;
    return 1;
}

static void report(const char *name, const char *exp, int ok)
{
    out(ok ? "[PASS] " : "[FAIL] ");
    out(name);
    out("\n         out = ");
    wire_print();
    if (!ok) { out("\n         exp = "); out(exp); }
    out("\n");

    if (ok) g_pass++; else g_fail++;
}

/* 单帧用例 */
static void feed(const char *name, const char *exp, const uint8_t *f, int n, int gap_ms)
{
    wire_reset();
    modbus_init();
    pump(f, n, gap_ms);
    report(name, exp, cmp_wire(exp));
}

/* 连发两帧：验证不串帧、状态不残留 */
static void feed2(const char *name, const char *exp,
                  const uint8_t *a, int an, const uint8_t *b, int bn)
{
    int ok = 1;
    wire_reset();
    modbus_init();
    pump(a, an, 1);
    if (!cmp_wire(exp)) ok = 0;
    wire_reset();
    pump(b, bn, 1);
    if (!cmp_wire(exp)) ok = 0;
    report(name, exp, ok);               /* 屏幕上看到的是第二帧的响应 */
}

/* ---------------- 用例 ---------------- */
int main(void)
{
    /* 注意：用例 8 -> 9 有依赖（先写 regs[2]=1000，再回读验证），别调换顺序 */

    static const uint8_t t01[] = {0x01,0x03,0x00,0x00,0x00,0x02,0xC4,0x0B};
    static const uint8_t t02[] = {0x01,0x03,0x00,0x00,0x00,0x01,0x84,0x0A};
    static const uint8_t t03[] = {0x01,0x03,0x00,0x10,0x00,0x01,0x85,0xCF};
    static const uint8_t t04[] = {0x01,0x03,0x00,0x00,0x00,0x7E,0xC5,0xEA};
    static const uint8_t t05[] = {0x01,0x03,0x00,0x00,0x00,0x02,0xC4,0x0C}; /* CRC 改一位   */
    static const uint8_t t06[] = {0x02,0x03,0x00,0x00,0x00,0x01,0x84,0x39}; /* 地址不是本机 */
    static const uint8_t t07[] = {0x01,0x04,0x00,0x00,0x00,0x01,0x31,0xCA}; /* 非法功能码   */
    static const uint8_t t08[] = {0x01,0x06,0x00,0x02,0x03,0xE8,0x28,0xB4}; /* 写 regs[2]   */
    static const uint8_t t09[] = {0x01,0x03,0x00,0x02,0x00,0x01,0x25,0xCA}; /* 回读 regs[2] */
    static const uint8_t t10[] = {0x01,0x03,0x40,0x21};                     /* 短帧 0x03    */
    static const uint8_t t11[] = {0x01,0x06,0x80,0x22};                     /* 短帧 0x06    */
    static const uint8_t t12[] = {0x01,0x06,0x00,0x10,0x00,0x01,0x49,0xCF}; /* 写越界地址   */
    static const uint8_t t13[] = {0x01,0x03,0x00,0x00,0x00,0x08,0x44,0x0C}; /* 读全部 8 个  */
    static const uint8_t t14[] = {0x01,0x06,0x00,0x00,0x00,0x0A,0x09,0xCD}; /* 写 regs[0]   */
    static const uint8_t t15[] = {0x01,0x03,0x00,0x00,0x00,0x00,0x45,0xCA}; /* 数量 = 0     */
    static const uint8_t t16[] = {0x01,0x03,0xFF,0xFF,0x00,0x09,0x85,0xE8}; /* 溢出攻击     */
    static const uint8_t t18[] = {0x01,0x03,0x00,0x00,0x00,0x01,0x84,0x0A}; /* 连发两帧     */

    out("\n===== Modbus slave PC simulation =====\n");

    feed ("01 read 2 regs",            "01 03 04 00 64 00 C8 BA 7A", t01, 8, 1);
    feed ("02 read 1 reg",             "01 03 02 00 64 B9 AF",       t02, 8, 1);
    feed ("03 addr 16 out of range",   "01 83 02 C0 F1",             t03, 8, 1);
    feed ("04 qty 126 over limit",     "01 83 03 01 31",             t04, 8, 1);
    feed ("05 CRC one bit flipped",    "SILENT",                     t05, 8, 1);
    feed ("06 addr 0x02 not mine",     "SILENT",                     t06, 8, 1);
    feed ("07 illegal function 0x04",  "01 84 01 82 C0",             t07, 8, 1);
    feed ("08 write regs[2] = 1000",   "01 06 00 02 03 E8 28 B4",    t08, 8, 1);
    feed ("09 read back regs[2]",      "01 03 02 03 E8 B8 FA",       t09, 8, 1);
    feed ("10 short frame 0x03 (4B)",  "SILENT",                     t10, 4, 1);
    feed ("11 short frame 0x06 (4B)",  "SILENT",                     t11, 4, 1);
    feed ("12 write out-of-range",     "01 86 02 C3 A1",             t12, 8, 1);
    feed ("13 read all 8 regs",        "01 03 10 .",                 t13, 8, 1);
    feed ("14 write regs[0] = 10",     "01 06 00 00 00 0A 09 CD",    t14, 8, 1);
    feed ("15 qty = 0",                "01 83 03 01 31",             t15, 8, 1);
    feed ("16 overflow: 0xFFFF + 9",   "01 83 02 C0 F1",             t16, 8, 1);
    /* 17 是"已知缺口"，不是缺陷用例，见 README 第 4 条。
       规范：字符间隔 > 1.5 字符判帧错、> 3.5 字符算新帧；
       当前实现只在 modbus_poll 里看"最后字节之后的静默"，
       不看"字节之间"的间隔，所以慢速发来的字节会被拼成一帧。
       这里先锁住当前行为，等补了帧内间隔检测再改成 "SILENT"。 */
    feed ("17 slow send (KNOWN LIMIT)", "01 03 04 .",                t01, 8, 5);
    feed2("18 two frames in a row",    "01 03 02 .",                 t18, 8, t18, 8);

    out("\n===============  PASS = ");
    out_dec(g_pass);
    out("   FAIL = ");
    out_dec(g_fail);
    out("  ===============\n\n");

    return g_fail;                       /* 退出码 = 失败数，方便脚本判断 */
}

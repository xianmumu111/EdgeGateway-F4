#include <stdio.h>
#include <string.h>
#include "modbus_rtu.h"
#include "crc16.h"

/* =====================================================================
 * test_modbus.c —— Modbus RTU 从站单元测试
 *
 * 验证策略（重要）：
 *   1. 用 **Modbus 官方 spec 的示例帧** 做对拍 —— 期望值是标准给的，
 *      不是我们自己算出来的，避免"自算自验"的自证循环。
 *   2. 其余用例：比对除 CRC 外的所有字节 + 独立校验 CRC 字段本身正确。
 *      （CRC 算法的正确性已由 test_crc.c 用官方向量保证，可复用）
 *   3. 每个测试用例都要能被"改坏实现"触发失败 —— 见 DEVLOG 的变异测试记录。
 * ===================================================================== */

static int g_total = 0;
static int g_fails = 0;

/* 失败计数，绝不用 ok &= f() —— 见 9/12 踩过的坑 */
static void chk(int cond, const char *name)
{
    g_total++;
    if (!cond) g_fails++;
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
}

/* 给一段"裸数据"补上正确的 CRC，返回帧长 */
static uint16_t mkframe(uint8_t *out, const uint8_t *body, uint16_t body_len)
{
    memcpy(out, body, body_len);
    uint16_t c = crc16_modbus(out, body_len);
    out[body_len]     = (uint8_t)(c & 0xFFu);
    out[body_len + 1] = (uint8_t)(c >> 8);
    return (uint16_t)(body_len + 2u);
}

static void dump(const char *tag, const uint8_t *p, uint16_t n)
{
    printf("       %s(%u):", tag, n);
    for (uint16_t i = 0; i < n; i++) printf(" %02X", p[i]);
    printf("\n");
}

/**
 * 跑一个用例。
 * want    期望的响应内容（含 CRC 时 need_crc_check 会自动校验 CRC 自洽）
 * want_len=0 且 want=NULL 表示期望"不该有响应"
 */
static void run(mb_slave_t *s,
                const uint8_t *req, uint16_t req_len,
                mb_status_t want_st,
                const uint8_t *want, uint16_t want_len,
                const char *name)
{
    uint8_t rsp[64];
    uint16_t rsp_len = 0;

    mb_status_t st = mb_slave_handle(s, req, req_len, rsp, sizeof rsp, &rsp_len);

    int ok = (st == want_st);

    if (ok && st == MB_OK) {
        /* 长度必须一致 */
        ok = (rsp_len == want_len);
        if (ok) {
            /* 前 want_len-2 字节逐字节比对 */
            uint16_t body = (uint16_t)(want_len - 2u);
            for (uint16_t i = 0; i < body; i++) {
                if (rsp[i] != want[i]) { ok = 0; break; }
            }
            /* CRC 独立校验：用前 len-2 字节重算，应等于帧尾两字节 */
            if (ok) {
                uint16_t calc = crc16_modbus(rsp, (uint16_t)(rsp_len - 2u));
                uint16_t got  = (uint16_t)(rsp[rsp_len - 2] |
                                          ((uint16_t)rsp[rsp_len - 1] << 8));
                if (calc != got) ok = 0;
            }
        }
    } else if (ok && st != MB_OK) {
        ok = (rsp_len == 0u);   /* 非 OK 时必须没有任何输出 */
    }

    chk(ok, name);
    if (!ok) {
        printf("       status=%s (want %s)  rsp_len=%u (want %u)\n",
               mb_status_str(st), mb_status_str(want_st), rsp_len, want_len);
        dump("got ", rsp, rsp_len);
        if (want) dump("want", want, want_len);
    }
}

int main(void)
{
    printf("\n=== Modbus RTU Slave Unit Test ===\n\n");

    static uint16_t regs[128];
    mb_slave_t slave = {
        .addr = 0x11, .regs = regs, .regs_n = 128,
        .stat_ok = 0, .stat_except = 0, .stat_bad_crc = 0, .stat_ignored = 0,
    };

    /* ----------------------------------------------------------------
     * 用例 1（★ 最重要）：Modbus 官方 spec 示例帧，逐字节对拍
     *   请求: 11 03 006B 0003 7687     (从站0x11 读 0x006B 起 3 个寄存器)
     *   响应: 11 03 06 AE41 5652 4340 49AD
     * 期望值来自 Modbus 标准文档，不是我们算出来的。
     * -------------------------------------------------------------- */
    printf("[1] 官方 spec 示例帧对拍\n");
    regs[0x6B] = 0xAE41;   /* 这三个值是 spec 里给的示例数据 */
    regs[0x6C] = 0x5652;
    regs[0x6D] = 0x4340;

    {
        const uint8_t req[]  = {0x11,0x03,0x00,0x6B,0x00,0x03,0x76,0x87};
        const uint8_t want[] = {0x11,0x03,0x06,0xAE,0x41,0x56,0x52,
                                0x43,0x40,0x49,0xAD};
        run(&slave, req, sizeof req, MB_OK, want, sizeof want,
            "0x03 读3个寄存器 == spec 响应帧");
    }

    /* ----------------------------------------------------------------
     * 用例 2：不该响应的情况
     * -------------------------------------------------------------- */
    printf("\n[2] 静默场景（地址不匹配 / 广播 / 帧损坏）\n");
    {
        uint8_t req[8];
        uint8_t body1[] = {0x12,0x03,0x00,0x6B,0x00,0x03};  /* 地址 0x12 ≠ 0x11 */
        uint16_t n = mkframe(req, body1, 6);
        run(&slave, req, n, MB_NO_REPLY, NULL, 0, "地址不匹配 -> 不回应");

        uint8_t body2[] = {0x00,0x03,0x00,0x6B,0x00,0x03};  /* 广播 */
        n = mkframe(req, body2, 6);
        run(&slave, req, n, MB_NO_REPLY, NULL, 0, "广播地址 -> 不回应");

        /* CRC 故意错：把最后一字节取反 */
        uint8_t body3[] = {0x11,0x03,0x00,0x6B,0x00,0x03};
        n = mkframe(req, body3, 6);
        req[n - 1] ^= 0xFFu;
        run(&slave, req, n, MB_BAD_FRAME, NULL, 0, "CRC 错 -> 丢弃");

        /* 帧太短 */
        uint8_t short_f[] = {0x11,0x03,0x00};
        run(&slave, short_f, 3, MB_BAD_FRAME, NULL, 0, "帧长 3 < 4 -> 丢弃");
    }

    /* ----------------------------------------------------------------
     * 用例 3：异常响应
     * -------------------------------------------------------------- */
    printf("\n[3] 异常响应（addr | fc|0x80 | code | crc）\n");
    {
        uint8_t req[8];

        uint8_t b1[] = {0x11,0x03,0x00,0x00,0x00,0x00};   /* 读 0 个 -> 0x03 */
        uint16_t n = mkframe(req, b1, 6);
        uint8_t w1[] = {0x11,0x83,0x03,0x00,0x00};
        run(&slave, req, n, MB_OK, w1, 5, "读 0 个 -> 异常 0x83 0x03");

        uint8_t b2[] = {0x11,0x03,0x00,0x00,0x00,0xC8};   /* 读 200 -> 超 125 */
        n = mkframe(req, b2, 6);
        run(&slave, req, n, MB_OK, w1, 5, "读 200 个 -> 异常 0x83 0x03");

        uint8_t b3[] = {0x11,0x03,0x00,0x7F,0x00,0x02};   /* 0x7F+2 > 128 越界 */
        n = mkframe(req, b3, 6);
        uint8_t w3[] = {0x11,0x83,0x02,0x00,0x00};
        run(&slave, req, n, MB_OK, w3, 5, "地址越界 -> 异常 0x83 0x02");

        uint8_t b4[] = {0x11,0x02,0x00,0x00,0x00,0x01};   /* 功能码 0x02 不支持 */
        n = mkframe(req, b4, 6);
        uint8_t w4[] = {0x11,0x82,0x01,0x00,0x00};
        run(&slave, req, n, MB_OK, w4, 5, "未知功能码 -> 异常 0x82 0x01");
    }

    /* ----------------------------------------------------------------
     * 用例 4：写寄存器 0x06（正常 + 越界）
     * -------------------------------------------------------------- */
    printf("\n[4] 功能码 0x06 写单个寄存器\n");
    {
        uint8_t req[8];
        uint8_t b1[] = {0x11,0x06,0x00,0x10,0x12,0x34};   /* regs[0x10] = 0x1234 */
        uint16_t n = mkframe(req, b1, 6);
        uint8_t w1[] = {0x11,0x06,0x00,0x10,0x12,0x34,0x00,0x00};  /* 回显 */
        run(&slave, req, n, MB_OK, w1, 8, "写 0x10=0x1234 -> 回显请求");
        chk(regs[0x10] == 0x1234, "寄存器真的被写进去了");

        uint8_t b2[] = {0x11,0x06,0x00,0xFF,0x00,0x01};   /* 0xFF >= 128 越界 */
        n = mkframe(req, b2, 6);
        uint8_t w2[] = {0x11,0x86,0x02,0x00,0x00};
        run(&slave, req, n, MB_OK, w2, 5, "写越界 -> 异常 0x86 0x02");
    }

    /* ----------------------------------------------------------------
     * 用例 5：集成 —— 写进去的值能被读回来
     * -------------------------------------------------------------- */
    printf("\n[5] 集成：先写后读\n");
    {
        uint8_t req[8];
        uint8_t bw[] = {0x11,0x06,0x00,0x20,0xAB,0xCD};
        uint16_t n = mkframe(req, bw, 6);
        mb_slave_handle(&slave, req, n, req, sizeof req, &n);   /* 复用缓冲当输出 */

        uint8_t br[] = {0x11,0x03,0x00,0x20,0x00,0x01};
        n = mkframe(req, br, 6);
        uint8_t w[] = {0x11,0x03,0x02,0xAB,0xCD,0x00,0x00};
        run(&slave, req, n, MB_OK, w, 7, "读回 0x20 == 0xABCD");
    }

    /* ----------------------------------------------------------------
     * 用例 6：边界 —— 最后一个寄存器可以读
     * -------------------------------------------------------------- */
    printf("\n[6] 边界：regs_n=128 时读最后一个(0x7F)\n");
    {
        regs[0x7F] = 0x55AA;
        uint8_t req[8];
        uint8_t b[] = {0x11,0x03,0x00,0x7F,0x00,0x01};
        uint16_t n = mkframe(req, b, 6);
        uint8_t w[] = {0x11,0x03,0x02,0x55,0xAA,0x00,0x00};
        run(&slave, req, n, MB_OK, w, 7, "读 0x7F 成功");
    }

    printf("\n=== 统计 ===\n");
    printf("  ok=%lu  except=%lu  bad_crc=%lu  ignored=%lu\n",
           (unsigned long)slave.stat_ok,      (unsigned long)slave.stat_except,
           (unsigned long)slave.stat_bad_crc, (unsigned long)slave.stat_ignored);

    printf("\n=== %d/%d passed ===\n", g_total - g_fails, g_total);
    return (g_fails == 0) ? 0 : 1;
}

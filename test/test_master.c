/* =====================================================================
 * test_master.c —— Modbus 主站协议层单元测试
 *
 * 跑法：cd test && build.bat
 *
 * 用的是官方 spec 的 golden vector，和从站那边同一组，
 * 方便"主站组帧 -> 从站解析"对拍。
 * ===================================================================== */

#include <stdio.h>
#include <string.h>
#include "mb_master.h"

static int g_fail = 0;
static int g_run  = 0;

/* ⚠️ 累加失败数，绝不用 ok &= f()（0 & 任何数 = 0，会让测试永远通过） */
static void check(int cond, const char *what)
{
    g_run++;
    if (!cond) { g_fail++; printf("  [FAIL] %s\n", what); }
}

static void dump(const char *tag, const uint8_t *b, int n)
{
    printf("  %s: ", tag);
    for (int i = 0; i < n; i++) printf("%02X ", b[i]);
    printf("\n");
}

static int cmp_hex(const uint8_t *got, const uint8_t *want, int n)
{
    for (int i = 0; i < n; i++)
        if (got[i] != want[i]) return 0;
    return 1;
}

/* 测试用的小工具：大端写入 */
static void put_sim_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

/* 测试用的小工具：就地给前 n 个字节补上正确的 CRC，
 * 这样构造"CRC 正确但内容错"的帧时不用手算 */
static void fix_crc(uint8_t *b, uint16_t n)
{
    uint16_t c = 0xFFFF;
    for (uint16_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (uint16_t)((c >> 1) ^ 0xA001) : (uint16_t)(c >> 1);
    }
    b[n]     = (uint8_t)(c & 0xFF);
    b[n + 1] = (uint8_t)(c >> 8);
}

int main(void)
{
    uint8_t buf[64];
    uint16_t out[16];
    uint16_t out_n = 0;
    uint8_t  exc   = 0;
    mb_m_err_t e;

    printf("==== test_master : Modbus RTU master protocol layer ====\n\n");

    /* ---------------------------------------------------------------
     * 1. 组帧 0x03 —— 官方 spec golden vector
     * --------------------------------------------------------------- */
    printf("[1] build_read golden vector (official spec)\n");
    {
        const uint8_t want[] = {0x11,0x03,0x00,0x6B,0x00,0x03,0x76,0x87};
        uint16_t n = mb_master_build_read(buf, sizeof(buf), 0x11, 0x006B, 0x0003);
        check(n == 8, "0x03 frame length == 8");
        if (n == 8) {
            dump("got ", buf, n);
            dump("want", want, 8);
            check(cmp_hex(buf, want, 8), "0x03 official vector (11 03 00 6B 00 03 76 87)");
        }

        /* 今天上板用的：1 号从站，读 reg0..1 */
        const uint8_t want2[] = {0x01,0x03,0x00,0x00,0x00,0x02,0xC4,0x0B};
        n = mb_master_build_read(buf, sizeof(buf), 0x01, 0x0000, 0x0002);
        check(n == 8, "0x03 length == 8 (today's frame)");
        if (n == 8) {
            dump("got ", buf, n);
            check(cmp_hex(buf, want2, 8), "0x03 today vector (01 03 00 00 00 02 C4 0B)");
        }
    }

    /* ---------------------------------------------------------------
     * 2. 组帧 0x03 —— 参数防御
     * --------------------------------------------------------------- */
    printf("\n[2] build_read argument guards\n");
    check(mb_master_build_read(buf, sizeof(buf), 0x01, 0, 0) == 0,    "qty=0 rejected");
    check(mb_master_build_read(buf, sizeof(buf), 0x01, 0, 126) == 0,  "qty=126 rejected (>125)");
    check(mb_master_build_read(buf, 7, 0x01, 0, 2) == 0,              "buf_max=7 rejected");
    check(mb_master_build_read(NULL, 64, 0x01, 0, 2) == 0,            "NULL buf rejected");
    check(mb_master_build_read(buf, sizeof(buf), 0x01, 0, 125) == 8,  "qty=125 accepted (boundary)");

    /* ---------------------------------------------------------------
     * 3. 组帧 0x06
     * --------------------------------------------------------------- */
    printf("\n[3] build_write_single\n");
    {
        const uint8_t want[] = {0x01,0x06,0x00,0x00,0x00,0xFF,0xC9,0x8A};
        uint16_t n = mb_master_build_write_single(buf, sizeof(buf), 0x01, 0x0000, 0x00FF);
        check(n == 8, "0x06 frame length == 8");
        if (n == 8) {
            dump("got ", buf, n);
            check(cmp_hex(buf, want, 8), "0x06 vector (01 06 00 00 00 FF C9 8A)");
        }
        check(mb_master_build_write_single(buf, 7, 0x01, 0, 1) == 0, "0x06 buf_max=7 rejected");

        /* 大端检查：值 0x1234 在线上必须是 12 34，不是 34 12 */
        n = mb_master_build_write_single(buf, sizeof(buf), 0x01, 0x0010, 0x1234);
        check(n == 8 && buf[4] == 0x12 && buf[5] == 0x34, "0x06 value is big-endian on wire");
        n = mb_master_build_write_single(buf, sizeof(buf), 0x01, 0x1234, 0x0001);
        check(n == 8 && buf[2] == 0x12 && buf[3] == 0x34, "0x06 addr is big-endian on wire");
    }

    /* ---------------------------------------------------------------
     * 4. 预判响应长度（主站分帧核心）
     * --------------------------------------------------------------- */
    printf("\n[4] rsp_len prediction\n");
    {
        const uint8_t req[] = {0x01,0x03,0x00,0x00,0x00,0x02,0xC4,0x0B};
        const uint8_t h1[]  = {0x01,0x03,0x04};          /* 正常：读 2 个 */
        const uint8_t h2[]  = {0x01,0x83,0x02};          /* 异常帧 */
        const uint8_t h3[]  = {0x01,0x03};               /* 才 2 字节 */

        check(mb_master_rsp_len(req, 8, h1, 3) == 9, "0x03 qty=2 -> 9 bytes");
        check(mb_master_rsp_len(req, 8, h2, 3) == 5, "exception frame -> 5 bytes");
        check(mb_master_rsp_len(req, 8, h3, 2) == 0, "only 2 bytes -> 0 (need more)");
        check(mb_master_rsp_len(req, 8, h1, 0) == 0, "head_len=0 -> 0");

        /* 读 125 个：5 + 250 = 255 */
        const uint8_t req125[] = {0x01,0x03,0x00,0x00,0x00,0x7D,0x00,0x00};
        const uint8_t h125[]   = {0x01,0x03,0xFA};
        check(mb_master_rsp_len(req125, 8, h125, 3) == 255, "0x03 qty=125 -> 255 bytes");

        /* 0x06 回显 8 字节 */
        const uint8_t req06[] = {0x01,0x06,0x00,0x00,0x00,0xFF,0xC9,0x8A};
        const uint8_t h06[]   = {0x01,0x06,0x00};
        check(mb_master_rsp_len(req06, 8, h06, 3) == 8, "0x06 -> 8 bytes (echo)");
    }

    /* ---------------------------------------------------------------
     * 5. 解析正常响应 —— 官方 spec
     * --------------------------------------------------------------- */
    printf("\n[5] parse normal response (official spec)\n");
    {
        const uint8_t req[] = {0x11,0x03,0x00,0x6B,0x00,0x03,0x76,0x87};
        const uint8_t rsp[] = {0x11,0x03,0x06,0xAE,0x41,0x56,0x52,0x43,0x40,0x49,0xAD};
        out_n = 0;
        e = mb_master_parse(req, 8, rsp, 11, out, 16, &out_n, &exc);
        check(e == MB_M_OK, "official vector -> MB_M_OK");
        check(out_n == 3, "out_n == 3");
        check(out[0] == 0xAE41, "out[0] == 0xAE41 (big-endian converted)");
        check(out[1] == 0x5652, "out[1] == 0x5652");
        check(out[2] == 0x4340, "out[2] == 0x4340");
        if (e != MB_M_OK) printf("  -> got %s\n", mb_m_err_str(e));
    }

    /* ---------------------------------------------------------------
     * 6. 解析正常响应 —— 今天上板那组
     * --------------------------------------------------------------- */
    printf("\n[6] parse normal response (today's board frame)\n");
    {
        const uint8_t req[] = {0x01,0x03,0x00,0x00,0x00,0x02,0xC4,0x0B};
        const uint8_t rsp[] = {0x01,0x03,0x04,0x00,0x0A,0x00,0x14,0xDA,0x3E};
        out_n = 0;
        e = mb_master_parse(req, 8, rsp, 9, out, 16, &out_n, &exc);
        check(e == MB_M_OK, "today vector -> MB_M_OK");
        check(out_n == 2, "out_n == 2");
        check(out[0] == 10, "out[0] == 10");
        check(out[1] == 20, "out[1] == 20");
        if (e != MB_M_OK) printf("  -> got %s\n", mb_m_err_str(e));
    }

    /* ---------------------------------------------------------------
     * 7. 异常帧
     * --------------------------------------------------------------- */
    printf("\n[7] parse exception frame\n");
    {
        const uint8_t req[] = {0x01,0x03,0x00,0x00,0x00,0x02,0xC4,0x0B};
        const uint8_t rsp[] = {0x01,0x83,0x02,0xC0,0xF1};
        exc = 0;
        e = mb_master_parse(req, 8, rsp, 5, out, 16, &out_n, &exc);
        check(e == MB_M_EXCEPTION, "exception frame -> MB_M_EXCEPTION");
        check(exc == 0x02, "exc code == 0x02 (illegal data address)");
        if (e != MB_M_EXCEPTION) printf("  -> got %s\n", mb_m_err_str(e));
    }

    /* ---------------------------------------------------------------
     * 8. 各种错误分支
     * --------------------------------------------------------------- */
    printf("\n[8] parse error branches\n");
    {
        const uint8_t req[] = {0x01,0x03,0x00,0x00,0x00,0x02,0xC4,0x0B};

        /* CRC 坏掉：把最后一个字节改掉 */
        uint8_t bad[] = {0x01,0x03,0x04,0x00,0x0A,0x00,0x14,0xDA,0x3F};
        check(mb_master_parse(req, 8, bad, 9, out, 16, &out_n, &exc) == MB_M_CRC_ERR,
              "bad CRC -> MB_M_CRC_ERR");

        /* 地址不是我等的从站（总线串扰 / 别的从站的迟到响应） */
        uint8_t wrong_addr[] = {0x02,0x03,0x04,0x00,0x0A,0x00,0x14,0x00,0x00};
        fix_crc(wrong_addr, 7);   /* 补正确 CRC，才能测到"地址不匹配"这一层 */
        check(mb_master_parse(req, 8, wrong_addr, 9, out, 16, &out_n, &exc) == MB_M_ADDR_ERR,
              "slave addr mismatch -> MB_M_ADDR_ERR");

        /* 功能码完全不相关 */
        uint8_t wrong_fc[] = {0x01,0x05,0x04,0x00,0x0A,0x00,0x14,0x00,0x00};
        fix_crc(wrong_fc, 7);
        check(mb_master_parse(req, 8, wrong_fc, 9, out, 16, &out_n, &exc) == MB_M_FC_ERR,
              "unrelated fc -> MB_M_FC_ERR");

        /* 字节计数和请求的数量对不上 */
        uint8_t bad_cnt[] = {0x01,0x03,0x08,0x00,0x0A,0x00,0x14,0x00,0x00};
        fix_crc(bad_cnt, 7);
        check(mb_master_parse(req, 8, bad_cnt, 9, out, 16, &out_n, &exc) == MB_M_LEN_ERR,
              "byte count != 2*qty -> MB_M_LEN_ERR");

        /* 帧太短 */
        check(mb_master_parse(req, 8, req, 4, out, 16, &out_n, &exc) == MB_M_BAD_ARG,
              "rsp_len < 5 -> MB_M_BAD_ARG");

        /* 输出缓冲太小 */
        const uint8_t rsp_ok[] = {0x01,0x03,0x04,0x00,0x0A,0x00,0x14,0xDA,0x3E};
        check(mb_master_parse(req, 8, rsp_ok, 9, out, 1, &out_n, &exc) == MB_M_NO_SPACE,
              "out_max too small -> MB_M_NO_SPACE");
    }

    /* ---------------------------------------------------------------
     * 9. 0x06 响应解析
     * --------------------------------------------------------------- */
    printf("\n[9] parse 0x06 echo response\n");
    {
        const uint8_t req[] = {0x01,0x06,0x00,0x00,0x00,0xFF,0xC9,0x8A};
        const uint8_t rsp[] = {0x01,0x06,0x00,0x00,0x00,0xFF,0xC9,0x8A};
        out_n = 0;
        e = mb_master_parse(req, 8, rsp, 8, out, 16, &out_n, &exc);
        check(e == MB_M_OK, "0x06 echo -> MB_M_OK");
        check(out_n == 1, "out_n == 1");
        check(out[0] == 0x00FF, "out[0] == 0x00FF");

        /* 回显的地址和请求对不上 —— 写错寄存器了，必须抓住 */
        const uint8_t rsp_bad[] = {0x01,0x06,0x00,0x05,0x00,0xFF,0x00,0x00};
        check(mb_master_parse(req, 8, rsp_bad, 8, out, 16, &out_n, &exc) == MB_M_ADDR_ERR ||
              mb_master_parse(req, 8, rsp_bad, 8, out, 16, &out_n, &exc) == MB_M_CRC_ERR,
              "0x06 wrong echo addr -> rejected (ADDR or CRC)");
    }

    /* ---------------------------------------------------------------
     * 10. 组帧 -> 解析 回环（自己发的自己能读懂）
     * --------------------------------------------------------------- */
    printf("\n[10] build -> parse round trip\n");
    {
        uint8_t req[8];
        uint16_t n = mb_master_build_read(req, sizeof(req), 0x0A, 0x0100, 0x0004);
        check(n == 8, "round trip: build ok");

        /* 手工拼一个从站响应：4 个寄存器 */
        uint8_t rsp[64];
        rsp[0] = 0x0A; rsp[1] = 0x03; rsp[2] = 8;
        put_sim_be16(&rsp[3], 0x1234);
        put_sim_be16(&rsp[5], 0x5678);
        put_sim_be16(&rsp[7], 0x9ABC);
        put_sim_be16(&rsp[9], 0xDEF0);
        fix_crc(rsp, 11);
        out_n = 0;
        e = mb_master_parse(req, 8, rsp, 13, out, 16, &out_n, &exc);
        check(e == MB_M_OK, "round trip -> MB_M_OK");
        check(out_n == 4, "round trip out_n == 4");
        check(out[0] == 0x1234 && out[1] == 0x5678 &&
              out[2] == 0x9ABC && out[3] == 0xDEF0, "round trip values match");
    }

    /* --------------------------------------------------------------- */
    printf("\n========================================\n");
    printf("  %d checks, %d failed\n", g_run, g_fail);
    if (g_fail == 0) printf("  ==== ALL PASS ====\n");
    else             printf("  ==== FAILED ====\n");
    printf("========================================\n");
    return g_fail ? 1 : 0;
}

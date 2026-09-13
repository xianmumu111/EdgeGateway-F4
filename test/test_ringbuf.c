/* =====================================================================
 * test_ringbuf.c —— ringbuf 单元测试
 *
 * 编译：gcc -Wall -Wextra -std=c11 -I../Drivers/ringbuf -I../Drivers/crc16 \
 *           test_ringbuf.c ../Drivers/ringbuf/ringbuf.c ../Drivers/crc16/crc16.c \
 *           -o test_ringbuf.exe
 *
 * 两条测试纪律（上次写 crc16 测试时踩过坑）：
 *   1) 结果用「失败计数」，绝对不用 ok &= xxx —— 一旦出现 0 就永远翻不回来，
 *      测试会变成"永远通过"的样子，比没有测试更危险。
 *   2) 随机数用确定性 PRNG（xorshift32），跑失败可以原样复现。
 * ===================================================================== */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../Drivers/ringbuf/ringbuf.h"
#include "../Drivers/crc16/crc16.h"

/* ------------------------------------------------------------------
 * 迷你测试框架
 * ---------------------------------------------------------------- */
static int g_pass = 0;
static int g_fail = 0;

#define SECTION(name)  printf("\n--- %s\n", name)

#define CHECK(cond, msg, ...)                                              \
    do {                                                                   \
        if (cond) {                                                        \
            g_pass++;                                                      \
        } else {                                                           \
            g_fail++;                                                      \
            printf("    [FAIL] line %d: " msg "\n", __LINE__, ##__VA_ARGS__); \
        }                                                                  \
    } while (0)

/* 确定性伪随机：xorshift32，同一个 seed 永远得到同一串数 */
static uint32_t g_seed = 0x12345678u;

static void rnd_set(uint32_t s) { g_seed = s ? s : 1u; }

static uint32_t rnd(void)
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

/* ------------------------------------------------------------------
 * 参考模型：一个"无限大"的 FIFO，用来和被测试的环形缓冲对拍。
 * 它用最笨的线性数组实现，几乎不可能写错 —— 写错的人应该是 rb。
 * ---------------------------------------------------------------- */
#define MODEL_CAP (1u << 20)
static uint8_t  m_buf[MODEL_CAP];
static uint32_t m_head;
static uint32_t m_tail;

static void     model_reset(void) { m_head = 0; m_tail = 0; }
static uint32_t model_used(void)  { return m_head - m_tail; }

static void model_put(uint8_t c)
{
    m_buf[(m_head++) & (MODEL_CAP - 1u)] = c;
}

static int model_get(uint8_t *c)
{
    if (m_head == m_tail) { return 0; }
    *c = m_buf[(m_tail++) & (MODEL_CAP - 1u)];
    return 1;
}

static uint8_t model_at(uint32_t off)
{
    return m_buf[(m_tail + off) & (MODEL_CAP - 1u)];
}

/* ==================================================================
 * 用例 1：初始化与参数防御
 * ================================================================ */
static void test_init_capacity(void)
{
    static uint8_t st[64];
    rb_t rb;
    rb_t bad;

    SECTION("init / capacity / parameter guards");

    /* size 非 2 的幂必须被拒绝 —— 这是本实现的硬性前提 */
    CHECK(rb_init(NULL, st, 64) == 0, "null rb must be rejected");
    CHECK(rb_init(&rb, NULL, 64) == 0, "null buf must be rejected");
    CHECK(rb_init(&rb, st, 63)   == 0, "non power-of-2 size must be rejected");
    CHECK(rb_init(&rb, st, 0)    == 0, "zero size must be rejected");
    CHECK(rb_init(&rb, st, 64)   == 1, "valid init must succeed");

    CHECK(rb_is_valid(&rb),       "rb_is_valid");
    CHECK(rb_capacity(&rb) == 64, "capacity = 64, got %u", rb_capacity(&rb));
    CHECK(rb_is_empty(&rb),       "fresh buffer must be empty");
    CHECK(!rb_is_full(&rb),       "fresh buffer must not be full");
    CHECK(rb_used(&rb) == 0,      "used = 0, got %u", rb_used(&rb));
    CHECK(rb_free(&rb) == 64,     "free = 64, got %u", rb_free(&rb));

    /* 未初始化的控制块：所有查询都应给出安全答案，不能崩 */
    memset(&bad, 0, sizeof bad);
    CHECK(rb_is_valid(&bad) == 0,  "all-zero rb is invalid");
    CHECK(rb_capacity(&bad) == 0,  "invalid rb capacity = 0");
    CHECK(rb_used(&bad) == 0,      "invalid rb used = 0");
    CHECK(rb_put(&bad, 1) == 0,    "put into invalid rb fails");
    CHECK(rb_write(&bad, st, 4) == 0, "write into invalid rb fails");
    rb_flush(&bad);                    /* 不能崩 */
}

/* ==================================================================
 * 用例 2：单字节 FIFO 顺序
 * ================================================================ */
static void test_fifo_order(void)
{
    static uint8_t st[16];
    rb_t rb;
    int  ok = 1;

    SECTION("single byte FIFO order");

    rb_init(&rb, st, sizeof st);
    for (uint32_t i = 0; i < 10u; i++) {
        ok &= (rb_put(&rb, (uint8_t)(i + 100u)) == 1);
    }
    CHECK(ok, "put 10 bytes");
    CHECK(rb_used(&rb) == 10, "used = 10, got %u", rb_used(&rb));

    ok = 1;
    for (uint32_t i = 0; i < 10u; i++) {
        uint8_t c = 0;
        if (rb_get(&rb, &c) != 1)      { ok = 0; break; }
        if (c != (uint8_t)(i + 100u))  { ok = 0; break; }
    }
    CHECK(ok, "read back exactly in FIFO order");
    CHECK(rb_is_empty(&rb), "empty after draining 10 bytes");
}

/* ==================================================================
 * 用例 3：满载 —— 一格都不能浪费
 * ================================================================ */
static void test_full_no_wasted_slot(void)
{
    static uint8_t st[32];
    rb_t rb;

    SECTION("full boundary: all 32 slots usable (no wasted slot)");

    rb_init(&rb, st, sizeof st);

    uint32_t n = 0;
    for (uint32_t i = 0; i < 32u; i++) {
        n += (uint32_t)rb_put(&rb, (uint8_t)i);
    }
    CHECK(n == 32, "all 32 puts must succeed, got %u", n);
    CHECK(rb_is_full(&rb), "must report full");
    CHECK(rb_used(&rb) == 32, "used = 32 (NOT 31), got %u", rb_used(&rb));
    CHECK(rb_free(&rb) == 0, "free = 0, got %u", rb_free(&rb));
    CHECK(rb_put(&rb, 0xAA) == 0, "33rd put must fail");

    /* 读一个出来，立刻又能写一个进去 */
    uint8_t c = 0;
    CHECK(rb_get(&rb, &c) == 1, "get one");
    CHECK(c == 0, "first byte should be 0, got %u", c);
    CHECK(!rb_is_full(&rb), "no longer full after one get");
    CHECK(rb_put(&rb, 0xFF) == 1, "put succeeds again");

    /* 空 buffer 读不出东西 */
    rb_flush(&rb);
    CHECK(rb_is_empty(&rb), "flush -> empty");
    CHECK(rb_get(&rb, &c) == 0, "get from empty must fail");
    CHECK(rb_read(&rb, st, 5) == 0, "read from empty must return 0");
    CHECK(rb_peek(&rb, st, 5) == 0, "peek from empty must return 0");
    CHECK(rb_peek_at(&rb, 0, &c) == 0, "peek_at(0) on empty must fail");
    CHECK(rb_skip(&rb, 5) == 0, "skip on empty must return 0");
    CHECK(rb_write(&rb, NULL, 5) == 0, "write with null src must be rejected");
    CHECK(rb_read(&rb, NULL, 5) == 0, "read with null dst must be rejected");
}

/* ==================================================================
 * 用例 4：peek 不推进 tail / skip 丢弃
 * ================================================================ */
static void test_peek_and_skip(void)
{
    static uint8_t st[16];
    rb_t rb;
    uint8_t out[16];

    SECTION("peek (non-destructive) and skip");

    rb_init(&rb, st, sizeof st);
    for (uint32_t i = 0; i < 8u; i++) { rb_put(&rb, (uint8_t)(i + 1u)); }

    memset(out, 0, sizeof out);
    CHECK(rb_peek(&rb, out, 4) == 4, "peek 4 bytes");
    CHECK(out[0] == 1 && out[3] == 4, "peek content 1..4");
    CHECK(rb_used(&rb) == 8, "peek must NOT move tail (used still 8, got %u)", rb_used(&rb));

    uint8_t c = 0;
    CHECK(rb_peek_at(&rb, 7, &c) == 1, "peek_at(7) ok");
    CHECK(c == 8, "last byte should be 8, got %u", c);
    CHECK(rb_peek_at(&rb, 8, &c) == 0, "peek_at(8) out of range must fail");
    CHECK(rb_used(&rb) == 8, "peek_at must NOT move tail");

    CHECK(rb_skip(&rb, 3) == 3, "skip 3");
    CHECK(rb_used(&rb) == 5, "used = 5 after skip 3, got %u", rb_used(&rb));
    CHECK(rb_skip(&rb, 100) == 5, "skip beyond used clamps to 5");
    CHECK(rb_is_empty(&rb), "empty after skip all");

    rb_flush(&rb);
    CHECK(rb_used(&rb) == 0, "flushed before wrap test");

    /* 跨回绕的 peek_at：写满 16 个，读掉 14 个，再写 4 个 —— head 必然绕过数组末尾 */
    for (uint32_t i = 0; i < 16u; i++) { rb_put(&rb, (uint8_t)(0xF0u + i)); }
    CHECK(rb_used(&rb) == 16, "filled again, used = %u", rb_used(&rb));
    int ok_get = 1;
    for (uint32_t i = 0; i < 14u; i++) { uint8_t t = 0; ok_get &= (rb_get(&rb, &t) == 1); }
    CHECK(ok_get, "get 14 during wrap setup");
    for (uint32_t i = 0; i < 4u;  i++) { rb_put(&rb, (uint8_t)(0x10u + i)); }
    CHECK(rb_used(&rb) == 6, "used = 6 after 16-14+4, got %u", rb_used(&rb));
    /* 现在 head 已经绕过数组末尾，peek_at 必须仍然按逻辑顺序给出正确字节 */
    int ok = 1;
    for (uint32_t i = 0; i < 6u; i++) {
        uint8_t v;
        if (rb_peek_at(&rb, i, &v) != 1) { ok = 0; break; }
        uint8_t want = (uint8_t)((i < 2u) ? (0xF0 + 14u + i) : (0x10 + i - 2u));
        if (v != want) { ok = 0; break; }
    }
    CHECK(ok, "peek_at correct across the wrap point");
}

/* ==================================================================
 * 用例 5：小 buffer 反复回绕
 * ================================================================ */
static void test_wrap_small(void)
{
    static uint8_t st[8];
    rb_t rb;
    uint8_t out[64];

    SECTION("wrap around a tiny 8-byte buffer");

    rb_init(&rb, st, sizeof st);

    /* 每次 put 1 个 get 1 个，tail/head 必然反复经过回绕点 */
    int ok = 1;
    for (uint32_t i = 0; i < 200u; i++) {
        uint8_t v = (uint8_t)(i * 7u + 1u);
        uint8_t g = 0;
        if (rb_put(&rb, v) != 1) { ok = 0; break; }
        if (rb_get(&rb, &g) != 1) { ok = 0; break; }
        if (g != v)               { ok = 0; break; }
    }
    CHECK(ok, "200 rounds of put/get across wrap points");

    /* 构造一次必然跨界的批量写 */
    rb_flush(&rb);
    uint8_t a[6] = {1, 2, 3, 4, 5, 6};
    uint8_t b[6] = {7, 8, 9, 10, 11, 12};
    uint8_t tmp[4];

    CHECK(rb_write(&rb, a, 6) == 6, "write 6");
    CHECK(rb_read(&rb, tmp, 4) == 4, "read 4");
    CHECK(tmp[0] == 1 && tmp[3] == 4, "read back 1..4");
    CHECK(rb_write(&rb, b, 6) == 6, "write 6 crossing the array end");
    CHECK(rb_used(&rb) == 8, "exactly full, used = %u", rb_used(&rb));

    uint32_t n = rb_read(&rb, out, sizeof out);
    const uint8_t want[8] = {5, 6, 7, 8, 9, 10, 11, 12};
    CHECK(n == 8, "read 8 out, got %u", n);
    CHECK(memcmp(out, want, 8) == 0, "byte sequence intact across wrap");
}

/* ==================================================================
 * 用例 6：head/tail 的 32 位溢出回绕（本方案最容易藏 bug 的地方）
 * ================================================================ */
static void test_overflow_wrap(void)
{
    static uint8_t st[16];
    rb_t rb;
    uint8_t in[16], out[16];

    SECTION("uint32 head/tail counter overflow (0xFFFFFFF8 -> 0x00000000)");

    rb_init(&rb, st, sizeof st);
    for (uint32_t i = 0; i < 16u; i++) { in[i] = (uint8_t)(i + 1u); }

    /* 人为把序号推到 2^32 前 8 个字节处 */
    rb.head = 0xFFFFFFF8u;
    rb.tail = 0xFFFFFFF8u;

    CHECK(rb_used(&rb) == 0, "used = 0 near overflow, got %u", rb_used(&rb));
    CHECK(rb_free(&rb) == 16, "free = 16 near overflow, got %u", rb_free(&rb));

    CHECK(rb_write(&rb, in, 16) == 16, "write 16");
    CHECK(rb.head == 8u, "head wrapped to 8, got %u", (unsigned)rb.head);
    CHECK(rb_used(&rb) == 16, "used still 16 after overflow, got %u", rb_used(&rb));
    CHECK(rb_is_full(&rb), "full after crossing 2^32");

    CHECK(rb_read(&rb, out, 16) == 16, "read 16");
    CHECK(rb.tail == 8u, "tail wrapped to 8, got %u", (unsigned)rb.tail);
    CHECK(memcmp(in, out, 16) == 0, "data intact across the 32-bit rollover");
    CHECK(rb_is_empty(&rb), "empty after drain");

    /* 再压一轮，确认账单没有因为回绕而算错 */
    int ok = 1;
    for (uint32_t round = 0; round < 3u; round++) {
        for (uint32_t i = 0; i < 16u; i++) {
            if (rb_put(&rb, in[i]) != 1) { ok = 0; }
        }
        if (rb_used(&rb) != 16u) { ok = 0; }
        for (uint32_t i = 0; i < 16u; i++) {
            uint8_t c = 0;
            if (rb_get(&rb, &c) != 1) { ok = 0; }
            if (c != in[i])           { ok = 0; }
        }
    }
    CHECK(ok, "3 extra rounds right after the rollover");
}

/* ==================================================================
 * 用例 7：DMA 场景 —— write_ptr + commit
 * ================================================================ */
static void test_dma_write_ptr(void)
{
    SECTION("DMA-style write_ptr + commit (for UART IDLE DMA)");

    /* 顺便验证 RB_STATIC_DEFINE 宏：编译期静态定义，连 rb_init 都不用调 */
    RB_STATIC_DEFINE(rb_dma, 16u);

    CHECK(rb_is_valid(&rb_dma),      "RB_STATIC_DEFINE produced a valid rb");
    CHECK(rb_capacity(&rb_dma) == 16, "static rb capacity = 16, got %u", rb_capacity(&rb_dma));
    CHECK(rb_is_empty(&rb_dma),       "static rb starts empty");

    uint32_t room = 0;
    uint8_t *p = rb_write_ptr(&rb_dma, &room);
    CHECK(p != NULL,        "write_ptr not null");
    CHECK(p == rb_dma.buf,  "first write_ptr points to buffer start");
    CHECK(room == 16,       "initial contiguous room = 16, got %u", room);

    /* 模拟 DMA 收到 5 个字节 */
    for (uint32_t i = 0; i < 5u; i++) { p[i] = (uint8_t)(0xA0u + i); }
    CHECK(rb_commit(&rb_dma, 5) == 5, "commit 5");
    CHECK(rb_used(&rb_dma) == 5, "used = 5, got %u", rb_used(&rb_dma));

    /* 连续可写空间应该缩短到 11（到数组末尾只剩那么多） */
    p = rb_write_ptr(&rb_dma, &room);
    CHECK(p == rb_dma.buf + 5, "write_ptr advanced by 5");
    CHECK(room == 11, "contiguous room shrinks to 11, got %u", room);

    /* 提交量超过剩余空间时会被截断，不许越界 */
    for (uint32_t i = 0; i < 11u; i++) { p[i] = (uint8_t)(0xB0u + i); }
    CHECK(rb_commit(&rb_dma, 100) == 11, "commit clamped to free space");
    CHECK(rb_is_full(&rb_dma), "full after commit");
    CHECK(rb_write_ptr(&rb_dma, &room) == NULL, "write_ptr is NULL when full");

    uint8_t out[16];
    CHECK(rb_read(&rb_dma, out, 16) == 16, "read all 16");
    CHECK(out[0] == 0xA0 && out[4] == 0xA4, "head segment A0..A4");
    CHECK(out[5] == 0xB0 && out[15] == 0xBA, "tail segment B0..BA");
}

/* ==================================================================
 * 用例 8：随机差分测试（对拍参考模型）
 * ================================================================ */
static void test_random_model(void)
{
    static uint8_t st[256];
    rb_t rb;

    SECTION("randomized differential test vs reference model");

    rb_init(&rb, st, sizeof st);
    model_reset();
    rnd_set(0x13572468u);

    uint32_t err_write = 0, err_read = 0, err_peek = 0, err_seq = 0;

    for (uint32_t i = 0; i < 200000u; i++) {
        uint32_t op = rnd() % 100u;
        uint32_t k  = (rnd() % 9u) + 1u;

        if (op < 45u) {                                  /* 生产 */
            uint32_t room = rb_free(&rb);
            uint32_t want = (k < room) ? k : room;
            if (model_used() + want >= MODEL_CAP) { break; }   /* 别把模型撑爆 */
            uint8_t tmp[9];
            for (uint32_t j = 0; j < want; j++) {
                tmp[j] = (uint8_t)(rnd() >> 13);
                model_put(tmp[j]);
            }
            if (rb_write(&rb, tmp, want) != want) { err_write++; }
        } else if (op < 90u) {                           /* 消费 */
            uint32_t used = rb_used(&rb);
            uint32_t want = (k < used) ? k : used;
            uint8_t tmp[9];
            uint32_t got = rb_read(&rb, tmp, want);
            if (got != want) { err_read++; }
            for (uint32_t j = 0; j < got; j++) {
                uint8_t m = 0;
                if (model_get(&m) == 0) { err_seq++; break; }
                if (m != tmp[j])        { err_seq++; }
            }
        } else {                                         /* 偷看，不许动 tail */
            uint32_t used = rb_used(&rb);
            if (used > 0u) {
                uint32_t off = rnd() % used;
                uint8_t  c   = 0;
                if (rb_peek_at(&rb, off, &c) == 0) { err_peek++; }
                else if (c != model_at(off))       { err_peek++; }
                if (rb_used(&rb) != used)          { err_peek++; }
            }
        }

        /* 每一步之后，存量都必须与模型一致 */
        if (rb_used(&rb) != model_used()) { err_seq++; break; }
    }

    CHECK(err_write == 0, "rb_write return-value mismatches: %u", err_write);
    CHECK(err_read  == 0, "rb_read  return-value mismatches: %u", err_read);
    CHECK(err_peek  == 0, "peek errors: %u", err_peek);
    CHECK(err_seq   == 0, "content/used mismatch vs model: %u", err_seq);

    /* 排空后逐字节比对残留内容 */
    uint32_t err_drain = 0, drained = 0;
    uint8_t c, m;
    while (rb_get(&rb, &c)) {
        if (model_get(&m) == 0) { err_drain++; break; }
        if (c != m)             { err_drain++; }
        drained++;
    }
    CHECK(err_drain == 0, "drain mismatch: %u byte(s)", err_drain);
    CHECK(rb_is_empty(&rb) && model_used() == 0, "both sides empty after drain");
    printf("    drained %u leftover bytes\n", drained);
}

/* ==================================================================
 * 用例 9：1MB 数据流 + CRC16 端到端（CP0 验收：零丢帧）
 * ================================================================ */
#define STREAM_LEN (1u << 20)          /* 1048576 字节 */
static uint8_t g_stream[STREAM_LEN];
static uint8_t g_sink[STREAM_LEN];

static void test_bulk_stream(void)
{
    static uint8_t st[1024];
    rb_t rb;

    SECTION("1MB stream through a 1KB ring buffer (CP0 acceptance)");

    rb_init(&rb, st, sizeof st);
    rnd_set(0xA5A5A5A5u);
    for (uint32_t i = 0; i < STREAM_LEN; i++) {
        g_stream[i] = (uint8_t)(rnd() >> 11);
    }

    uint32_t produced = 0, consumed = 0, loops = 0, err = 0;

    while (consumed < STREAM_LEN) {
        /* 生产者突发写入 1..96 字节 */
        uint32_t f = rb_free(&rb);
        uint32_t k = (rnd() % 96u) + 1u;
        if (k > f)                     { k = f; }
        if (k > STREAM_LEN - produced) { k = STREAM_LEN - produced; }
        if (k > 0u) {
            if (rb_write(&rb, g_stream + produced, k) != k) { err++; }
            produced += k;
        }

        /* 消费者随机长度读出 1..96 字节 */
        uint32_t u = rb_used(&rb);
        uint32_t m = (rnd() % 96u) + 1u;
        if (m > u) { m = u; }
        if (m > 0u) {
            uint32_t got = rb_read(&rb, g_sink + consumed, m);
            if (got != m) { err++; }
            consumed += got;
        }
        loops++;
    }

    CHECK(err == 0, "transport errors: %u", err);
    CHECK(produced == STREAM_LEN, "produced %u / %u", produced, STREAM_LEN);
    CHECK(consumed == STREAM_LEN, "consumed %u / %u", consumed, STREAM_LEN);
    CHECK(memcmp(g_stream, g_sink, STREAM_LEN) == 0, "byte-exact after 1MB through 1KB buffer");

    /* 再用自己写的 CRC16 交叉验证（顺带把 crc16 模块联动起来跑一遍） */
    uint16_t crc_ref = crc16_modbus(g_stream, 4096);
    uint16_t crc_out = crc16_modbus(g_sink, 4096);
    CHECK(crc_ref == crc_out, "CRC16 of stream vs sink: 0x%04X vs 0x%04X", crc_ref, crc_out);

    printf("    %u bytes in %u producer/consumer rounds, %.1f bytes/round\n",
           STREAM_LEN, loops, (double)STREAM_LEN / (double)loops);
}

/* ================================================================== */
int main(void)
{
    printf("=========================================\n");
    printf(" EdgeGateway-F4 :: ringbuf unit test\n");
    printf(" ringbuf v%d.%d   built %s %s\n",
           RB_VERSION_MAJOR, RB_VERSION_MINOR, __DATE__, __TIME__);
    printf("=========================================\n");

    test_init_capacity();
    test_fifo_order();
    test_full_no_wasted_slot();
    test_peek_and_skip();
    test_wrap_small();
    test_overflow_wrap();
    test_dma_write_ptr();
    test_random_model();
    test_bulk_stream();

    printf("\n=========================================\n");
    printf("  %d checks passed, %d failed\n", g_pass, g_fail);
    printf("  %s\n", (g_fail == 0) ? "*** ALL PASS ***" : "*** FAILED ***");
    printf("=========================================\n");

    return (g_fail == 0) ? 0 : 1;
}

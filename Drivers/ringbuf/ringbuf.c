#include "ringbuf.h"

#include <string.h>   /* memcpy */

/* =====================================================================
 * ringbuf.c —— SPSC 无锁环形缓冲实现
 *
 * 通篇记住一句话就够了：
 *     head 只有生产者改，tail 只有消费者改。
 * 所有「不需要加锁」的正确性都建立在这句话上。
 * ===================================================================== */

/* ---------------------------------------------------------------------
 * 内部小工具
 * ------------------------------------------------------------------ */

#define RB_MIN(a, b)  (((a) < (b)) ? (a) : (b))

/* 把单调递增的序号折成数组下标（用掩码代替取模） */
#define RB_WRAP(rb, idx)  ((idx) & (rb)->mask)

/* 参数防御：嵌入式里 NULL 解引用 = HardFault，宁可多花几个字节做检查 */
static int rb_ready(const rb_t *rb)
{
    return (rb != NULL)
        && (rb->buf != NULL)
        && (rb->size != 0u)
        && (((rb->size) & (rb->size - 1u)) == 0u);   /* 必须是 2 的幂 */
}

/* ---------------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------------ */

int rb_init(rb_t *rb, uint8_t *buf, uint32_t size)
{
    if ((rb == NULL) || (buf == NULL) || !RB_IS_POW2(size)) {
        return 0;
    }

    rb->buf  = buf;
    rb->size = size;
    rb->mask = size - 1u;
    rb->head = 0u;
    rb->tail = 0u;

    /* 初始化完成必须对另一个核/另一条控制流可见，之后才允许对方使用 */
    RB_BARRIER();
    return 1;
}

/* ---------------------------------------------------------------------
 * 状态查询
 * ------------------------------------------------------------------ */

int rb_is_valid(const rb_t *rb)
{
    return rb_ready(rb);
}

uint32_t rb_capacity(const rb_t *rb)
{
    return rb_ready(rb) ? rb->size : 0u;
}

uint32_t rb_used(const rb_t *rb)
{
    if (!rb_ready(rb)) {
        return 0u;
    }
    /* 无符号减法：即使 head 已经溢出回绕、比 tail 数值上更小，
     * (head - tail) 依然等于真实的字节数。这是本方案最关键的技巧。 */
    return (uint32_t)(rb->head - rb->tail);
}

uint32_t rb_free(const rb_t *rb)
{
    if (!rb_ready(rb)) {
        return 0u;
    }
    return rb->size - (uint32_t)(rb->head - rb->tail);
}

int rb_is_empty(const rb_t *rb)
{
    if (!rb_ready(rb)) {
        return 1;
    }
    return (rb->head == rb->tail);
}

int rb_is_full(const rb_t *rb)
{
    if (!rb_ready(rb)) {
        return 0;
    }
    return ((uint32_t)(rb->head - rb->tail) >= rb->size);
}

void rb_flush(rb_t *rb)
{
    if (!rb_ready(rb)) {
        return;
    }
    /* tail 追平 head 即可，不需要动 buf 里的内容 ——
     * 那些旧字节只是"无效数据"，下次写入会自然覆盖。 */
    rb->tail = rb->head;
    RB_BARRIER();
}

/* ---------------------------------------------------------------------
 * 生产者：put / write
 * ------------------------------------------------------------------ */

int rb_put(rb_t *rb, uint8_t c)
{
    if (!rb_ready(rb)) {
        return 0;
    }
    if ((uint32_t)(rb->head - rb->tail) >= rb->size) {
        return 0;                       /* 满 */
    }

    /* 生产顺序：先写数据（普通写，不越过 head） */
    rb->buf[RB_WRAP(rb, rb->head)] = c;

    /* release 屏障：保证上面的数据写 visible 之后，head 才更新。
     * 少了这一步，消费者可能看到新 head 却读到旧数据。 */
    RB_BARRIER();

    rb->head += 1u;                     /* 只有生产者写 head，无需原子读改写 */
    return 1;
}

uint32_t rb_write(rb_t *rb, const uint8_t *src, uint32_t n)
{
    uint32_t room;
    uint32_t pos;
    uint32_t first;
    uint32_t len;

    if (!rb_ready(rb) || (src == NULL) || (n == 0u)) {
        return 0u;
    }

    room = rb_free(rb);
    if (room == 0u) {
        return 0u;
    }
    if (n > room) {
        n = room;                       /* 部分写：能写多少写多少 */
    }

    /* 从 head 折出来的位置开始写。因为 size 可能放不下 n，要分两段：
     *   第一段写到数组末尾，第二段从数组开头继续（回绕）。
     */
    pos   = RB_WRAP(rb, rb->head);
    first = rb->size - pos;             /* 从 pos 到数组末尾还剩多少 */
    len   = RB_MIN(n, first);

    memcpy(rb->buf + pos, src, len);
    if (n > len) {
        /* 长度不为 0 才 memcpy，避免空指针+0 的形式 UB */
        memcpy(rb->buf, src + len, n - len);
    }

    RB_BARRIER();                       /* release：数据先落地 */

    rb->head += n;                      /* 单调递增，永不取模 */
    return n;
}

uint8_t *rb_write_ptr(rb_t *rb, uint32_t *room)
{
    uint32_t pos;

    if (!rb_ready(rb) || (room == NULL)) {
        return NULL;
    }
    if ((uint32_t)(rb->head - rb->tail) >= rb->size) {
        *room = 0u;
        return NULL;
    }

    /* DMA 只能写物理连续的一段，所以可用长度 = min(到数组末尾的距离, 剩余空间)。
     * 回绕的部分不能一次给出去，必须由调用方下一轮再取。 */
    pos   = RB_WRAP(rb, rb->head);
    *room = RB_MIN(rb->size - pos, rb_free(rb));
    return rb->buf + pos;
}

uint32_t rb_commit(rb_t *rb, uint32_t n)
{
    uint32_t room;

    if (!rb_ready(rb) || (n == 0u)) {
        return 0u;
    }
    room = rb_free(rb);
    if (n > room) {
        n = room;
    }

    RB_BARRIER();                       /* DMA 写完的数据必须先对 CPU 可见 */

    rb->head += n;
    return n;
}

/* ---------------------------------------------------------------------
 * 消费者：get / read / peek / skip
 * ------------------------------------------------------------------ */

int rb_get(rb_t *rb, uint8_t *c)
{
    if (!rb_ready(rb) || (c == NULL)) {
        return 0;
    }
    if (rb->head == rb->tail) {
        return 0;                       /* 空 */
    }

    *c = rb->buf[RB_WRAP(rb, rb->tail)];

    RB_BARRIER();                       /* acquire：先读数据，再推进 tail */

    rb->tail += 1u;                     /* 只有消费者写 tail */
    return 1;
}

uint32_t rb_read(rb_t *rb, uint8_t *dst, uint32_t n)
{
    uint32_t have;
    uint32_t pos;
    uint32_t first;
    uint32_t len;

    if (!rb_ready(rb) || (dst == NULL) || (n == 0u)) {
        return 0u;
    }

    have = (uint32_t)(rb->head - rb->tail);   /* 读 head（对方会改） */
    if (have == 0u) {
        return 0u;
    }

    /* acquire 屏障：拿到 head 快照之后，才允许去读那一块数据，
     * 防止编译器把 memcpy 重排到读 head 之前。 */
    RB_BARRIER();

    if (n > have) {
        n = have;
    }

    pos   = RB_WRAP(rb, rb->tail);
    first = rb->size - pos;
    len   = RB_MIN(n, first);

    memcpy(dst, rb->buf + pos, len);
    if (n > len) {
        memcpy(dst + len, rb->buf, n - len);
    }

    RB_BARRIER();                       /* 确保数据已拷走，再释放这块空间 */

    rb->tail += n;
    return n;
}

uint32_t rb_peek(const rb_t *rb, uint8_t *dst, uint32_t n)
{
    uint32_t have;
    uint32_t pos;
    uint32_t first;
    uint32_t len;

    if (!rb_ready(rb) || (dst == NULL) || (n == 0u)) {
        return 0u;
    }

    have = (uint32_t)(rb->head - rb->tail);
    if (have == 0u) {
        return 0u;
    }
    RB_BARRIER();

    if (n > have) {
        n = have;
    }

    /* 与 rb_read 完全一样，唯一区别是**不推进 tail** */
    pos   = RB_WRAP(rb, rb->tail);
    first = rb->size - pos;
    len   = RB_MIN(n, first);

    memcpy(dst, rb->buf + pos, len);
    if (n > len) {
        memcpy(dst + len, rb->buf, n - len);
    }
    return n;
}

int rb_peek_at(const rb_t *rb, uint32_t off, uint8_t *c)
{
    if (!rb_ready(rb) || (c == NULL)) {
        return 0;
    }
    if (off >= (uint32_t)(rb->head - rb->tail)) {
        return 0;
    }
    RB_BARRIER();

    /* tail + off 之后统一 wrap：跨越回绕点也不用手动分段，
     * 因为 (tail+off) 是单调递增序号，& mask 天然落在正确位置。 */
    *c = rb->buf[RB_WRAP(rb, (uint32_t)(rb->tail + off))];
    return 1;
}

uint32_t rb_skip(rb_t *rb, uint32_t n)
{
    uint32_t have;

    if (!rb_ready(rb) || (n == 0u)) {
        return 0u;
    }
    have = (uint32_t)(rb->head - rb->tail);
    if (n > have) {
        n = have;
    }
    RB_BARRIER();

    rb->tail += n;
    return n;
}

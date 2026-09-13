#ifndef RINGBUF_H
#define RINGBUF_H

/* =====================================================================
 * ringbuf.h —— SPSC 无锁环形缓冲（Lock-free Single Producer Single Consumer）
 *
 * EdgeGateway-F4 / Module M1 串口驱动框架的地基
 * 作者: 周雄伟   版本: 1.0.0   日期: 2026-09-13
 *
 * ---------------------------------------------------------------------
 * 一、为什么要用环形缓冲
 * ---------------------------------------------------------------------
 * 串口数据到来是「突发」的，处理数据是「匀速」的。中间必须有一块暂存区
 * 削峰填谷，否则快的那一边会丢数据。
 *
 *   中断(ISR) ----写----> [ 环形缓冲 ] ----读----> 任务(Task)
 *   生产者 Producer                              消费者 Consumer
 *
 * 对应到本项目：
 *   - 生产者：USART3 的 DMA + IDLE 中断回调（HAL_UARTEx_RxEventCallback）
 *   - 消费者：Task_ModbusRx（优先级 5），从缓冲里取帧并解析
 *
 * ---------------------------------------------------------------------
 * 二、为什么 SPSC 可以「无锁」
 * ---------------------------------------------------------------------
 * 无锁的前提只有一条：每个变量永远只有一方能写。
 *
 *   head（写指针）：只有生产者写，消费者只读
 *   tail（读指针）：只有消费者写，生产者只读
 *
 * 因为只有一方写，不存在「两个核同时改一个变量」的问题，所以不需要关中断、
 * 不需要互斥锁、也不会阻塞 ISR。这是嵌入式里最常用的一种无锁队列。
 *
 * 代价：**只支持 1 个生产者 + 1 个消费者**。
 * 一旦变成多生产者（比如两个串口往同一个缓冲里写），本模块不适用，
 * 必须改成 FreeRTOS 的队列（内部已经加了锁）。
 *
 * ---------------------------------------------------------------------
 * 三、关键设计：单调递增的 head / tail（参照 Linux kfifo）
 * ---------------------------------------------------------------------
 * 本实现不是常见的「head==tail 表示空、(head+1)%size==tail 表示满」教材写法。
 * 那种写法有两个缺点：
 *   1) 必须浪费一个格子，实际容量只有 size-1；
 *   2) 空和满的判定都要取模，容易写错边界。
 *
 * 本实现的做法是：
 *   - head / tail 是 uint32_t，从 0 开始**一直加，永不取模**
 *   - 真正落到数组的下标是  (head & mask) / (tail & mask)，其中 mask = size-1
 *   - 头尾之差就是存量：used = head - tail，free = size - used
 *   - 判空：head == tail      判满：head - tail == size
 *
 * 这样：
 *   (a) size 个格子能装 size 个字节，一格不浪费；
 *   (b) 取模运算被位运算替代，快；
 *   (c) head - tail 是无符号减法，即使 head 溢出回绕到 0，差值依然正确
 *       —— 这一点必须想清楚，见 test_ringbuf.c 的 overflow_wrap 用例。
 *
 * 代价：size 必须是 2 的幂。这在嵌入式里不是限制，
 * 反而说明你懂这个技巧。(rb_init 会拒绝非 2 的幂并返回 0)
 *
 * ---------------------------------------------------------------------
 * 四、volatile 与内存屏障（面试高频追问）
 * ---------------------------------------------------------------------
 * Q: head 声明成 volatile 还不够吗？为什么要 RB_BARRIER()？
 * A: volatile 只做两件事：
 *      1) 每次都真的去内存里读，不许编译器把值缓存进寄存器；
 *      2) 两次 volatile 访问之间不许被编译器重排。
 *    它**拦不住** CPU 硬件层面的重排，也不保证写缓冲（Write Buffer）被刷出去。
 *    单核 Cortex-M 上，编译器屏障通常就够用；
 *    但一旦有 **DMA 这个独立总线主设备**，或者你上了 **M7/H7 有 D-Cache**，
 *    就必须有真正的屏障，否则会读到旧数据。
 *
 * 两条铁律（顺序不能反）：
 *   写数据 -> RB_BARRIER() -> 更新 head
 *   读 head -> RB_BARRIER() -> 读数据
 * 这两句分别是 release / acquire 语义，绝不能调换顺序。
 *
 * 注意：STM32F7 / H7 有 D-Cache，DMA 缓冲区必须放在非 Cache 区，
 * 或者手动 SCB_CleanInvalidateDCache()。F4 没有 D-Cache，本项目暂不涉及。
 * ===================================================================== */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RB_VERSION_MAJOR 1
#define RB_VERSION_MINOR 0

/* ---------------------------------------------------------------------
 * 移植层：编译器相关的宏
 * ------------------------------------------------------------------ */

/* 判断一个数是不是 2 的幂：n 非 0 且只有一个 bit 为 1 */
#define RB_IS_POW2(n)   (((n) != 0u) && (((n) & ((n) - 1u)) == 0u))

/* 静态断言：编译期就把错误拦下来（比如 RB_STATIC_DEFINE 给了非 2 的幂） */
#define RB_CAT_(a, b) a##b
#define RB_CAT(a, b)  RB_CAT_(a, b)

#ifndef RB_STATIC_ASSERT
#  if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#    define RB_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#  else
     /* Keil armcc 5 / 老编译器没有 _Static_assert，用非法 typedef 兜底 */
#    define RB_STATIC_ASSERT(cond, msg) \
         typedef char RB_CAT(rb_static_assert_, __COUNTER__)[(cond) ? 1 : -1]
#  endif
#endif

/* 内存屏障
 * RB_BARRIER()  —— 完整顺序屏障 Seq-Cst：阻止编译器重排 + 生成硬件屏障指令
 *                  gcc/clang -> __atomic_thread_fence -> ARM 上是 dmb 指令
 *                  Keil armcc -> __dmb(0xF)
 *
 * 性能敏感的场景可以把 SPSC 路径降级为纯编译器屏障 RB_COMPILER_BARRIER()，
 * 前提是你确认平台是单核 + 无 D-Cache + 无 DMA 参与（本项目不满足，故用前者）。
 */
#ifndef RB_COMPILER_BARRIER
#  if defined(__GNUC__) || defined(__clang__)
#    define RB_COMPILER_BARRIER() __asm__ volatile ("" ::: "memory")
#  elif defined(__CC_ARM) || defined(__ARMCC_VERSION)
#    define RB_COMPILER_BARRIER() __schedule_barrier()
#  else
#    define RB_COMPILER_BARRIER() ((void)0)
#  endif
#endif

#ifndef RB_BARRIER
#  if defined(__GNUC__) || defined(__clang__)
#    define RB_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#  elif defined(__CC_ARM) || (defined(__ARMCC_VERSION) && !defined(__clang__))
#    define RB_BARRIER() __dmb(0xF)
#  elif defined(_MSC_VER)
#    define RB_BARRIER() _ReadWriteBarrier()
#  else
#    define RB_BARRIER() RB_COMPILER_BARRIER()
#  endif
#endif

/* ---------------------------------------------------------------------
 * 数据结构
 * ------------------------------------------------------------------ */

/**
 * 环形缓冲控制块。
 * 内存由调用者提供，本模块**绝不 malloc**，符合静态分配原则
 * （FreeRTOS 项目里所有对象都应该静态分配，避免堆碎片）。
 *
 * head/tail 为什么要 volatile：
 *   它们在 ISR 和任务两条控制流之间共享，编译器不知道会被"意外"修改。
 *   不加 volatile，编译器可能把 `while (rb->head == rb->tail);` 优化成死循环。
 */
typedef struct rb {
    uint8_t         *buf;    /* 数据区首地址（外部提供，不再由本模块分配） */
    uint32_t         size;   /* 容量，必须是 2 的幂                        */
    uint32_t         mask;   /* = size - 1，用于把递增序号折成数组下标      */
    volatile uint32_t head;  /* 写位置：单调递增，只有生产者改              */
    volatile uint32_t tail;  /* 读位置：单调递增，只有消费者改              */
} rb_t;

/* ---------------------------------------------------------------------
 * 定义方式（二选一）
 * ------------------------------------------------------------------ */

/**
 * 方式 A：运行期初始化（推荐用于 ISR 与任务共用的全局缓冲）
 *
 *   static uint8_t g_rx_storage[128];
 *   static rb_t    g_rx_rb;
 *
 *   void app_init(void) { rb_init(&g_rx_rb, g_rx_storage, sizeof g_rx_storage); }
 *
 * @param rb   控制块
 * @param buf  数据区，size 必须 >= size 字节
 * @param size 容量，**必须是 2 的幂**
 * @return 1 成功；0 失败（参数非法 或 size 不是 2 的幂）
 */
int rb_init(rb_t *rb, uint8_t *buf, uint32_t size);

/**
 * 方式 B：编译期静态定义（.bss 里一次性搞定，连 rb_init 都不用调）
 *
 *   RB_STATIC_DEFINE(g_rx_rb, 128);   // 生成一个名为 g_rx_rb 的环形缓冲
 *
 * 注意 size 必须是 2 的幂，写错的话编译期就报错。
 */
#define RB_STATIC_DEFINE(name, size_)                                        \
    RB_STATIC_ASSERT(RB_IS_POW2(size_), "rb size must be a power of 2");     \
    static uint8_t RB_CAT(name, _storage_)[(size_)];                         \
    static rb_t name = { RB_CAT(name, _storage_), (size_), (size_) - 1u, 0u, 0u }

/* ---------------------------------------------------------------------
 * 状态查询（生产者和消费者都可以调用）
 * ------------------------------------------------------------------ */

int      rb_is_valid  (const rb_t *rb);
uint32_t rb_capacity  (const rb_t *rb);   /* 总容量 == size                */
uint32_t rb_used      (const rb_t *rb);   /* 已存字节数 == head - tail     */
uint32_t rb_free      (const rb_t *rb);   /* 剩余可写字节数                */
int      rb_is_empty  (const rb_t *rb);
int      rb_is_full   (const rb_t *rb);
void     rb_flush     (rb_t *rb);         /* 丢弃全部数据，只能消费者调用   */

/* ---------------------------------------------------------------------
 * 生产者接口（只允许生产者调用）
 * ------------------------------------------------------------------ */

/**
 * 写入 1 个字节。
 * @return 1 成功；0 缓冲已满（调用方要决定是丢掉还是等下一次）
 */
int      rb_put  (rb_t *rb, uint8_t c);

/**
 * 批量写入，最多写 n 个字节。空间不足时**能写多少写多少**（部分写）。
 * @return 实际写入的字节数（0 表示一点都没写进去）
 */
uint32_t rb_write(rb_t *rb, const uint8_t *src, uint32_t n);

/**
 * 直接拿到可写的物理地址与连续长度，给 DMA 用。
 *
 * 用法（对应 UART DMA 环形模式）：
 *     uint32_t room;
 *     uint8_t *p = rb_write_ptr(&rb, &room);
 *     HAL_UARTEx_ReceiveToIdle_DMA(&huart3, p, room);
 *     // IDLE 中断回调里：rb_commit(&rb, Size);
 *
 * @param rb    控制块
 * @param room 输出参数：从返回地址开始，有多少字节是**物理连续的**可写空间
 * @return      可写区域首地址；缓冲无效或已满时返回 NULL
 */
uint8_t *rb_write_ptr(rb_t *rb, uint32_t *room);

/**
 * 提交 n 个字节（配合 rb_write_ptr 使用）。n 会被自动截断到剩余空间。
 * @return 实际提交的字节数
 */
uint32_t rb_commit(rb_t *rb, uint32_t n);

/* ---------------------------------------------------------------------
 * 消费者接口（只允许消费者调用）
 * ------------------------------------------------------------------ */

/**
 * 读出 1 个字节。 @return 1 成功；0 缓冲为空
 */
int      rb_get (rb_t *rb, uint8_t *c);

/**
 * 批量读出，最多读 n 个字节，数据不足时**有多少读多少**。
 * @return 实际读出的字节数
 */
uint32_t rb_read(rb_t *rb, uint8_t *dst, uint32_t n);

/**
 * 偷看（复制出来但**不移动 tail**）。
 *
 * 本项目为什么必须有它：Modbus RTU 靠「3.5 个字符的静默时间」分帧，
 * 任务拿到数据后，常常要先看前 2 个字节（从站地址 + 功能码）才知道这一帧
 * 总共多长、要不要继续等。如果直接 read 掉了就再也拼不回来。
 * 正确姿势：先 peek 判断长度够不够，够才真正 read。
 * @return 实际偷看到的字节数
 */
uint32_t rb_peek(const rb_t *rb, uint8_t *dst, uint32_t n);

/**
 * 偷看第 off 个字节（从当前 tail 算起，off 从 0 开始）。
 * @return 1 成功；0 越界
 */
int      rb_peek_at(const rb_t *rb, uint32_t off, uint8_t *c);

/**
 * 跳过（丢弃）n 个已读到的字节，比 read 到临时数组再扔掉更省事。
 * 用于：解析出一个非法帧后，把它整段丢掉。
 * @return 实际跳过的字节数
 */
uint32_t rb_skip(rb_t *rb, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* RINGBUF_H */

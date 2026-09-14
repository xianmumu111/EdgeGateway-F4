#include "mb_port_f1.h"
#include "ringbuf.h"

/* =====================================================================
 * mb_port_f1.c —— Modbus 从站的 STM32F1 硬件适配层实现
 * 作者: 周雄伟   版本: 1.0.0   日期: 2026-09-13
 *
 * ⚠️ 本文件依赖 HAL，PC 上编译不了，只在 MCU 工程里参与编译。
 * ===================================================================== */

/* ---------------------------------------------------------------------
 * 静态资源：全部静态分配，绝不 malloc
 * ------------------------------------------------------------------ */
#define DMA_BUF_SIZE   64u    /* DMA 单次接收缓冲 */
#define RB_SIZE        256u   /* 环形缓冲，必须是 2 的幂 */
#define FRAME_MAX      64u    /* 单帧最大长度 */

static uint8_t              g_dma_buf[DMA_BUF_SIZE];
static uint8_t              g_rb_storage[RB_SIZE];
static rb_t                 g_rx_rb;
static uint8_t              g_frame[FRAME_MAX];
static uint8_t              g_tx_buf[FRAME_MAX];
static uint16_t             g_regs[MB_REG_N];
static mb_slave_t           g_slave;
static UART_HandleTypeDef  *g_huart = NULL;

/* ---------------------------------------------------------------------
 * 启动一次 DMA + IDLE 接收
 * ---------------------------------------------------------------------
 * HAL_UARTEx_ReceiveToIdle_DMA 会自动开启：
 *   - DMA 传输完成（TC）中断
 *   - DMA 半满（HT）中断
 *   - 串口 IDLE 中断
 * 三者都会回调 HAL_UARTEx_RxEventCallback。
 *
 * ⚠️ 为什么要关掉 HT（半满）中断？
 * HT 触发时只收了一半数据，但 HAL 仍会回调 RxEventCallback。
 * 如果我们不分青红皂白把 size 个字节写进环形缓冲，
 * 等 DMA 真正收满时又会再写一次 —— 同一批字节被写了两遍，帧就废了。
 *
 * 所以：关掉 HT，只保留 TC（缓冲满）和 IDLE（总线静默）两种结束条件。
 * 这是网上很多例程没处理、导致"偶尔收双份数据"的根因。
 */
static void start_rx(void)
{
    if (g_huart == NULL) return;

    HAL_UARTEx_ReceiveToIdle_DMA(g_huart, g_dma_buf, DMA_BUF_SIZE);

    if (g_huart->hdmarx != NULL) {
        __HAL_DMA_DISABLE_IT(g_huart->hdmarx, DMA_IT_HT);
    }
}

/* ---------------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------------ */
void mb_port_init(UART_HandleTypeDef *huart, uint8_t addr)
{
    g_huart = huart;

    rb_init(&g_rx_rb, g_rb_storage, RB_SIZE);

    g_slave.addr   = addr;
    g_slave.regs   = g_regs;
    g_slave.regs_n = MB_REG_N;

    /* 初值：先给几个可辨识的数，方便用串口助手一眼确认通信正常 */
    g_regs[MB_REG_TEMP]    = 250u;    /* 25.0℃ */
    g_regs[MB_REG_LIGHT]   = 1024u;
    g_regs[MB_REG_ACC_X]   = 0u;
    g_regs[MB_REG_ACC_Y]   = 0u;
    g_regs[MB_REG_KEY_CNT] = 0u;
    g_regs[MB_REG_UPTIME]  = 0u;
    g_regs[MB_REG_ALARM]   = 0u;

    start_rx();
}

/* ---------------------------------------------------------------------
 * RxEventCallback 里调用：把收到的字节塞进环形缓冲
 * ---------------------------------------------------------------------
 * 注意这里只做"搬运"，不做解析。
 * 解析是慢活（要算 CRC、查寄存器），放在 ISR 里会拖慢整个系统。
 * 正确姿势：ISR 只搬数据，解析交给主循环。这是嵌入式的一条铁律。
 */
void mb_port_rx_event(UART_HandleTypeDef *huart, uint16_t size)
{
    if (huart != g_huart || size == 0u) return;

    rb_write(&g_rx_rb, g_dma_buf, size);

    /* 重新挂上接收。注意：ReceiveToIdle 是一次性的，回调里必须重启 */
    start_rx();
}

/* ---------------------------------------------------------------------
 * 主循环调用：取帧 -> 解析 -> 回包
 * ---------------------------------------------------------------------
 * ⚠️ 关于分帧的一个必须知道的事实：
 *   UART 的 IDLE 中断 = 串口线**空闲 1 个字符的时间**就触发；
 *   而 Modbus RTU 规定帧间隔是 **3.5 个字符**。
 *
 *   也就是说 IDLE 比协议要求的要"急"。
 *   在实验室里（PC 一次性发完一帧）IDLE 分帧完全够用；
 *   但在真实 RS485 总线上，若发送方字节间有抖动，可能一帧被切成两半。
 *
 *   工业级做法：IDLE 触发后不立即处理，而是记一个时间戳，
 *   在主循环里等满 t3.5（9600bps 下约 4ms）且期间没再来新字节，才判定帧结束。
 *   本项目先用 IDLE 直接分帧跑通，后续在网关侧补 t3.5 定时器。
 *   —— 面试被问"你的分帧可靠吗"，这就是加分答案。
 */
void mb_port_poll(void)
{
    if (g_huart == NULL) return;

    uint16_t n = (uint16_t)rb_used(&g_rx_rb);

    /* 最短帧是 4 字节（addr + fc + crc16），不够就继续等 */
    if (n < 4u) return;

    if (n > FRAME_MAX) n = FRAME_MAX;

    n = (uint16_t)rb_read(&g_rx_rb, g_frame, n);

    uint16_t rsp_len = 0u;
    mb_status_t st = mb_slave_handle(&g_slave, g_frame, n,
                                     g_tx_buf, sizeof g_tx_buf, &rsp_len);

    /* 只有 MB_OK 才发；MB_NO_REPLY / MB_BAD_FRAME 必须保持沉默 */
    if (st == MB_OK && rsp_len > 0u) {
        HAL_UART_Transmit(g_huart, g_tx_buf, rsp_len, 100u);
    }
}

/* ---------------------------------------------------------------------
 * 应用接口
 * ------------------------------------------------------------------ */
void mb_port_reg_set(uint16_t idx, uint16_t val)
{
    if (idx < MB_REG_N) g_regs[idx] = val;
}

uint16_t mb_port_reg_get(uint16_t idx)
{
    return (idx < MB_REG_N) ? g_regs[idx] : 0u;
}

const mb_slave_t *mb_port_slave(void)
{
    return &g_slave;
}

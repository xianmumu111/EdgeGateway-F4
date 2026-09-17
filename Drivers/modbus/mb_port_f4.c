/* =====================================================================
 * mb_port_f4.c —— STM32F4（探索者）RS485 物理层实现
 *
 * 依赖：
 *   huart2       —— RS485 串口（CubeMX 里配的 9600 8N1）
 *   PG8          —— RS485 DE（发送使能），高=发，低=收
 *
 * 约定：
 *   - 不解析任何 Modbus 字段，解析在 mb_master_parse 里
 *   - 收帧分两段：先 3 字节，再算总长收剩余
 * ===================================================================== */
 
 #include "mb_port_f4.h"
 #include "main.h"
 #include "ringbuf.h"
 
 /* CubeMX 生成的 UART 句柄（main.c 里定义，这里 extern 引用） */
 extern UART_HandleTypeDef huart2;
 /* DE 引脚。若 CubeMX 里给 PG8 起了别名，可换成对应宏。 */
 #define RS485_DE_PORT GPIOG
 #define RS485_DE_PIN GPIO_PIN_8
 
 #define RS485_TX() HAL_GPIO_WritePin(RS485_DE_PORT, RS485_DE_PIN, GPIO_PIN_SET)
 #define RS485_RX() HAL_GPIO_WritePin(RS485_DE_PORT, RS485_DE_PIN, GPIO_PIN_RESET)
 
 // 1) DMA 落地缓冲：一次 ReceiveToIdle 最多搬这么多
 #define MB_PORT_DMA_BUF_SZ 128
 static uint8_t s_dma_buf[MB_PORT_DMA_BUF_SZ];
 
 // 2) ringbuf 存储：2 的幂，且 >= 2 × DMA_BUF_SZ
RB_STATIC_DEFINE(s_rx_rb, 256);

 /*
 * 作用：初始化modbus_port
 *
 * 参数：无
 *
 * 返回值：无
 */
 void mb_port_init(void)
 {
	 RS485_RX();
	 // 启动 UART2 DMA 空闲接收，缓冲区 s_dma_buf，大小 MB_DMA_SIZE
	 HAL_UARTEx_ReceiveToIdle_DMA(&huart2,s_dma_buf,MB_PORT_DMA_BUF_SZ);
	 // 关闭 DMA 半传输中断，避免搬到一半就触发回调干扰正常收包
	 __HAL_DMA_DISABLE_IT(huart2.hdmarx,DMA_IT_HT);
 }
 
 /*
 * 作用：UART 空闲事件回调。当 DMA 接收因空闲中断或缓冲区满而结束时，
 *       把本次 DMA 收到的字节推入环形缓冲，然后重启 DMA 接收，
 *       并再次关闭半传输中断，防止帧被 HT 中断切碎。
 *
 * 参数：huart —— UART 句柄，用于判断是否为 USART2
 *       Size  —— 本次 DMA 接收到的字节数
 *
 * 返回值：无
 */
 void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
 {
	 if(huart ->Instance != USART2) return;
	 
	 /* TODO 1: 把这批字节推进环形缓冲（rb_write 返回实际写入数，可不管） */
	 rb_write(&s_rx_rb,s_dma_buf,Size);
	 
	 /* TODO 2: 重启 DMA 接收（Mode=Normal，跑完一轮就停） */
	 HAL_UARTEx_ReceiveToIdle_DMA(&huart2,s_dma_buf,MB_PORT_DMA_BUF_SZ);
	 
	 /* TODO 3: 再关一次半传输中断 —— 每次 HAL_DMA_Start_IT 都会重新打开 HT */
	 __HAL_DMA_DISABLE_IT(huart2.hdmarx,DMA_IT_HT); 
 }
 
 /*
 * 作用：从环形缓冲取 n 字节到 dst。在 timeout_ms 内凑齐 n 字节返回 1，
 *       超时仍未凑齐返回 0。内部循环调用 rb_read，因为调用那一刻缓冲里
 *       可能只有部分字节，一次 rb_read 不一定能读满。
 *
 * 参数：dst        —— 目标缓冲区，用于存放读出的字节
 *       n          —— 需要读取的字节数
 *       timeout_ms —— 超时时间，单位毫秒
 *
 * 返回值：1 —— 成功凑齐 n 字节
 *         0 —— 超时，未凑齐 n 字节
 */
 static int rb_take(uint8_t *dst, uint16_t n, uint32_t timeout_ms)
 {
	 uint32_t time_bigen = HAL_GetTick();
	 uint16_t got = 0;
	 
	 while(got < n)
	 {
		 got += rb_read(&s_rx_rb, dst + got, (uint16_t)(n - got));
		 if(got >= n) return 1;
		 if((HAL_GetTick() - time_bigen) >= timeout_ms) return 0;
	 }
	 return 1;
 }
 
 
 
 /*
 * 作用：
 *    Modbus 主站端口层传输函数。发送请求数据，并在超时时间内接收响应数据；
 *    返回实际接收长度和传输结果。
 *
 * 参数：
 *    req         [in]  请求数据缓冲区
 *    req_len     [in]  请求数据长度（字节）
 *    rsp         [out] 响应数据接收缓冲区
 *    rsp_max     [in]  响应缓冲区最大长度（字节）
 *    rsp_len     [out] 实际接收到的响应数据长度（字节）
 *    timeout_ms  [in]  接收超时时间（毫秒）
 *
 * 返回值：
 *    MB_M_OK          传输成功，收到响应
 *    其他 mb_m_err_t  参数错误、发送失败、接收失败、超时等错误
 */
 mb_m_err_t mb_port_transfer(const uint8_t *req, uint16_t req_len,
                            uint8_t *rsp, uint16_t rsp_max, uint16_t *rsp_len,
                            uint32_t timeout_ms)
 {
	 /* ---- 0) 参数检查 ---- */
	 if(!req || !rsp || !rsp_len || req_len == 0 ||rsp_max < 3)
		 return MB_M_BAD_ARG;
	 /* ---- 4 之前：丢弃上一轮残留字节 ----
     * S4 那步从站不回，但 DMA 一直在跑——万一从站晚到、或线路上有噪声，
     * 这些字节会留在缓冲里，把下一帧整体顶错位，症状是“偶尔 CRC 错”。
     * rb_flush 只允许消费者调用，这里在主循环上下文，合法。
     */
	 rb_flush(&s_rx_rb);
	 
	 /* ---- 1) DE 拉高：切到发送 ---- */
	 RS485_TX();
	 
	 /* ---- 2) 阻塞发送整帧 ---- */
    /* 100ms 只是发送上限；9600bps 下 8 字节 ≈ 8.4ms，绰绰有余 */
	 if(HAL_UART_Transmit(&huart2, (uint8_t *)req, req_len, 100) != HAL_OK)
	 {
		 RS485_RX();
		 return MB_M_TIMEOUT;
	 }
     /* ---- 3) DE 拉低：切到接收 ----
     * 不需要额外延时：HAL_UART_Transmit 内部会等 TC（发送完成）才返回，
     * 最后一个停止位已经出完，DE 可以立即拉低。
     */
	 RS485_RX();
	 
     /* ---- 4) 先收 3 个字节（地址 + 功能码 + 第 3 字节） ----
     * 3 字节足够 mb_master_rsp_len 判断正常帧还是异常帧，
     * 从而算出精确总长，避免“一次收 9 字节 + 从站只回 5 字节 → 卡满超时”。
     * 改用 rb_take：内部循环从环形缓冲取，凑齐 3 字节或超时才返回。
     */
	 if(!rb_take(rsp, 3, timeout_ms))
		 return MB_M_TIMEOUT;
	 
	 /* ---- 5) 用协议层算总长 ---- */
	 uint16_t total = mb_master_rsp_len(req, req_len, rsp, 3);
	 if(total == 0)
	 {
		 /* 功能码不认识 / req 太短 / head 不够 —— 当超时处理 */
		 return MB_M_TIMEOUT;
	 }
	 
	 /* ---- 6) 收剩余 total - 3 字节 ---- */
	 if(total > rsp_max)
	 return MB_M_NO_SPACE;
	 
	 if(total > 3)
	 {
		 /* 注意：timeout 是整段剩余字节的总超时，不是字节间超时。
         * 9600bps 下每字节 ≈ 1.04ms，最多再收 252 字节 ≈ 262ms，
         * 上层调用时给 200ms 起就够日常的 0x03/0x06 帧。
         */
		 if(!rb_take(rsp + 3,total - 3, timeout_ms))
			return MB_M_TIMEOUT; 
	 }
	 /* ---- 7) 回填长度 ---- */
	 *rsp_len = total;
	 return MB_M_OK;
 }
 

 
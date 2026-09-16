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
 
 /* CubeMX 生成的 UART 句柄（main.c 里定义，这里 extern 引用） */
 extern UART_HandleTypeDef huart2;
 /* DE 引脚。若 CubeMX 里给 PG8 起了别名，可换成对应宏。 */
 #define RS485_DE_PORT GPIOG
 #define RS485_DE_PIN GPIO_PIN_8
 
 #define RS485_TX() HAL_GPIO_WritePin(RS485_DE_PORT, RS485_DE_PIN, GPIO_PIN_SET)
 #define RS485_RX() HAL_GPIO_WritePin(RS485_DE_PORT, RS485_DE_PIN, GPIO_PIN_RESET)
 
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
     * 这是整个函数的灵魂：3 字节足够 mb_master_rsp_len 判断
     *   正常帧（功能码回显）还是异常帧（功能码最高位置 1），
     * 从而算出精确总长，避免"一次收 9 字节 + 从站只回 5 字节 → 卡满超时"。
     */
	 if(HAL_UART_Receive(&huart2, rsp, 3, timeout_ms) != HAL_OK)
		 return MB_M_TIMEOUT;
	 
	 /* ---- 5) 用协议层算总长 ---- */
	 uint16_t tota1 = mb_master_rsp_len(req, req_len, rsp, 3);
	 if(tota1 == 0)
	 {
		 /* 功能码不认识 / req 太短 / head 不够 —— 当超时处理 */
		 return MB_M_TIMEOUT;
	 }
	 
	 /* ---- 6) 收剩余 total - 3 字节 ---- */
	 if(tota1 > rsp_max)
	 return MB_M_NO_SPACE;
	 
	 if(tota1 > 3)
	 {
		 /* 注意：timeout 是整段剩余字节的总超时，不是字节间超时。
         * 9600bps 下每字节 ≈ 1.04ms，最多再收 252 字节 ≈ 262ms，
         * 上层调用时给 200ms 起就够日常的 0x03/0x06 帧。
         */
		 if(HAL_UART_Receive(&huart2,rsp + 3, (uint16_t)(tota1 - 3), timeout_ms) != HAL_OK)
			return MB_M_TIMEOUT; 
	 }
	 /* ---- 7) 回填长度 ---- */
	 *rsp_len = tota1;
	 return MB_M_OK;
 }
 
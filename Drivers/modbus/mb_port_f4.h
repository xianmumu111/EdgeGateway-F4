/* =====================================================================
 * mb_port_f4.h —— STM32F4（探索者）RS485 物理层
 *
 * 只做串口收发 + DE 方向控制，不做任何 Modbus 解析。
 * ===================================================================== */

#ifndef MB_PORT_F4_H
#define MB_PORT_F4_H

#include "mb_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一次完整的 Modbus RTU 收发：
 *   发 req[0..req_len-1]  →  收一帧响应到 rsp，长度写 *rsp_len
 *
 * 内部先收 3 字节，用 mb_master_rsp_len 推算总长，再收剩余字节。
 * 这样从站回 5 字节异常帧时不会卡满整个 timeout。
 *
 * 返回：
 *   MB_M_OK        收发成功，*rsp_len 有效
 *   MB_M_TIMEOUT   发送失败 / 首 3 字节没收到 / 剩余字节没收到
 *   MB_M_NO_SPACE  算出的总长超过 rsp_max
 *   MB_M_BAD_ARG   参数为空 / req_len == 0 / rsp_max < 3
 */
mb_m_err_t mb_port_transfer(const uint8_t *req, uint16_t req_len,
                            uint8_t *rsp, uint16_t rsp_max, uint16_t *rsp_len,
                            uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* MB_PORT_F4_H */

#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>

/* CRC-16/MODBUS
 * 多项式 poly = 0xA001 (即 0x8005 的位反转形式)
 * 初始值 init = 0xFFFF
 * 输入/输出均反转 (refin = refout = true)
 * 返回值是"逻辑值"，真正发到线上时要【低字节在前】
 */
uint16_t crc16_modbus(const uint8_t *buf, uint16_t len);

#endif

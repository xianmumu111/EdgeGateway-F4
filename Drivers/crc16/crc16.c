#include "crc16.h"

uint16_t crc16_modbus(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    for (uint16_t i = 0; i < len; i++) {
        crc ^= buf[i];                 /* 与当前字节异或 */

        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 0x0001)          /* 最低位为 1 */
                crc = (uint16_t)((crc >> 1) ^ 0xA001);
            else
                crc = (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

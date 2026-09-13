#include <stdio.h>
#include "crc16.h"

static int check(const uint8_t *f, uint16_t n, uint16_t expect, const char *name)
{
    uint16_t c = crc16_modbus(f, n);

    printf("%-10s crc = 0x%04X   on-wire: %02X %02X   expect 0x%04X   %s\n",
           name, c, (c & 0xFF), (c >> 8), expect,
           (c == expect) ? "PASS" : "FAIL");

    return (c == expect);
}

int main(void)
{
    /* Modbus 官方 spec 示例：从站 0x11，读保持寄存器，起始 0x006B，数量 3 */
    uint8_t req[] = {0x11, 0x03, 0x00, 0x6B, 0x00, 0x03};

    /* 同一示例的响应帧：11 03 06 AE41 5652 4340 + CRC 49 AD */
    uint8_t rsp[] = {0x11, 0x03, 0x06, 0xAE, 0x41, 0x56, 0x52, 0x43, 0x40};

    /* 用失败计数而不是 ok &= check(...)。
     * ok &= f() 时一旦某个 case 返回 0，后续结果与 0 相与恒为 0，
     * 而 0 又会让后面的结果永远是 0 —— 表面"测试通过"实则什么都没测。
     * 教训：布尔累加用 |= 或计数，别用 &=。 */
    int fail = 0;
    if (!check(req, (uint16_t)sizeof req, 0x8776, "request"))  { fail++; }
    if (!check(rsp, (uint16_t)sizeof rsp, 0xAD49, "response")) { fail++; }

    printf("\n%s\n", (fail == 0) ? "ALL PASS" : "FAILED");
    return fail;
}

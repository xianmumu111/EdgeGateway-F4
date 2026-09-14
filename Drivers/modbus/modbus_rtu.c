#include "modbus_rtu.h"
#include "../crc16/crc16.h"

/* =====================================================================
 * modbus_rtu.c —— Modbus RTU 从站协议层实现
 * 作者: 周雄伟   版本: 1.0.0   日期: 2026-09-13
 *
 * 设计原则（面试要能讲出来）：
 *   1. 绝不 malloc —— 嵌入式里静态分配，避免堆碎片
 *   2. 所有输入都当"敌人" —— 长度、地址、数量全部要校验
 *   3. 不做任何阻塞动作，函数执行时间有上界
 * ===================================================================== */

/* ---------------------------------------------------------------------
 * 工具函数
 * ------------------------------------------------------------------ */

void mb_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);    /* 高字节在前 */
    p[1] = (uint8_t)(v & 0xFFu);
}

uint16_t mb_get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

const char *mb_status_str(mb_status_t st)
{
    switch (st) {
    case MB_OK:        return "OK";
    case MB_NO_REPLY:  return "NO_REPLY";
    case MB_BAD_FRAME: return "BAD_FRAME";
    case MB_NO_SPACE:  return "NO_SPACE";
    default:           return "?";
    }
}

/* ---------------------------------------------------------------------
 * 内部：给一帧数据补上 CRC（低字节在前）
 * ------------------------------------------------------------------ */
static uint16_t mb_append_crc(uint8_t *frame, uint16_t len)
{
    uint16_t crc = crc16_modbus(frame, len);
    frame[len]     = (uint8_t)(crc & 0xFFu);   /* 低字节先发 */
    frame[len + 1] = (uint8_t)(crc >> 8);      /* 高字节后发 */
    return len + 2u;
}

/* ---------------------------------------------------------------------
 * 内部：组一个异常响应帧
 *
 *   addr | fc|0x80 | 异常码 | CRC_L | CRC_H     —— 固定 5 字节
 * ------------------------------------------------------------------ */
static uint16_t mb_build_exception(uint8_t *rsp, uint16_t rsp_max,
                                   uint8_t addr, uint8_t fc, uint8_t exc)
{
    if (rsp_max < 5u) return 0u;

    rsp[0] = addr;
    rsp[1] = (uint8_t)(fc | 0x80u);   /* 功能码最高位置 1 = 异常标志 */
    rsp[2] = exc;
    return mb_append_crc(rsp, 3u);
}

/* ---------------------------------------------------------------------
 * 内部：功能码 0x03 —— 读保持寄存器
 *
 * 请求（8 字节）：
 *   addr | 03 | start_h start_l | qty_h qty_l | CRC_L CRC_H
 * 响应：
 *   addr | 03 | byte_count | data... | CRC_L CRC_H
 *   byte_count = qty * 2
 * ------------------------------------------------------------------ */
static mb_status_t mb_fc_read_holding(mb_slave_t *s,
                                      const uint8_t *req, uint16_t req_len,
                                      uint8_t *rsp, uint16_t rsp_max,
                                      uint16_t *rsp_len)
{
    /* 长度不对就是坏帧：0x03 的请求永远是 8 字节 */
    if (req_len != 8u) return MB_BAD_FRAME;

    uint16_t start = mb_get_be16(&req[2]);
    uint16_t qty   = mb_get_be16(&req[4]);

    /* 数量校验：协议规定 1~125。
     * 注意这个顺序：先查数量（0x03），再查地址（0x02）。
     * Modbus 规范里数量不合法比地址越界"更根本"，
     * 主站通常靠这个区分是"我请求写错了"还是"你没这个寄存器"。 */
    if (qty < 1u || qty > MB_READ_REGS_MAX) {
        *rsp_len = mb_build_exception(rsp, rsp_max, s->addr, MB_FC_READ_HOLDING,
                                      MB_EX_ILLEGAL_VAL);
        return (*rsp_len > 0u) ? MB_OK : MB_NO_SPACE;
    }

    /* 地址越界校验。用 32 位做加法防止 start+qty 溢出：
     * 例如 start=0xFFF0、qty=125，16 位加法会回绕成小数字从而绕过检查。
     * 这是真实产品里出过事的经典漏洞。 */
    if ((uint32_t)start + (uint32_t)qty > (uint32_t)s->regs_n) {
        *rsp_len = mb_build_exception(rsp, rsp_max, s->addr, MB_FC_READ_HOLDING,
                                      MB_EX_ILLEGAL_ADDR);
        return (*rsp_len > 0u) ? MB_OK : MB_NO_SPACE;
    }

    uint16_t nbytes = (uint16_t)(qty * 2u);
    if ((uint32_t)3u + nbytes + 2u > rsp_max) return MB_NO_SPACE;

    rsp[0] = s->addr;
    rsp[1] = MB_FC_READ_HOLDING;
    rsp[2] = (uint8_t)nbytes;

    for (uint16_t i = 0u; i < qty; i++) {
        mb_put_be16(&rsp[3u + i * 2u], s->regs[start + i]);
    }

    *rsp_len = mb_append_crc(rsp, (uint16_t)(3u + nbytes));
    return MB_OK;
}

/* ---------------------------------------------------------------------
 * 内部：功能码 0x06 —— 写单个寄存器
 *
 * 请求（8 字节）：
 *   addr | 06 | reg_h reg_l | val_h val_l | CRC_L CRC_H
 * 响应：**原样回显前 6 字节** + CRC
 *   （这是协议规定，不是随便设计的 —— 主站靠回显确认"写进去的值"）
 * ------------------------------------------------------------------ */
static mb_status_t mb_fc_write_single(mb_slave_t *s,
                                      const uint8_t *req, uint16_t req_len,
                                      uint8_t *rsp, uint16_t rsp_max,
                                      uint16_t *rsp_len)
{
    if (req_len != 8u) return MB_BAD_FRAME;

    uint16_t reg = mb_get_be16(&req[2]);
    uint16_t val = mb_get_be16(&req[4]);

    if (reg >= s->regs_n) {
        *rsp_len = mb_build_exception(rsp, rsp_max, s->addr, MB_FC_WRITE_SINGLE,
                                      MB_EX_ILLEGAL_ADDR);
        return (*rsp_len > 0u) ? MB_OK : MB_NO_SPACE;
    }

    /* 真正写入。真实产品里这里要判断该寄存器是否只读、
     * 值是否在合法量程内，否则回 0x03 / 0x04。 */
    s->regs[reg] = val;

    if (rsp_max < 8u) return MB_NO_SPACE;

    /* 回显请求的前 6 个字节（addr fc reg val），再补 CRC */
    for (uint8_t i = 0u; i < 6u; i++) rsp[i] = req[i];
    *rsp_len = mb_append_crc(rsp, 6u);
    return MB_OK;
}

/* ---------------------------------------------------------------------
 * 核心：处理一个请求帧
 * ------------------------------------------------------------------ */
mb_status_t mb_slave_handle(mb_slave_t *s,
                            const uint8_t *req, uint16_t req_len,
                            uint8_t *rsp, uint16_t rsp_max,
                            uint16_t *rsp_len)
{
    *rsp_len = 0u;   /* 先清零，避免调用方读到上一次的残留长度 */

    if (s == NULL || req == NULL || rsp == NULL) return MB_BAD_FRAME;

    /* ---- 第 1 步：最短长度检查 ----
     * 地址(1) + 功能码(1) + CRC(2) = 4 字节，比这短一定是坏帧。
     * ⚠️ 这一步必须在读 req[0]、req[1] **之前**做，否则越界读。 */
    if (req_len < 4u) return MB_BAD_FRAME;

    /* ---- 第 2 步：CRC 校验 ----
     * 线上是低字节在前，所以要反过来拼回逻辑值：
     *   收到的两个字节是 [CRC_L][CRC_H] -> 逻辑值 = CRC_L | (CRC_H << 8) */
    uint16_t crc_recv = (uint16_t)(req[req_len - 2] | ((uint16_t)req[req_len - 1] << 8));
    uint16_t crc_calc = crc16_modbus(req, (uint16_t)(req_len - 2u));

    if (crc_recv != crc_calc) {
        if (s->stat_bad_crc != 0xFFFFFFFFu) s->stat_bad_crc++;
        /* CRC 错了连地址都不可信 —— 静默丢弃，一个字节都不回 */
        return MB_BAD_FRAME;
    }

    uint8_t addr = req[0];
    uint8_t fc   = req[1];

    /* ---- 第 3 步：地址判断 ---- */
    if (addr == MB_ADDR_BROADCAST) {
        /* 广播：所有从站都要执行写动作，但谁都不许回应
         * （否则总线上所有从站同时发数据 = 电平打架，全乱）
         * 本项目暂不支持广播写，仅做静默处理。 */
        if (s->stat_ignored != 0xFFFFFFFFu) s->stat_ignored++;
        return MB_NO_REPLY;
    }
    if (addr != s->addr) {
        /* 不是发给我的 —— 别插嘴。RS485 是半双工总线，
         * 一插嘴就和对面的响应撞在一起。 */
        if (s->stat_ignored != 0xFFFFFFFFu) s->stat_ignored++;
        return MB_NO_REPLY;
    }

    /* ---- 第 4 步：按功能码分发 ---- */
    mb_status_t st;
    switch (fc) {
    case MB_FC_READ_HOLDING:
        st = mb_fc_read_holding(s, req, req_len, rsp, rsp_max, rsp_len);
        break;
    case MB_FC_WRITE_SINGLE:
        st = mb_fc_write_single(s, req, req_len, rsp, rsp_max, rsp_len);
        break;
    default:
        /* 不认识的功能码 -> 异常 0x01 非法功能码 */
        *rsp_len = mb_build_exception(rsp, rsp_max, addr, fc, MB_EX_ILLEGAL_FUNC);
        st = (*rsp_len > 0u) ? MB_OK : MB_NO_SPACE;
        break;
    }

    /* ---- 第 5 步：统计 ---- */
    if (st == MB_OK) {
        /* 响应帧的功能码最高位是 1 => 这是异常帧 */
        if ((*rsp_len > 1u) && ((rsp[1] & 0x80u) != 0u)) {
            if (s->stat_except != 0xFFFFFFFFu) s->stat_except++;
        } else {
            if (s->stat_ok != 0xFFFFFFFFu) s->stat_ok++;
        }
    }
    return st;
}

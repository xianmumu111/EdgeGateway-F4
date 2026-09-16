/* =====================================================================
 * mb_master.c —— Modbus RTU 主站协议层（骨架，待填）
 *
 * 作者: 周雄伟   日期: 2026-09-16
 * 依赖: crc16.h
 *
 * 填完以后在 PC 上跑：
 *     cd test && build.bat
 * 看到 test_master 全绿再上板。
 * ===================================================================== */

#include "mb_master.h"
#include "crc16.h"

/* ---------------------------------------------------------------------
 * 内部小工具（这些给你写好了，直接用）
 * --------------------------------------------------------------------- */

/* 把一个 16 位数按【大端】写进 buf：高字节在前（Modbus 线上序） */
static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

/* 从 buf 按【大端】读出一个 16 位数 */
static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* CRC 校验：对 buf 的前 len-2 字节算 CRC，和末尾 2 字节比对
 * 注意线上是【低字节在前】，所以期望值是 (buf[len-1]<<8) | buf[len-2] */
static int crc_ok(const uint8_t *buf, uint16_t len)
{
    if (len < 2) return 0;
    uint16_t calc = crc16_modbus(buf, (uint16_t)(len - 2));
    uint16_t recv = (uint16_t)(((uint16_t)buf[len - 1] << 8) | buf[len - 2]);
    return calc == recv;
}

/* 在帧尾追加 CRC（低字节在前） */
static uint16_t append_crc(uint8_t *buf, uint16_t len)
{
    uint16_t c = crc16_modbus(buf, len);
    buf[len]     = (uint8_t)(c & 0xFF);   /* CRC 低字节先上线路 */
    buf[len + 1] = (uint8_t)(c >> 8);
    return (uint16_t)(len + 2);
}

/* --------------------------------------------------------------------- */

const char *mb_m_err_str(mb_m_err_t e)
{
    switch (e) {
    case MB_M_OK:        return "OK";
    case MB_M_CRC_ERR:   return "CRC error";
    case MB_M_ADDR_ERR:  return "slave addr mismatch";
    case MB_M_FC_ERR:    return "function code mismatch";
    case MB_M_LEN_ERR:   return "frame length invalid";
    case MB_M_EXCEPTION: return "slave returned exception";
    case MB_M_NO_SPACE:  return "output buffer too small";
    case MB_M_TIMEOUT:   return "timeout";
    case MB_M_BAD_ARG:   return "bad argument";
    default:             return "unknown";
    }
}

/* =====================================================================
 * TODO 1 : mb_master_build_read —— 组 0x03 读保持寄存器请求
 * =====================================================================
 * 目标帧（8 字节）：
 *     [0] slave 地址
 *     [1] 0x03
 *     [2][3] 起始寄存器号 start   ← 大端
 *     [4][5] 数量 qty             ← 大端
 *     [6][7] CRC（低字节在前）
 *
 * 步骤提示：
 *   1) 先查参数：buf 为空 / buf_max < 8 / qty == 0 / qty > 125  -> 返回 0
 *      （qty 上限 125 是 Modbus 规范：一次最多读 125 个保持寄存器。
 *        原因：响应帧最长 256 字节，5 + 2*125 = 255 刚好塞得下）
 *   2) 依次填 slave、0x03、put_be16(&buf[2], start)、put_be16(&buf[4], qty)
 *   3) return append_crc(buf, 6);
 *
 * 自查：填完应该能对上这个 golden vector（官方 spec）
 *      slave=0x11 start=0x006B qty=0x0003
 *      -> 11 03 00 6B 00 03 76 87
 */
uint16_t mb_master_build_read(uint8_t *buf, uint16_t buf_max,
                              uint8_t slave, uint16_t start, uint16_t qty)
{
    /* ↓↓↓ 在这里写 ↓↓↓ */
    if(!buf || buf_max < 8 || qty == 0 || qty > 125) return 0;
	
	buf[0] = slave;
	buf[1] = 0x03;
	put_be16(&buf[2],start);
	put_be16(&buf[4],qty);
	
	return append_crc(buf,6);
    /* ↑↑↑ 在这里写 ↑↑↑ */
}

/* =====================================================================
 * TODO 2 : mb_master_build_write_single —— 组 0x06 写单个寄存器请求
 * =====================================================================
 * 目标帧（8 字节）：
 *     [0] slave  [1] 0x06
 *     [2][3] 寄存器地址 addr  ← 大端
 *     [4][5] 要写入的值 val   ← 大端
 *     [6][7] CRC
 *
 * 和 TODO 1 几乎一样，只是 fc 不同、没有 qty 上限检查（0x06 写的是 1 个）。
 *
 * 自查：slave=0x01 addr=0x0000 val=0x00FF -> 01 06 00 00 00 FF C9 8A
 */
uint16_t mb_master_build_write_single(uint8_t *buf, uint16_t buf_max,
                                      uint8_t slave, uint16_t addr, uint16_t val)
{
    /* ↓↓↓ 在这里写 ↓↓↓ */
    if(!buf || buf_max < 8) return 0;
	
	buf[0] = slave;
	buf[1] = 0x06;
	put_be16(&buf[2],addr);
	put_be16(&buf[4],val);
	
	return append_crc(buf,6);
    /* ↑↑↑ 在这里写 ↑↑↑ */
}

/* =====================================================================
 * TODO 3 : mb_master_rsp_len —— 主站分帧的核心
 * =====================================================================
 * 输入：req（刚才发去的请求帧）、head（已收到的前几个字节）、head_len
 * 输出：这一帧总共应该有多少字节；信息不够就返回 0 让调用方继续收
 *
 * 步骤提示：
 *   1) 参数检查：req/head 为空、req_len < 2、head_len < 3 -> 返回 0
 *   2) 看 head[1]（响应里的功能码）：
 *        - 若 head[1] & 0x80  -> 异常帧，固定 5 字节，直接返回 5
 *        - 否则按请求的功能码 req[1] 分情况：
 *            0x03 : 正常响应 = 5 + 2*qty，其中 qty = get_be16(&req[4])
 *            0x06 : 正常响应 = 8（把请求原样回显）
 *            其它 : 不认识，返回 0
 *   3) ⚠️ 顺手想一下：算出 5+2*qty 之后要不要设个上限？
 *      如果 qty 是 125，响应就是 255 字节 —— 你的接收缓冲够大吗？
 *      （这一版先不管，等 port 层定缓冲区大小时再回来加断言）
 *
 * 自查向量：
 *      req  = 01 03 00 00 00 02 C4 0B
 *      head = 01 03 04          -> 应返回 9
 *      head = 01 83 02          -> 应返回 5（异常帧）
 *      head = 01 03             -> 应返回 0（才 2 字节，不够判断）
 */
uint16_t mb_master_rsp_len(const uint8_t *req, uint16_t req_len,
                           const uint8_t *head, uint16_t head_len)
{
    /* ↓↓↓ 在这里写 ↓↓↓ */
    if(!req || !head || req_len < 2 || head_len < 3) return 0; 
	
	/* 异常帧：功能码最高位置 1，固定 5 字节 */
	if(head[1] & 0x80) return 5;
	
	/* 正常响应，按请求功能码判断总长 */
	switch(req[1])
	{
		case 0x03:
		{
			if(req_len < 6) return 0;
			uint16_t qty = get_be16(&req[4]);
			/* 地址1 + 功能码1 + 字节数1 + 数据2*qty + CRC2 = 5 + 2*qty */
			return (uint16_t)(5 + 2 * qty);
		}
		case 0x06:
			/* 写单个寄存器正常响应就是原请求回显，固定 8 字节 */
			return 8;
		default:
			return 0;
	}
    /* ↑↑↑ 在这里写 ↑↑↑ */
}

/* =====================================================================
 * TODO 4 : mb_master_parse —— 校验 + 解析（最难的一个）
 * =====================================================================
 * 必须按顺序做这 6 步，顺序不能乱：
 *
 *   0) 参数检查：rsp 为空 / rsp_len < 5 / req_len < 2 -> MB_M_BAD_ARG
 *   1) CRC  ：crc_ok(rsp, rsp_len) 不过 -> MB_M_CRC_ERR
 *      ⚠️ CRC 必须第一个查。帧都坏了，后面每个字段都不可信。
 *   2) 地址 ：rsp[0] != req[0] -> MB_M_ADDR_ERR
 *      这是主站独有的检查（收到别的从站的迟到响应时抓得住）
 *   3) 功能码：
 *        rsp[1] == req[1]              -> 正常响应，走第 4 步
 *        rsp[1] == (req[1] | 0x80)     -> 异常帧：
 *              rsp_len 必须是 5，否则 MB_M_LEN_ERR
 *              *exc = rsp[2]，返回 MB_M_EXCEPTION
 *        其它                          -> MB_M_FC_ERR
 *   4) 按请求的功能码解析：
 *        0x03 :
 *            a) 字节计数 rsp[2] 必须 == 2 * qty（qty = get_be16(&req[4])），
 *               不等 -> MB_M_LEN_ERR
 *            b) rsp_len 必须 == 3 + rsp[2] + 2，不等 -> MB_M_LEN_ERR
 *            c) out_max < qty -> MB_M_NO_SPACE
 *            d) 循环：out[i] = get_be16(&rsp[3 + 2*i])，*out_n = qty
 *            e) 返回 MB_M_OK
 *        0x06 :
 *            a) rsp_len 必须 == 8，不等 -> MB_M_LEN_ERR
 *            b) 回显的地址 get_be16(&rsp[2]) 必须 == 请求的地址
 *               get_be16(&req[2])，不等 -> MB_M_ADDR_ERR
 *               （写错寄存器在工业现场是要出事的，必须回读确认）
 *            c) out_max < 1 -> MB_M_NO_SPACE
 *            d) out[0] = get_be16(&rsp[4])，*out_n = 1
 *            e) 返回 MB_M_OK
 *   5) 其余功能码 -> MB_M_BAD_ARG
 *
 * 自查向量（官方 spec，从站那边也用的同一组）：
 *      req = 11 03 00 6B 00 03 76 87
 *      rsp = 11 03 06 AE 41 56 52 43 40 49 AD
 *      -> MB_M_OK, out = {0xAE41, 0x5652, 0x4340}, out_n = 3
 *
 *      今日上板用的：
 *      req = 01 03 00 00 00 02 C4 0B
 *      rsp = 01 03 04 00 0A 00 14 DA 3E
 *      -> MB_M_OK, out = {10, 20}, out_n = 2
 *
 *      异常帧：
 *      req = 01 03 00 00 00 02 C4 0B
 *      rsp = 01 83 02 C0 F1
 *      -> MB_M_EXCEPTION, exc = 0x02
 */
mb_m_err_t mb_master_parse(const uint8_t *req, uint16_t req_len,
                           const uint8_t *rsp, uint16_t rsp_len,
                           uint16_t *out, uint16_t out_max, uint16_t *out_n,
                           uint8_t *exc)
{
    /* ↓↓↓ 在这里写 ↓↓↓ */
    if(!req || !rsp || !out || !out_n || !exc) return MB_M_BAD_ARG;
	if(rsp_len < 5 || req_len < 2) return MB_M_BAD_ARG;
	
	if(!crc_ok(rsp,rsp_len)) return MB_M_CRC_ERR;
	if(rsp[0] != req[0]) return MB_M_ADDR_ERR;
	
	/* 功能码判断 */
	if(rsp[1] == req[1])
	{ 
		/* 正常响应：什么都不做，继续往下解析 */
	}
		else if(rsp[1] == (uint8_t)(req[1] | 0x80))
		{
			if(rsp_len != 5) return MB_M_LEN_ERR;
			*exc = rsp[2];
			return MB_M_EXCEPTION;
		}
		else return MB_M_FC_ERR;
	/* 正常响应解析 */
		switch(req[1])
		{
			case 0x03:
				if(req_len < 6) return MB_M_BAD_ARG;
				{
					uint16_t qty = get_be16(&req[4]);
					if(rsp[2] != (uint8_t)(2 * qty)) return MB_M_LEN_ERR;
					if(rsp_len != (uint16_t)(3 + rsp[2] + 2)) return MB_M_LEN_ERR;
					if(out_max < qty) return MB_M_NO_SPACE;
					for(uint16_t i = 0; i < qty; i++)
					{
						out[i] = get_be16(&rsp[3 + 2 * i]);
					}
					*out_n = qty;
					return MB_M_OK;
				}
			case 0x06:
				if(req_len < 4) return MB_M_BAD_ARG;
				if(rsp_len != 8) return MB_M_LEN_ERR;
				if(get_be16(&rsp[2]) != get_be16(&req[2])) return MB_M_ADDR_ERR;
				if(out_max < 1) return MB_M_NO_SPACE;
				
				out[0] = get_be16(&rsp[4]);
				*out_n = 1;
				return MB_M_OK;
			
			default:
				return MB_M_BAD_ARG;
				
		}
    /* ↑↑↑ 在这里写 ↑↑↑ */
}

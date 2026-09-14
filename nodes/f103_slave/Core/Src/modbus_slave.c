#include "modbus_slave.h"
#include "usart.h"
#include <string.h>

/* =========================================================================
 * 调试开关
 *   1 = 用 printf 把收到的帧打到串口上（排错用）
 *   0 = 完全静默，串口只跑 Modbus 协议
 *
 *   注意：printf 和 Modbus 响应共用 USART1。开着调试时，主站（Modbus Poll /
 *   以后的 F407）收到的第一帧会是 "RX: ..." 这种 ASCII 垃圾，CRC 必然失败。
 *   所以只要一接主站，这里必须是 0。
 * ========================================================================= */
#define MODBUS_DEBUG 0

#if MODBUS_DEBUG
#include "stdio.h"
#define MB_LOG(...)  printf(__VA_ARGS__)
#else
#define MB_LOG(...)  ((void)0)
#endif

/* =========================================================================
 * 帧间隔 t3.5
 *   Modbus RTU 靠"总线静默 3.5 个字符时间"来判断一帧结束。
 *   1 个字符 = 起始位+8数据位+停止位 ≈ 10 bit，3.5 字符 = 35 bit
 *     t35(ms) = 35000 / 波特率
 *   波特率 > 19200 时，规范规定固定用 1.75ms（这里取 2ms 留余量）。
 *
 *   改波特率时记得同步改这个宏，否则 9600 下会卡在边界上误判帧结束。
 * ========================================================================= */
#ifndef MODBUS_BAUD
#define MODBUS_BAUD  9600
#endif

#if MODBUS_BAUD > 19200
#define MODBUS_T35_MS  2
#else
#define MODBUS_T35_MS  ((35000UL / MODBUS_BAUD) + 1)
#endif

#define MODBUS_BUF_SIZE 256

static uint8_t           rx_buf[MODBUS_BUF_SIZE];
static volatile uint16_t rx_len = 0;
static volatile uint32_t last_rx_tick = 0;

static uint8_t  frame[MODBUS_BUF_SIZE];   /* 主循环里的帧快照，见 modbus_poll 临界区 */
static uint16_t frame_len = 0;

#define REG_COUNT	8
static uint16_t regs[REG_COUNT] = {100,200,0,0,0,0,0,0};

/* =========================================================================
 * 函数声明区
 * ========================================================================= */
/* ---- 内部静态函数 ---- */
static uint16_t modbus_crc16(const uint8_t *buf, uint16_t len);   /* 算 CRC16 */
static int      modbus_crc_ok(const uint8_t *f, uint16_t len);    /* 校验帧尾 CRC */
static void     modbus_err(uint8_t fc, uint8_t code);             /* 发异常响应帧 */
static void     modbus_handle(uint8_t *req, uint16_t len);        /* 请求分发处理 */


void modbus_tick(void)
{
	regs[2]++;
	regs[3] = HAL_GetTick()/1000;
}

static uint16_t modbus_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for(uint16_t i = 0; i < len; i++)
    {
        crc ^= buf[i];
        for(uint8_t j = 0; j < 8; j++)
        {
            if(crc & 0x0001)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

void modbus_init(void)
{
    rx_len = 0;
    frame_len = 0;
    last_rx_tick = 0;
}

/* 在 USART1 接收中断里被调用（每来一个字节调一次） */
void modbus_rx_byte(uint8_t byte)
{
    if(rx_len < MODBUS_BUF_SIZE)
    {
        rx_buf[rx_len++] = byte;
    }
    else
    {
        rx_len = 0;   /* 缓冲溢出：整帧作废，从头再来，不能让 rx_len 卡死在 256 */
    }
    last_rx_tick = HAL_GetTick();
}

/* 在主循环里被反复调用 */
void modbus_poll(void)
{
    if(rx_len == 0)
        return;

    /* ---------------- 临界区开始 ----------------
     * "判断帧是否收完" 和 "把帧取走并清零" 这两件事必须一气呵成。
     * 如果分两步做：刚判断完"静默够了"，中断里又进来一个字节，
     * 我们就会把这一帧的前半截当成完整帧发出去响应 —— 帧就拆错了。
     *
     * 关中断期间 USART1 收不到字节吗？不是。硬件照收，但中断不响应，
     * 字节会停在 USART 的 DR 寄存器里。只要临界区够短（几十微秒），
     * 9600 波特率下 1 个字节要 1ms，绝对来得及，不会丢字节。
     * ---------------------------------------------- */
    __disable_irq();

    if((uint32_t)(HAL_GetTick() - last_rx_tick) < MODBUS_T35_MS)
    {
        __enable_irq();          /* 还没静默够，帧没收完，下次再来 */
        return;
    }

    frame_len = rx_len;
    memcpy(frame, (const void *)rx_buf, frame_len);
    rx_len = 0;

    __enable_irq();
    /* ---------------- 临界区结束 ---------------- */

    MB_LOG("RX: ");
    for(uint16_t i = 0; i < frame_len; i++)
    {
        MB_LOG("%02X ", frame[i]);
    }
    MB_LOG("\r\n");

    modbus_handle(frame, frame_len);

    frame_len = 0;
}

/*
 * 作用：校验 Modbus RTU 帧的 CRC16 是否正确
 * 参数：f 指向完整帧数据的指针；len 帧总长度（含末尾 2 字节 CRC）
 * 返回值：CRC 正确返回 1，错误返回 0
 */
static int modbus_crc_ok(const uint8_t *f, uint16_t len)
{
	uint16_t calc = modbus_crc16(f, len -2);	//除最后两个字节外全算
	uint16_t got = (uint16_t) (f[len -2] | (f[len -1] << 8));
	return (calc == got);
}

/*
 * 作用：发送 Modbus 异常响应帧
 * 参数：fc 原功能码；code 异常码（01非法功能 02非法地址 03非法值）
 * 返回值：无
 */
static void modbus_err(uint8_t fc, uint8_t code)
{
	uint8_t r[5];	/* 异常响应帧缓冲区：地址+功能码+异常码+CRC低+CRC高 */
	uint16_t c;
	
	r[0] = MODBUS_SLAVE_ADDRESS;	/* 从机地址 */
	r[1] = fc | 0x80;	/* 功能码最高位置1 = 异常标志 */
	r[2] = code;	/* 01非法功能 02非法地址 03非法值 */
	
	c = modbus_crc16(r, 3); 	/* 对前3字节计算 CRC16 */
	r[3] = c & 0xff;	 /* CRC 低字节在前 */
	r[4] = c >> 8;   	 /* CRC 高字节在后 */
	
	HAL_UART_Transmit(&huart1,r,5,100);		/* 通过串口1发送5字节异常响应帧 */
}

/*
 * 作用：处理接收到的 Modbus RTU 请求帧，解析功能码并执行相应操作
 * 参数：req 指向请求帧数据的指针；len 请求帧长度
 * 返回值：无
 */
static void modbus_handle(uint8_t *req, uint16_t len)
{
	uint8_t rsp[256];	/* 响应帧缓冲区 */
	
	/* n:响应长度  start:起始寄存器  qty:寄存器数量  i:循环变量  c:CRC值 */
	uint16_t n = 0, addr, qty, val, i, c;
	if(len < 4) return;		/* 连头都不全（最短帧：地址+功能码+CRC=4字节） */
	if(!modbus_crc_ok(req, len)) return;	 /* CRC错 → 沉默（Modbus规定不响应） */
	if(req[0] != MODBUS_SLAVE_ADDRESS) return;	/* 不是叫我 → 沉默（地址不匹配） */
	
    switch (req[1])                     /* 按功能码分发处理 */
    {
        /* ---------------- 读保持寄存器 ---------------- */
        case 0x03:
            /* 0x03 请求帧固定 8 字节：地址+功能码+起始地址(2)+数量(2)+CRC(2) */
            if (len != 8) return;

            addr = (uint16_t)((req[2] << 8) | req[3]);   /* 起始地址：高字节在前 */
            qty  = (uint16_t)((req[4] << 8) | req[5]);   /* 寄存器数量：高字节在前 */

            /* 数量合法性检查：1~125 个寄存器（Modbus 协议限制） */
            if (qty == 0 || qty > 125)
            {
                modbus_err(0x03, 0x03);                  /* 异常码 03：非法值 */
                return;
            }

            /* 越界检查必须用 32 位，防止 addr + qty 在 16 位下溢出绕回 */
            if ((uint32_t)addr + qty > REG_COUNT)
            {
                modbus_err(0x03, 0x02);                  /* 异常码 02：非法地址 */
                return;
            }

            /* 构造正常响应帧：地址 + 功能码 + 字节数 + 数据 + CRC */
            rsp[n++] = MODBUS_SLAVE_ADDRESS;             /* 从机地址 */
            rsp[n++] = 0x03;                             /* 功能码 */
            rsp[n++] = (uint8_t)(qty * 2);               /* 字节数！不是寄存器数（每寄存器 2 字节） */

            /* 逐个填入寄存器值，高字节在前 */
            for (i = 0; i < qty; i++)
            {
                rsp[n++] = (uint8_t)(regs[addr + i] >> 8);    /* 高字节先 */
                rsp[n++] = (uint8_t)(regs[addr + i] & 0xFF);  /* 低字节后 */
            }

            /* 计算并追加 CRC16（低字节在前，高字节在后） */
            c = modbus_crc16(rsp, n);
            rsp[n++] = (uint8_t)(c & 0xFF);              /* CRC 低字节 */
            rsp[n++] = (uint8_t)(c >> 8);                /* CRC 高字节 */

            HAL_UART_Transmit(&huart1, rsp, n, 100);     /* 发送响应帧，超时 100ms */
            return;

        /* ---------------- 写单个保持寄存器 ---------------- */
        case 0x06:
            /* 成功响应 = 原样回显请求帧 */
            /* 0x06 请求帧固定 8 字节：地址+功能码+寄存器地址(2)+写入值(2)+CRC(2) */
            if (len != 8) return;

            addr = (uint16_t)((req[2] << 8) | req[3]);   /* 寄存器地址：高字节在前 */
            val  = (uint16_t)((req[4] << 8) | req[5]);   /* 要写入的值，不是数量 */

            /* 地址越界检查：只写 1 个寄存器，直接判 addr 是否越界 */
            if (addr >= REG_COUNT)
            {
                modbus_err(0x06, 0x02);                  /* 异常码 02：非法地址 */
                return;
            }

            regs[addr] = val;                            /* 写入寄存器 */

            HAL_UART_Transmit(&huart1, req, 8, 100);     /* 成功：原样回显整个请求帧（含 CRC），协议规定 */
            break;

        /* ---------------- 未知功能码 ---------------- */
        default:
            modbus_err(req[1], 0x01);                    /* 异常码 01：非法功能 */
            return;
    }
}

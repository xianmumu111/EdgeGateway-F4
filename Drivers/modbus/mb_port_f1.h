#ifndef MB_PORT_F1_H
#define MB_PORT_F1_H

/* =====================================================================
 * mb_port_f1.h —— Modbus 从站的 STM32F1 硬件适配层
 *
 * 作者: 周雄伟   版本: 1.0.0   日期: 2026-09-13
 *
 * ---------------------------------------------------------------------
 * 为什么要单独分出这一层
 * ---------------------------------------------------------------------
 * modbus_rtu.c 是纯协议，不碰硬件，所以能在 PC 上测。
 * 但"字节从哪来、往哪发"最终还是要落到 UART 上 —— 这就是本文件的活。
 *
 *   硬件层(mb_port_f1.c)  ──喂字节──>  协议层(modbus_rtu.c)  ──出字节──>  硬件层发出去
 *     UART + DMA + IDLE                  纯 C，可测                    HAL_UART_Transmit
 *
 * 好处：以后把从站搬到 F407，或者改成走 RS485 / 走 TCP，
 * 只需要换掉这一层，协议层一个字都不用动。（面试时能讲出这个分层，
 * 说明你懂"可移植性"不是口号）
 *
 * ---------------------------------------------------------------------
 * ⚠️ 本文件依赖 HAL，PC 上编译不了
 * ---------------------------------------------------------------------
 * test/build.bat 不会编译它。它只在 Keil / CubeIDE 工程里参与编译。
 * ===================================================================== */

#include "modbus_rtu.h"
#include "stm32f1xx_hal.h"

/* ---------------------------------------------------------------------
 * 寄存器映射表（本项目从站对外暴露的数据点）
 *
 * 主站（F407 网关）会轮询这些寄存器。地址就是数组下标。
 * ------------------------------------------------------------------ */
#define MB_REG_TEMP      0u   /* 温度，单位 0.1℃ —— 250 表示 25.0℃  */
#define MB_REG_LIGHT     1u   /* 光照，ADC 原始值 0~4095             */
#define MB_REG_ACC_X     2u   /* 倾角 X（int16，0.1°）               */
#define MB_REG_ACC_Y     3u   /* 倾角 Y                              */
#define MB_REG_KEY_CNT   4u   /* 按键按下累计次数                    */
#define MB_REG_UPTIME    5u   /* 运行秒数（主循环每秒 +1）           */
#define MB_REG_ALARM     6u   /* 报警标志位：bit0=防拆 bit1=温度超限 */
#define MB_REG_N         8u   /* 寄存器总个数                        */

/**
 * 初始化：建环形缓冲、绑寄存器表、启动 DMA+IDLE 接收。
 * 必须在 CubeMX 生成的 MX_USART1_UART_Init() 之后调用。
 *
 * @param huart 串口句柄（本项目用 USART1）
 * @param addr  本从站地址 1~247
 */
void mb_port_init(UART_HandleTypeDef *huart, uint8_t addr);

/**
 * 在 HAL_UARTEx_RxEventCallback() 里调用。
 * 由硬件层把"收到了 size 个字节"这件事通知给本模块。
 */
void mb_port_rx_event(UART_HandleTypeDef *huart, uint16_t size);

/**
 * 主循环里不停调用：从环形缓冲取一帧 -> 交给协议层 -> 把响应发回去。
 * 非阻塞，没有数据就立刻返回。
 */
void mb_port_poll(void);

/** 由应用写传感器值（主循环采样后调用） */
void     mb_port_reg_set(uint16_t idx, uint16_t val);

/** 由应用读寄存器值（比如要在 OLED 上显示） */
uint16_t mb_port_reg_get(uint16_t idx);

/** 拿到从站对象，用于读统计（stat_ok / stat_bad_crc ...） */
const mb_slave_t *mb_port_slave(void);

#endif /* MB_PORT_F1_H */

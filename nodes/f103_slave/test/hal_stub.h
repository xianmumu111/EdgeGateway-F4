/* =========================================================================
 * hal_stub.h —— HAL 替身
 *
 * 目的：让 modbus_slave.c 一行都不改，就能在 PC 上编译运行。
 *
 * 原理：
 *   1) 本目录下的 usart.h 会转发到这里，屏蔽掉真实的 Core/Inc/usart.h
 *      （真实的那个会拖进 main.h -> 整个 HAL 库，PC 上编译不过）
 *   2) 编译【从站源码】时加两个宏（只作用在它身上，绝不能加给 harness.c）：
 *
 *        -Dprintf=mbtest_printf   把源码的调试打印接到虚拟线路上。
 *                                 MODBUS_DEBUG 忘了关时，用例会一起变红。
 *
 *        -D_INC_STDIO             跳过 stdio.h。MinGW 的 printf 是
 *                                 static inline 定义，一旦展开就会和
 *                                 mbtest_printf 的声明打架（DEBUG=1 时
 *                                 源码会 include stdio.h，必须挡掉）。
 * ========================================================================= */
#ifndef HAL_STUB_H
#define HAL_STUB_H

#include <stdint.h>
#include <string.h>

/* 假的串口句柄：仿真台只关心"发出了哪些字节"，不关心寄存器 */
typedef struct { int Instance; } UART_HandleTypeDef;

extern UART_HandleTypeDef huart1;

uint32_t HAL_GetTick(void);
int      HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *p, uint16_t n, uint32_t t);

/* 从站源码里的 printf 会被 -Dprintf=mbtest_printf 重定向到这里。
   本头文件刻意不 include <stdio.h>：MinGW 的 printf 是 static inline，
   一旦展开就会和这个声明打架。 */
int      mbtest_printf(const char *fmt, ...);

/* 关中断在单线程 PC 上无意义，留空即可（源码里的临界区逻辑照跑） */
#define __disable_irq()  do {} while (0)
#define __enable_irq()   do {} while (0)

#endif /* HAL_STUB_H */

#ifndef DWT_US_H
#define DWT_US_H

#include <stdint.h>

/* 调用方必须先 #include "main.h"（拿到 CMSIS 的 core_cm4.h），
 * 这里故意不 include MCU 头，是为了让 F1/F4 共用同一份文件。 */

/**
 * @brief 初始化 DWT 周期计数器
 *        必须在调用 dwt_cycles/dwt_us 之前执行一次
 */
static inline void dwt_init(void)
{
    /* 1. 使能调试跟踪单元
     *    CoreDebug->DEMCR 是调试异常与监控控制寄存器
     *    TRCENA 位:Trace Enable,置1后才能访问 DWT 等跟踪组件
     *    注意:单独写这一句可能被编译器优化掉或需要内存屏障 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

    /* 2. 清零周期计数器,从 0 开始计
     *    DWT->CYCCNT 是一个 32 位自由运行的计数器 */
    DWT->CYCCNT = 0;

    /* 3. 启动周期计数器
     *    DWT_CTRL 是 DWT 控制寄存器
     *    CYCCNTENA 位:使能 CYCCNT 计数,置1后每个内核时钟自增1 */
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/**
 * @brief 读取当前周期计数值
 * @return 从 dwt_init 以来经过的 CPU 周期数(32位,会溢出)
 */
static inline uint32_t dwt_cycles(void)
{
    return DWT->CYCCNT;
}

/**
 * @brief 将周期数换算为微秒数
 *
 * 计算流程:
 *   SystemCoreClock / 1000000UL  →  1 微秒等于多少个 CPU 周期
 *   例:主频 168 MHz → 168 cycles/us
 *
 *   CYCCNT / (cycles_per_us)  →  得到微秒数
 *
 * 精度:整数除法,丢小数,单次误差 < 1us
 * 溢出:32位 CYCCNT 在 168MHz 下约 25.5 秒后回绕一次
 */
static inline uint32_t dwt_us(void)
{
    return DWT->CYCCNT / (SystemCoreClock / 1000000UL);
}

/**
 * @brief 计算自 last 时刻起经过的微秒数
 * @param last 上一次 dwt_us() 的返回值
 * @return 相对时间差(us)
 *
 * 通过无符号减法自动处理 32 位回绕:
 *   只要两次采样间隔 < 2^32 us,结果就是正确的
 *   (168MHz 下最大可测量约 25.5 秒的间隔)
 */
static inline uint32_t dwt_elapsed_us(uint32_t last)
{
    return (uint32_t)(dwt_us() - last);
}
#endif /* DWT_US_H */

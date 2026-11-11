/**
 * @file rd91_math.h
 * @brief 纯 C 整数运算辅助（无浮点，便于主机侧单测和确定性的行为）。
 *
 * 为什么不用 sqrtf()：本工程用 -mfloat-abi=soft 编译，浮点全部走软件库；
 * 而这里只需要整数平方根，位数固定、结果可复现，也不会引入浮点库依赖。
 */
#ifndef RD91_MATH_H
#define RD91_MATH_H

#include <stdint.h>

/* 向下取整的整数平方根：Rd91_IsqrtU64(1000000)=1000。
 * 逐位算法，最多 32 轮；每次收到一个新样本调用一次，开销可以忽略。 */
static inline uint32_t Rd91_IsqrtU64(uint64_t n)
{
    uint64_t bit = (uint64_t)1U << 62;
    uint64_t res = 0U;

    while (bit > n)
    {
        bit >>= 2;
    }
    while (bit != 0U)
    {
        if (n >= res + bit)
        {
            n -= res + bit;
            res = (res >> 1) + bit;
        }
        else
        {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)res;
}

#endif /* RD91_MATH_H */

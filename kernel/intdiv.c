#include <stdint.h>

/* Core routine: quotient returned, remainder through *rem when non-NULL. */
uint64_t __udivmoddi4(uint64_t num, uint64_t den, uint64_t *rem)
{
    if (den == 0) {                 /* divide by zero: the caller is already
                                     * broken; give it something defined
                                     * rather than faulting the kernel */
        if (rem) *rem = 0;
        return 0;
    }
    if (num < den) {
        if (rem) *rem = num;
        return 0;
    }

    uint64_t quot = 0, r = 0;
    for (int bit = 63; bit >= 0; bit--) {
        r = (r << 1) | ((num >> bit) & 1u);
        if (r >= den) {
            r -= den;
            quot |= (uint64_t)1 << bit;
        }
    }
    if (rem) *rem = r;
    return quot;
}

uint64_t __udivdi3(uint64_t num, uint64_t den) { return __udivmoddi4(num, den, 0); }

uint64_t __umoddi3(uint64_t num, uint64_t den)
{
    uint64_t rem = 0;
    __udivmoddi4(num, den, &rem);
    return rem;
}

int64_t __divdi3(int64_t num, int64_t den)
{
    int neg = 0;
    uint64_t a, b;
    if (num < 0) { a = (uint64_t)(-(num + 1)) + 1u; neg = !neg; } else a = (uint64_t)num;
    if (den < 0) { b = (uint64_t)(-(den + 1)) + 1u; neg = !neg; } else b = (uint64_t)den;
    uint64_t q = __udivmoddi4(a, b, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t num, int64_t den)
{
    int neg = num < 0;
    uint64_t a, b, r = 0;
    if (num < 0) a = (uint64_t)(-(num + 1)) + 1u; else a = (uint64_t)num;
    if (den < 0) b = (uint64_t)(-(den + 1)) + 1u; else b = (uint64_t)den;
    __udivmoddi4(a, b, &r);
    return neg ? -(int64_t)r : (int64_t)r;
}

/* hw_wide.c -- see hw_wide.h. Pure C. */
#include "hw_wide.h"

#include <assert.h>

int32_t hwwide_sin(int32_t sin_fovy, int *ok)
{
    int64_t v = ((int64_t)sin_fovy * 5 + 2) >> 2;

    assert(ok != 0);
    if (sin_fovy <= 0 || v >= 4096) {
        *ok = 0;
        return sin_fovy;
    }
    *ok = 1;
    return (int32_t)v;
}

int32_t hwwide_aspect(int32_t aspect)
{
    int64_t v = ((int64_t)aspect * 5 + 2) >> 2;

    assert(aspect > 0);
    return v > INT32_MAX ? INT32_MAX : (int32_t)v;
}

float hwwide_sigma(float slider, int32_t m00, int32_t dist, float iod)
{
    if (dist <= 0 || m00 <= 0 || !(slider > 0.0f)) {
        return 0.0f;
    }
    if (slider > 1.0f) {
        slider = 1.0f;
    }
    return slider * ((float)m00 / 4096.0f) * (iod * 0.5f) / ((float)dist / 4096.0f);
}

int32_t hwwide_shear(int32_t x, int32_t w, int32_t dist, float e_sigma)
{
    double v = (double)x + (double)e_sigma * ((double)w - (double)dist);

    v = v < 0.0 ? v - 0.5 : v + 0.5;
    if (v >= 2147483647.0) {
        return INT32_MAX;
    }
    if (v <= -2147483648.0) {
        return INT32_MIN;
    }
    return (int32_t)v;
}

double hwwide_shear_sx(int32_t x, int32_t w, int32_t dist, float e_sigma)
{
    double ww = (double)((uint32_t)w & 0x00FFFFFFu);

    if (w <= 0 || ww == 0.0) {
        return 0.0;
    }
    return ((double)x + (double)e_sigma * (ww - (double)dist) + ww) * 128.0 / ww + 72.0;
}

int hwwide_shear_in_guard(int32_t w, int32_t dist, float e_sigma, int32_t guard_q12)
{
    double shear, bound;

    if (w <= 0) {
        return 1;
    }
    shear = (double)e_sigma * ((double)w - (double)dist);
    shear = shear < 0.0 ? -shear : shear;
    bound = (double)w * (6400.0 / 4096.0) * ((double)guard_q12 / 4096.0);
    return shear <= bound ? 1 : 0;
}

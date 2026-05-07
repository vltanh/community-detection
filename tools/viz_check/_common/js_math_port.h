/* fdlibm __ieee754_log port — V8-equivalent Math.log.
 *
 * V8's Math.log derives from fdlibm e_log.c. glibc's std::log differs by
 * 1 ulp on a non-trivial subset of inputs (~65/10014 measured on
 * x in [1, 1e6]). Algorithms whose JS port computes log on intermediate
 * doubles must route the canonical-side log through jsLog under
 * `-DTRACER_MODE` to get bit-equal evaluation between cpp + JS.
 *
 * Used by: WCC (IsWellConnected threshold via std::log), CM (chain WCC).
 * NOT used by: Leiden/Louvain/CC (audit row D = N/A no FP primitives in
 * their CPM/Mod paths). Infomap has its own copy at
 * `viz_check/infomap/instrumented/js_math_port.h` (different namespace).
 *
 * Source: https://www.netlib.org/fdlibm/e_log.c
 *
 * Bit-equal verified vs V8 Math.log on dense sweep across [1e-9, 1e9]
 * via the WCC pre-pipeline audit (row D).
 */
#ifndef VIZ_CHECK_JS_MATH_PORT_H_
#define VIZ_CHECK_JS_MATH_PORT_H_

#include <cstdint>
#include <cstring>

namespace viz_check {
namespace jsmath {

inline double jsLog(double x) {
    static constexpr double ln2_hi = 6.93147180369123816490e-01;
    static constexpr double ln2_lo = 1.90821492927058770002e-10;
    static constexpr double two54  = 1.80143985094819840000e+16;
    static constexpr double Lg1 = 6.666666666666735130e-01;
    static constexpr double Lg2 = 3.999999999940941908e-01;
    static constexpr double Lg3 = 2.857142874366239149e-01;
    static constexpr double Lg4 = 2.222219843214978396e-01;
    static constexpr double Lg5 = 1.818357216161805012e-01;
    static constexpr double Lg6 = 1.531383769920937332e-01;
    static constexpr double Lg7 = 1.479819860511658591e-01;
    static constexpr double zero = 0.0;
    auto hi_word = [](double v){ uint64_t b; std::memcpy(&b,&v,8); return (uint32_t)(b>>32); };
    auto lo_word = [](double v){ uint64_t b; std::memcpy(&b,&v,8); return (uint32_t)b; };
    auto set_hi  = [](double& v, uint32_t hi){ uint64_t b; std::memcpy(&b,&v,8); b=(b&0xFFFFFFFFULL)|((uint64_t)hi<<32); std::memcpy(&v,&b,8); };
    int32_t hx = (int32_t)hi_word(x);
    uint32_t lx = lo_word(x);
    int32_t k = 0;
    if (hx < 0x00100000) {
      if (((hx & 0x7fffffff) | (int32_t)lx) == 0) return -two54 / zero;
      if (hx < 0) return (x - x) / zero;
      k -= 54; x *= two54;
      hx = (int32_t)hi_word(x);
    }
    if (hx >= 0x7ff00000) return x + x;
    k += (hx >> 20) - 1023;
    hx &= 0x000fffff;
    int32_t i = (hx + 0x95f64) & 0x100000;
    set_hi(x, (uint32_t)(hx | (i ^ 0x3ff00000)));
    k += (i >> 20);
    double f = x - 1.0;
    double dk, hfsq, s, z, R, w, t1, t2;
    if ((0x000fffff & (2 + hx)) < 3) {
      if (f == zero) {
        if (k == 0) return zero;
        dk = (double)k; return dk * ln2_hi + dk * ln2_lo;
      }
      R = f * f * (0.5 - 0.33333333333333333 * f);
      if (k == 0) return f - R;
      dk = (double)k; return dk * ln2_hi - ((R - dk * ln2_lo) - f);
    }
    s = f / (2.0 + f);
    dk = (double)k;
    z = s * s;
    i = hx - 0x6147a;
    w = z * z;
    int32_t j = 0x6b851 - hx;
    t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    i |= j;
    R = t2 + t1;
    if (i > 0) {
      hfsq = 0.5 * f * f;
      if (k == 0) return f - (hfsq - s * (hfsq + R));
      return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
    } else {
      if (k == 0) return f - s * (f - R);
      return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
    }
}

} // namespace jsmath
} // namespace viz_check

#endif // VIZ_CHECK_JS_MATH_PORT_H_

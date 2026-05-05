// L2 alt: jsLog2 = jsLog (fdlibm-derived, bit-equal V8 Math.log) * Math.LOG2E.
// Compare against JS replacement: function jsLog2(x) = x===1?0: Math.log(x)*Math.LOG2E.
// If bit-equal across the same sweep, infomap's plogp can route through this
// path instead of needing an independent log2 port.
//
// Build: g++ -O2 -std=c++17 -ffp-contract=off -fno-fast-math L2_jsLog2_cpp.cpp -o L2_jsLog2_cpp

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <random>

// jsLog: fdlibm __ieee754_log port. Bit-equal V8 Math.log on ~94k sampled inputs
// per the SBM tracer's verification (community-detection/tools/viz_check/sbm/
// instrumented/flat_traced.cpp:81-148).
double jsLog(double x) {
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

// jsLog2: matches JS jsLog2 helper which routes through Math.log * Math.LOG2E.
// V8 exact: Math.LOG2E == 1.4426950408889634. Special case: log2(1) = 0.0 (exact).
double jsLog2(double x) {
    if (x == 1.0) return 0.0;
    return jsLog(x) * 1.4426950408889634;
}

static uint64_t bits(double x) { uint64_t b; std::memcpy(&b,&x,8); return b; }

int main(int argc, char** argv) {
    long N = (argc > 1) ? std::atol(argv[1]) : 100000;
    unsigned int seed = (argc > 2) ? static_cast<unsigned int>(std::atoll(argv[2])) : 7u;
    std::mt19937 mt(seed);
    std::printf("xbits,logbits\n");
    for (long i = 0; i < N; ++i) {
        double x;
        if (i % 3 == 0) {
            double r = static_cast<double>(mt()) / 4294967296.0;
            x = std::ldexp(1.0, static_cast<int>(-50.0 + r * 51.0));
        } else if (i % 3 == 1) {
            uint32_t u = static_cast<uint32_t>(mt());
            x = (u + 1.0) / 4294967297.0;
        } else {
            uint32_t u = static_cast<uint32_t>(mt());
            double m = 1.0 + (u % 1000000);
            x = 1.0 / (2.0 * m);
        }
        if (x <= 0) continue;
        double y = jsLog2(x);
        std::printf("%llu,%llu\n",
            (unsigned long long)bits(x),
            (unsigned long long)bits(y));
    }
    return 0;
}

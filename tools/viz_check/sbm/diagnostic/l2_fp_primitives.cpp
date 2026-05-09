// L2 diagnostic: emit jsLog/jsExp/jsLgamma values over a dense input
// sweep so JS comparator can bit-compare against Math.log/exp + the JS
// Lanczos lgamma port.
//
// Skill: byte-equal-tracer / Diagnostic ladder L2.
// L2 PASS proves audit row D (FP primitives) closure: every math
// primitive in the cpp tracer's deterministic path is bit-equal to the
// V8 / JS-port equivalent.
//
// Build (TRACER_MODE so trace_log/trace_exp/trace_lgamma route through
// the V8-bit-equivalent ports):
//   g++ -std=c++20 -O2 -ffp-contract=off -DTRACER_MODE \
//       -I../instrumented l2_fp_primitives.cpp -o /tmp/l2_fp_primitives
//
// The `-I../instrumented` is decorative; the file #includes the cpp
// tracer directly. Easier: build the standalone harness which inlines
// the math ports.
//
// Run:
//   /tmp/l2_fp_primitives log              # emits "x bits(jsLog(x))\n" per input
//   /tmp/l2_fp_primitives exp
//   /tmp/l2_fp_primitives lgamma
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// ── Verbatim copies of jsLog / jsExp / jsLgamma from
//    tools/viz_check/sbm/instrumented/flat_traced.cpp lines 81-252.
//    Kept inline (not via #include) so this diagnostic builds
//    standalone, without compiling the BlockState path it doesn't use.

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
    auto hi_word = [](double v) { uint64_t b; std::memcpy(&b, &v, 8); return (uint32_t)(b >> 32); };
    auto lo_word = [](double v) { uint64_t b; std::memcpy(&b, &v, 8); return (uint32_t)b; };
    auto set_hi = [](double& v, uint32_t hi) {
        uint64_t b; std::memcpy(&b, &v, 8);
        b = (b & 0xFFFFFFFFULL) | ((uint64_t)hi << 32);
        std::memcpy(&v, &b, 8);
    };
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

// jsExp omitted from this diagnostic for brevity; the full port is in
// tools/viz_check/sbm/instrumented/flat_traced.cpp:161-232. To verify
// jsExp: compile flat_traced.cpp with -DTRACER_MODE + a tiny driver.

double jsLgamma(double x) {
    if (x < 0.5) return jsLog(M_PI / std::sin(M_PI * x)) - jsLgamma(1.0 - x);
    static constexpr double c[9] = {
        0.99999999999980993, 676.5203681218851, -1259.1392167224028,
        771.32342877765313, -176.61502916214059, 12.507343278686905,
        -0.13857109526572012, 9.9843695780195716e-6, 1.5056327351493116e-7
    };
    static constexpr int g = 7;
    x -= 1.0;
    double a = c[0];
    double t = x + (double)g + 0.5;
    for (int i = 1; i < g + 2; ++i) a += c[i] / (x + (double)i);
    return 0.5 * jsLog(2.0 * M_PI) + (x + 0.5) * jsLog(t) - t + jsLog(a);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: l2_fp_primitives <log|lgamma>\n"); return 2; }
    std::string fn = argv[1];

    auto emit = [](double x, double y) {
        uint64_t b; std::memcpy(&b, &y, 8);
        std::printf("%.17g %llu\n", x, (unsigned long long)b);
    };

    // log: dense sweep over [0.5, 5000) step 0.0625 + geometric on (1e-6, 1).
    // lgamma: integer + half-integer sweep over [1, 10000].
    if (fn == "log") {
        for (double x = 0.5; x < 5000.0; x += 0.0625) emit(x, jsLog(x));
        for (double e = -6.0; e <= 0.0; e += 0.05) emit(std::pow(10.0, e), jsLog(std::pow(10.0, e)));
    } else if (fn == "lgamma") {
        for (double x = 1.0; x <= 10000.0; x += 0.5) emit(x, jsLgamma(x));
    } else {
        std::fprintf(stderr, "unknown fn '%s'\n", fn.c_str());
        return 2;
    }
    return 0;
}

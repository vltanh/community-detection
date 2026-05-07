/* Forked infomath.h with compile-time mode toggle (skill §"Compile-time
 * tracer-mode toggle"). One source, two build paths:
 *
 *   -DCANONICAL_MODE -> plogp routes through std::log2; build produces
 *                       a tracer that should be bit-equal to the
 *                       unmodified canonical Infomap pipeline (build-pair
 *                       equivalence test (a) of the byte-equal-tracer skill).
 *   -DTRACER_MODE    -> plogp routes through jsmath::jsLog2 (fdlibm
 *                       __ieee754_log * Math.LOG2E, bit-equal V8 Math.log).
 *                       This is the binary the JS production walker
 *                       cross-checks against (verification target).
 *
 * Header-guarded via INFOMATH_H_ so any subsequent canonical
 * infomath.h include from MapEquation.h / InfomapOptimizer.h is a
 * no-op; both modes resolve plogp through this overlay.
 *
 * Empirical findings (TRACER_MODE only):
 *   - `p * (log(p) * LOG2E)` widens the ULP gap between V8 and glibc
 *     compared to correctly-rounded log2; jsLog (fdlibm e_log.c) *
 *     LOG2E with the log2(1)=0 special case is the bit-tightest form
 *     verified at tools/viz_check/infomap/L2_log2/.
 */
#ifndef INFOMATH_H_
#define INFOMATH_H_

#if !defined(CANONICAL_MODE) && !defined(TRACER_MODE)
#error "infomath_traced.h: compile with -DCANONICAL_MODE or -DTRACER_MODE"
#endif
#if defined(CANONICAL_MODE) && defined(TRACER_MODE)
#error "infomath_traced.h: -DCANONICAL_MODE and -DTRACER_MODE are mutually exclusive"
#endif

#include <cmath>
#include <cstdlib>
#include "js_math_port.h"

namespace infomap {
namespace infomath {

  using std::log2;

  // plogp picks log2 backend at compile time; see header comment.
  inline double plogp(double p)
  {
#ifdef TRACER_MODE
    return p > 0.0 ? p * jsmath::jsLog2(p) : 0.0;
#else
    return p > 0.0 ? p * std::log2(p) : 0.0;
#endif
  }

  inline double isEqual(double a, double b, double tol = 1e-8)
  {
    return std::abs(a - b) <= tol;
  }

  /**
   * Tsallis entropy S_q of a uniform probability distribution of length n
   */
  inline double tsallisEntropyUniform(double n, double q = 1)
  {
    if (isEqual(q, 1)) {
      return std::log2(n);
    }
    return 1 / (q - 1) * (1 - pow(n, (1 - q))) / std::log(2);
  }

  /**
   * Interpolate from linear (q = 0) to log (q = 1)
   * linlog(k, 0) = k
   * linlog(k, 1) = log2(k)
   */
  inline double linlog(double k, double q = 1)
  {
    double baseCorrection = q <= 1 ? (1 - q) * std::log(2) + q : 1;
    double offsetCorrection = q <= 1 ? 1 - q : 0;
    return tsallisEntropyUniform(k, q) * baseCorrection + offsetCorrection;
  }

} // namespace infomath
} // namespace infomap

#endif // INFOMATH_H_

/* Forked infomath.h. Currently a verbatim copy of the canonical
 * header — the One-Definition-Rule mechanism is in place so any
 * future override of plogp lands in our build first (the canonical
 * header re-include inside MapEquation.h / InfomapOptimizer.h is
 * a no-op once INFOMATH_H_ is defined here).
 *
 * Empirical findings:
 *   - Switching plogp to `p * (log(p) * LOG2E)` widens the ULP gap
 *     between V8 and glibc rather than closing it; correctly-rounded
 *     log2 (which both libraries provide on x86_64 Linux) is the
 *     bit-tightest form.
 *   - The residual 1.78e-15 dnc drift is not log2-attributable; it
 *     comes from running-accumulator ULP noise over the ~4500-visit
 *     trajectory, not from the leaf-level transcendental.
 */
#ifndef INFOMATH_H_
#define INFOMATH_H_

#include <cmath>
#include <cstdlib>

namespace infomap {
namespace infomath {

  using std::log2;

  inline double plogp(double p)
  {
    return p > 0.0 ? p * log2(p) : 0.0;
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

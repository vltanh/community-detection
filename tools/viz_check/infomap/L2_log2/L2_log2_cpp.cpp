// L2 std::log2 vs Math.log2: dump (x_bits_uint64, log2x_bits_uint64) for a sweep
// of inputs covering the range plogp sees inside infomap's kernel hot path.
// Inputs cluster around (0, 1] (flow values are p_v = degree(v)/(2m), products
// of those, and sums up to ~1.0).
//
// Build: g++ -O2 -std=c++17 -ffp-contract=off -fno-fast-math L2_log2_cpp.cpp -o L2_log2_cpp

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <random>

static uint64_t bits(double x) {
    uint64_t b; std::memcpy(&b, &x, 8); return b;
}

int main(int argc, char** argv) {
    long N = (argc > 1) ? std::atol(argv[1]) : 100000;
    unsigned int seed = (argc > 2) ? static_cast<unsigned int>(std::atoll(argv[2])) : 7u;

    std::mt19937 mt(seed);
    // Inputs span [2^-50, 2.0], log-uniform plus dense linear samples.
    std::printf("xbits,logbits\n");
    for (long i = 0; i < N; ++i) {
        // Mix log-uniform and uniform-in-[0,1] sampling.
        double x;
        if (i % 3 == 0) {
            // Log-uniform in [2^-50, 2.0]
            double r = static_cast<double>(mt()) / 4294967296.0;
            x = std::ldexp(1.0, static_cast<int>(-50.0 + r * 51.0));
        } else if (i % 3 == 1) {
            // Uniform in (0, 1]
            uint32_t u = static_cast<uint32_t>(mt());
            x = (u + 1.0) / 4294967297.0;
        } else {
            // Dense around 1/(2m) where m varies up to 1e6
            uint32_t u = static_cast<uint32_t>(mt());
            double m = 1.0 + (u % 1000000);
            x = 1.0 / (2.0 * m);
        }
        if (x <= 0) continue;
        double y = std::log2(x);
        std::printf("%llu,%llu\n",
            static_cast<unsigned long long>(bits(x)),
            static_cast<unsigned long long>(bits(y)));
    }
    return 0;
}

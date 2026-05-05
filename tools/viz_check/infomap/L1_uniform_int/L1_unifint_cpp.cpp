// L1 uniform_int_distribution<unsigned int> parity:
//   - shared random_test_seq: a stream of (min, max) pairs derived from a
//     SEPARATE std::mt19937 stream so the test inputs are reproducible.
//   - for each pair, draw via std::uniform_int_distribution<unsigned int>
//     using the kernel's std::mt19937(seed). After the draw, also record
//     how many raw mt() calls were consumed (rejection-sampler retries).
//   - emits CSV rows: min,max,result,raw_consumed
// JS leg replays the same seed + same (min, max) sequence against
// LOUVAIN.MT19937(seed) + uniformInt() and asserts bit-equal.
//
// Build: g++ -O2 -std=c++17 -ffp-contract=off L1_unifint_cpp.cpp -o L1_unifint_cpp

#include <cstdio>
#include <cstdlib>
#include <random>

int main(int argc, char** argv) {
    unsigned int seed = (argc > 1) ? static_cast<unsigned int>(std::atoll(argv[1])) : 1u;
    long N = (argc > 2) ? std::atol(argv[2]) : 100000;

    // Kernel RNG: the one infomap uses.
    std::mt19937 mt(seed);

    // Test-input RNG: a separate fixed-seed stream so the test sequence
    // is reproducible across cpp + JS.
    std::mt19937 inputs(0xCAFEBABEu);

    // Wrap mt() to count raw consumes between distribution draws.
    long total_raw_consumed = 0;
    auto counted = [&]() -> unsigned int {
        ++total_raw_consumed;
        return static_cast<unsigned int>(mt());
    };
    // std::uniform_int_distribution needs a UniformRandomBitGenerator.
    // Wrap counted in a thin adapter.
    struct Adapter {
        std::mt19937& mt; long& counter;
        using result_type = unsigned int;
        static constexpr result_type min() { return std::mt19937::min(); }
        static constexpr result_type max() { return std::mt19937::max(); }
        result_type operator()() { ++counter; return static_cast<unsigned int>(mt()); }
    };
    Adapter adapter{mt, total_raw_consumed};

    std::printf("min,max,result,raw_consumed_this_draw\n");
    for (long i = 0; i < N; ++i) {
        // Pick min in [0, 2^16), max in [min, 2^17). Mix of small + larger ranges.
        unsigned int a = static_cast<unsigned int>(inputs()) & 0xFFFFu;
        unsigned int b = static_cast<unsigned int>(inputs()) & 0x1FFFFu;
        if (b < a) b = a;
        long before = total_raw_consumed;
        std::uniform_int_distribution<unsigned int> dist(a, b);
        unsigned int r = dist(adapter);
        long consumed = total_raw_consumed - before;
        std::printf("%u,%u,%u,%ld\n", a, b, r, consumed);
    }
    return 0;
}

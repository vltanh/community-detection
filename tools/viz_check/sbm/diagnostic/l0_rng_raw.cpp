// L0 diagnostic: dump first N raw outputs of std::mt19937(seed).
// JS comparator (l0_rng_raw.mjs) seeds COMDET.LOUVAIN.MT19937(seed) and
// asserts byte-equality with the cpp stream.
//
// Skill: byte-equal-tracer / Diagnostic ladder L0.
// L0 PASS proves audit row A (RNG byte stream) closure: same family,
// same init recurrence, same tempering.
//
// Build:
//   g++ -std=c++17 -O2 l0_rng_raw.cpp -o /tmp/l0_rng_raw
// Run:
//   /tmp/l0_rng_raw <seed> <N>            # prints N decimal uint32 lines
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: l0_rng_raw <seed> <N>\n"); return 2; }
    uint32_t seed = (uint32_t)std::strtoul(argv[1], nullptr, 10);
    int N = std::atoi(argv[2]);
    std::mt19937 mt(seed);
    for (int i = 0; i < N; ++i) std::printf("%u\n", (unsigned)mt());
    return 0;
}

// L0 RNG raw stream: print first N raw uint32 from std::mt19937(seed) one per line.
// Mirrors infomap/src/utils/Random.h: RandGen = std::mt19937; seed via Random(seed).m_randGen(seed).
// Build: g++ -O2 -std=c++17 -ffp-contract=off L0_rng_cpp.cpp -o L0_rng_cpp
// Usage: ./L0_rng_cpp <seed> <N>

#include <cstdio>
#include <cstdlib>
#include <random>

int main(int argc, char** argv) {
    unsigned int seed = (argc > 1) ? static_cast<unsigned int>(std::atoll(argv[1])) : 1u;
    long N = (argc > 2) ? std::atol(argv[2]) : 1000;
    std::mt19937 mt(seed);
    for (long i = 0; i < N; ++i) {
        std::printf("%u\n", static_cast<unsigned int>(mt()));
    }
    return 0;
}

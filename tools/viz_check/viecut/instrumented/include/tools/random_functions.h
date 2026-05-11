/******************************************************************************
 * random_functions.h (instrumented)
 *
 * Sibling reimpl of VieCut/lib/tools/random_functions.h with stderr [TRACE-RNG]
 * emission on every next() / nextInt() / nextBool() / nextDouble() call.
 * Build the instrumented binary with -I .../instrumented/include ahead of
 * VieCut/lib so this header takes precedence.
 *
 * Trace format (one line per call):
 *   [TRACE-RNG] kind:next   val:0xXXXXXXXX
 *   [TRACE-RNG] kind:nextInt lb:N rb:M val:K
 *   [TRACE-RNG] kind:nextBool val:0|1
 *   [TRACE-RNG] kind:nextDouble lb:F rb:F val:F
 *   [TRACE-RNG] kind:setSeed val:N
 *
 * The JS replay reads these lines into an oracle queue keyed by call kind +
 * args. Insertion order = consumption order (no multi-thread interleave; we
 * disable OpenMP at link time for the tracer).
 *
 * Verbatim API surface from upstream; only the bodies of the public statics
 * gain stderr emit + the underlying state behaviour is unchanged.
 *****************************************************************************/

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <random>
#include <utility>
#include <vector>

#include "common/definitions.h"
#include "tlx/logger.hpp"

typedef std::mt19937 MersenneTwister;
static int m_seed;
static MersenneTwister m_mt;

class random_functions {
 public:
    random_functions() { }
    virtual ~random_functions() { }

    template <typename sometype>
    static void circular_permutation(std::vector<sometype>* v) {
        std::vector<sometype>& vec = *v;
        if (vec.size() < 2) return;
        for (unsigned int i = 0; i < vec.size(); i++) vec[i] = i;
        unsigned int size = vec.size();
        std::uniform_int_distribution<unsigned int> A(0, size - 1);
        std::uniform_int_distribution<unsigned int> B(0, size - 1);
        for (unsigned int i = 0; i < size; i++) {
            unsigned int posA = A(m_mt);
            unsigned int posB = B(m_mt);
            while (posB == posA) posB = B(m_mt);
            if (posA != vec[posB] && posB != vec[posA]) std::swap(vec[posA], vec[posB]);
        }
    }

    template <typename sometype>
    static void permutate_vector_fast(std::vector<sometype>* v, bool init) {
        std::vector<sometype>& vec = *v;
        if (init) for (unsigned int i = 0; i < vec.size(); i++) vec[i] = i;
        if (vec.size() < 10) return;
        int distance = 20;
        std::uniform_int_distribution<unsigned int> A(0, distance);
        unsigned int size = vec.size() - 4;
        for (unsigned int i = 0; i < size; i++) {
            unsigned int posA = i;
            unsigned int posB = (posA + A(m_mt)) % size;
            std::swap(vec[posA], vec[posB]);
            std::swap(vec[posA + 1], vec[posB + 1]);
            std::swap(vec[posA + 2], vec[posB + 2]);
            std::swap(vec[posA + 3], vec[posB + 3]);
        }
    }

    template <typename sometype>
    static void permutate_vector_local(std::vector<sometype>* v, bool init) {
        std::vector<sometype>& vec = *v;
        if (init) for (unsigned int i = 0; i < vec.size(); i++) vec[i] = i;
        size_t localsize = 128;
        for (size_t i = 0; i < vec.size(); i += localsize) {
            size_t end = std::min(vec.size(), i + localsize);
            std::shuffle(vec.begin() + i, vec.begin() + end, m_mt);
        }
    }

    template <typename sometype>
    static void permutate_vector_good(std::vector<sometype>* v) {
        std::vector<sometype>& vec = *v;
        unsigned int size = vec.size();
        if (size < 4) return;
        std::uniform_int_distribution<unsigned int> A(0, size - 4);
        std::uniform_int_distribution<unsigned int> B(0, size - 4);
        for (unsigned int i = 0; i < size; i++) {
            unsigned int posA = A(m_mt);
            unsigned int posB = B(m_mt);
            std::swap(vec[posA], vec[posB]);
            std::swap(vec[posA + 1], vec[posB + 1]);
            std::swap(vec[posA + 2], vec[posB + 2]);
            std::swap(vec[posA + 3], vec[posB + 3]);
        }
    }

    template <typename sometype>
    static void permutate_vector_good(std::vector<sometype>* v, bool init) {
        std::vector<sometype>& vec = *v;
        if (init) for (unsigned int i = 0; i < vec.size(); i++) vec[i] = (sometype)i;
        if (vec.size() < 10) { permutate_vector_good_small(v); return; }
        unsigned int size = vec.size();
        std::uniform_int_distribution<unsigned int> A(0, size - 4);
        std::uniform_int_distribution<unsigned int> B(0, size - 4);
        for (unsigned int i = 0; i < size; i++) {
            unsigned int posA = A(m_mt);
            unsigned int posB = B(m_mt);
            std::swap(vec[posA], vec[posB]);
            std::swap(vec[posA + 1], vec[posB + 1]);
            std::swap(vec[posA + 2], vec[posB + 2]);
            std::swap(vec[posA + 3], vec[posB + 3]);
        }
    }

    template <typename sometype>
    static void permutate_vector_good_small(std::vector<sometype>* v) {
        std::vector<sometype>& vec = *v;
        if (vec.size() < 2) return;
        unsigned int size = vec.size();
        std::uniform_int_distribution<unsigned int> A(0, size - 1);
        std::uniform_int_distribution<unsigned int> B(0, size - 1);
        for (unsigned int i = 0; i < size; i++) {
            unsigned int posA = A(m_mt);
            unsigned int posB = B(m_mt);
            std::swap(vec[posA], vec[posB]);
        }
    }

    static bool nextBool() {
        std::uniform_int_distribution<unsigned int> A(0, 1);
        bool v = static_cast<bool>(A(m_mt));
        std::fprintf(stderr, "[TRACE-RNG] kind:nextBool val:%d\n", (int)v);
        return v;
    }

    static unsigned nextInt(unsigned int lb, unsigned int rb) {
        std::uniform_int_distribution<unsigned int> A(lb, rb);
        unsigned v = A(m_mt);
        std::fprintf(stderr,
                     "[TRACE-RNG] kind:nextInt lb:%u rb:%u val:%u\n",
                     lb, rb, v);
        return v;
    }

    static double nextDouble(double lb, double rb) {
        std::uniform_real_distribution<> A(lb, rb);
        double v = A(m_mt);
        std::fprintf(stderr,
                     "[TRACE-RNG] kind:nextDouble lb:%.17g rb:%.17g val:%.17g\n",
                     lb, rb, v);
        return v;
    }

    static uint32_t next() {
        uint32_t v = m_mt();
        std::fprintf(stderr, "[TRACE-RNG] kind:next val:0x%08x\n", v);
        return v;
    }

    // libstdc++ uniform_int_distribution via floor-rejection scaling,
    // routed through traced next(). Mirror of JS uniformIntViaNext.
    static uint32_t uniform_int_via_next(uint32_t lo, uint32_t hi) {
        uint32_t r = hi - lo + 1u;
        if (r == 0) return (next() + lo);
        if (r == 1) return lo;
        uint64_t U = 4294967296ull;  // 2^32
        uint64_t scaling = U / (uint64_t)r;
        uint64_t past = scaling * (uint64_t)r;
        while (true) {
            uint64_t ret = (uint64_t)next();
            if (ret < past) return (uint32_t)(lo + (uint32_t)(ret / scaling));
        }
    }

    // libstdc++ std::shuffle optimized branch (bits/stl_algo.h:3719-3792).
    // Routes all uint32 draws through next() so each draw emits [TRACE-RNG]
    // in the same order the JS port consumes them.
    template <typename T>
    static void shuffle_range_traced(std::vector<T>& vec, size_t lo,
                                     size_t hi) {
        if (hi - lo < 2) return;
        size_t urange = hi - lo;
        size_t i = lo + 1;
        if ((urange & 1u) == 0) {
            uint32_t pos = uniform_int_via_next(0, 1);
            std::swap(vec[i], vec[lo + pos]);
            i++;
        }
        while (i < hi) {
            uint32_t swap_range = (uint32_t)((i - lo) + 1);
            uint32_t hi2 = swap_range * (swap_range + 1) - 1;
            uint32_t x = uniform_int_via_next(0, hi2);
            uint32_t pp_first = x / (swap_range + 1);
            uint32_t pp_second = x % (swap_range + 1);
            std::swap(vec[i], vec[lo + pp_first]);
            i++;
            std::swap(vec[i], vec[lo + pp_second]);
            i++;
        }
    }

    template <typename T>
    static void permutate_vector_local_traced(std::vector<T>* v, bool init) {
        std::vector<T>& vec = *v;
        if (init) for (unsigned int i = 0; i < vec.size(); i++) vec[i] = (T)i;
        size_t localsize = 128;
        for (size_t i = 0; i < vec.size(); i += localsize) {
            size_t end = std::min(vec.size(), i + localsize);
            shuffle_range_traced(vec, i, end);
        }
    }

    static MersenneTwister getRand() { return m_mt; }

    static void setSeed(int seed) {
        m_seed = seed;
        srand(seed);
        m_mt.seed(m_seed);
        std::fprintf(stderr, "[TRACE-RNG] kind:setSeed val:%d\n", seed);
    }

    static int getSeed() { return m_seed; }
};

/*
 * LD_PRELOAD shim for the upstream gen-louvain binary.
 *
 * The original main_louvain.cpp calls srand(time(NULL)+getpid()) unconditionally,
 * giving non-reproducible runs. We never modify upstream source. This shim overrides
 * srand at load time so the binary, when launched with LD_PRELOAD=<this.so>
 * LOUVAIN_SEED=<n>, seeds with the requested value instead. Without LOUVAIN_SEED set,
 * srand passes through unchanged.
 *
 * Build:
 *   gcc -shared -fPIC -O2 -o louvain_seed_preload.so louvain_seed_preload.c -ldl
 *
 * Use:
 *   LD_PRELOAD=/abs/path/louvain_seed_preload.so LOUVAIN_SEED=1 ./louvain graph.bin ...
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <dlfcn.h>

void srand(unsigned int seed) {
    static void (*real_srand)(unsigned int) = NULL;
    if (!real_srand) {
        real_srand = (void (*)(unsigned int))dlsym(RTLD_NEXT, "srand");
    }
    const char *env = getenv("LOUVAIN_SEED");
    if (env && *env) {
        seed = (unsigned int)strtoul(env, NULL, 10);
    }
    real_srand(seed);
}

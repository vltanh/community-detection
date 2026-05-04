#!/bin/bash
# Build the LD_PRELOAD srand-override shim used to reproducibly seed the
# upstream gen-louvain binary.
set -eu
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
gcc -shared -fPIC -O2 -o "${DIR}/louvain_seed_preload.so" "${DIR}/louvain_seed_preload.c" -ldl
echo "Built ${DIR}/louvain_seed_preload.so"

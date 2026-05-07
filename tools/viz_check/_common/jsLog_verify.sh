#!/usr/bin/env bash
# Verify jsLog (fdlibm port) bit-equal vs V8 Math.log over dense sweep.
# Per playbook L2: ≥10k inputs, 0/N mismatches required.
# Expected outcome: jsLog == V8 Math.log byte-for-byte; std::log differs
# by 1 ulp on ~65/10014 inputs (the closure motivation).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SWEEP=/tmp/jsLog_sweep.txt
CPP_OUT=/tmp/jsLog_cpp.bin
STD_OUT=/tmp/std_log_cpp.bin
JS_OUT=/tmp/jsLog_js.bin

# 1. Generate sweep: log-spaced + linear-spaced + boundary cases.
python3 - <<'PY' > $SWEEP
import math
# Log-spaced [1e-9, 1e9]: 10000 inputs
import random
random.seed(42)
inputs = []
for i in range(10000):
    e = random.uniform(-9.0, 9.0)
    inputs.append(10.0 ** e)
# Boundary cases
inputs.extend([1.0, 2.0, 0.5, 1e-300, 1.0 + 1e-15, 1.0 - 1e-15, math.e, math.pi])
inputs.extend([float(i) for i in range(1, 100)])  # small ints
print('\n'.join(repr(x) for x in inputs))
PY
N=$(wc -l < $SWEEP)
echo "sweep size: $N"

# 2. cpp jsLog + std::log
cat > /tmp/jsLog_test.cpp <<'CPP'
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include "_common/js_math_port.h"
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    std::ifstream in(argv[1]);
    std::ofstream js(argv[2], std::ios::binary);
    std::ofstream st(argv[3], std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        double x = std::stod(line);
        double yj = viz_check::jsmath::jsLog(x);
        double ys = std::log(x);
        uint64_t bj, bs;
        std::memcpy(&bj, &yj, 8);
        std::memcpy(&bs, &ys, 8);
        js.write((char*)&bj, 8);
        st.write((char*)&bs, 8);
    }
    return 0;
}
CPP
g++ -std=c++20 -O2 -I"$HERE/.." /tmp/jsLog_test.cpp -o /tmp/jsLog_test
/tmp/jsLog_test $SWEEP $CPP_OUT $STD_OUT

# 3. JS Math.log
cat > /tmp/jsLog_test.mjs <<'JS'
import fs from "fs";
const sweep = fs.readFileSync(process.argv[2], "utf8").trim().split("\n");
const buf = new ArrayBuffer(sweep.length * 8);
const f64 = new Float64Array(buf);
for (let i = 0; i < sweep.length; i++) f64[i] = Math.log(parseFloat(sweep[i]));
fs.writeFileSync(process.argv[3], Buffer.from(buf));
JS
node /tmp/jsLog_test.mjs $SWEEP $JS_OUT

# 4. Diff: jsLog vs JS Math.log (must be 0); std::log vs JS Math.log (will diff)
echo ""
echo "=== jsLog (fdlibm port) vs V8 Math.log ==="
if cmp -s $CPP_OUT $JS_OUT; then
    echo "PASS: bit-equal across $N inputs."
else
    bad=$(cmp -l $CPP_OUT $JS_OUT 2>/dev/null | wc -l)
    echo "FAIL: $bad byte differences (max $((N*8)))"
fi
echo "=== std::log vs V8 Math.log ==="
if cmp -s $STD_OUT $JS_OUT; then
    echo "match"
else
    bad=$(cmp -l $STD_OUT $JS_OUT 2>/dev/null | wc -l)
    echo "DIFF: $bad / $((N*8)) bytes (this is the closure motivation; expected divergence)"
fi

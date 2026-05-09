/* L2 diagnostic: bit-compare cpp jsLog / jsLgamma values vs V8 Math.log /
 * the JS Lanczos lgamma port over a dense input sweep.
 *
 * Skill: byte-equal-tracer / Diagnostic ladder L2.
 * Closes audit row D (FP primitives).
 *
 * jsExp coverage is in flat_traced.cpp's audit grid (200k inputs in
 * [-100, 100], step 0.001, bit-equal V8 Math.exp); folding it into this
 * harness needs the full e_exp.c port that already lives inside the
 * tracer binary.
 *
 * Build helper binary first:
 *   g++ -std=c++20 -O2 -ffp-contract=off l2_fp_primitives.cpp -o /tmp/l2_fp_primitives
 * Run:
 *   node l2_fp_primitives.mjs log
 *   node l2_fp_primitives.mjs lgamma
 */
import { spawnSync } from "child_process";
import path from "path";
import { fileURLToPath } from "url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const fn = process.argv[2];
if (!["log", "lgamma"].includes(fn)) {
  console.error("usage: l2_fp_primitives.mjs <log|lgamma>");
  process.exit(2);
}

const cpp = spawnSync("/tmp/l2_fp_primitives", [fn], { encoding: "utf8", maxBuffer: 256 * 1024 * 1024 });
if (cpp.status !== 0) {
  console.error("/tmp/l2_fp_primitives failed; build it first:\n" +
                "  g++ -std=c++20 -O2 -ffp-contract=off " +
                path.join(__dirname, "l2_fp_primitives.cpp") +
                " -o /tmp/l2_fp_primitives");
  process.exit(2);
}

globalThis.window = globalThis;
const WEB = path.resolve(__dirname, "../../../../vltanh.github.io/comdet/js");
await import(path.join(WEB, "sbm/util.js"));
const lgamma = window.COMDET.SBM.UTIL.lgamma;

function bits(y) {
  const b = new Float64Array(1); b[0] = y;
  return new BigUint64Array(b.buffer)[0];
}

const lines = cpp.stdout.trim().split("\n");
let mismatches = 0;
let total = 0;
for (const line of lines) {
  const parts = line.split(" ");
  const x = Number(parts[0]);
  const cppBits = BigInt(parts[1]);
  const jsY = (fn === "log") ? Math.log(x) : lgamma(x);
  const jsBits = bits(jsY);
  total++;
  if (cppBits !== jsBits) {
    if (mismatches < 5) {
      console.log(`  L2 mismatch fn=${fn} x=${x} cpp=0x${cppBits.toString(16)} js=0x${jsBits.toString(16)}`);
    }
    mismatches++;
  }
}
console.log(`L2 (FP primitive ${fn}): N=${total} mismatches=${mismatches}/${total}`);
process.exit(mismatches === 0 ? 0 : 1);

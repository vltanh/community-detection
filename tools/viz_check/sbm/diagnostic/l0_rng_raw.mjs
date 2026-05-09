/* L0 diagnostic: byte-equal first N raw outputs of std::mt19937(seed)
 * vs JS COMDET.SBM.MT19937(seed).
 *
 * Skill: byte-equal-tracer / Diagnostic ladder L0.
 * Closes audit row A (RNG byte stream).
 *
 * Build helper binary first:
 *   g++ -std=c++17 -O2 l0_rng_raw.cpp -o /tmp/l0_rng_raw
 * Run:
 *   node l0_rng_raw.mjs                   # default seed=7 N=100
 *   node l0_rng_raw.mjs <seed> <N>
 */
import { spawnSync } from "child_process";
import path from "path";
import { fileURLToPath } from "url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const seed = args[0] != null ? Number(args[0]) : 7;
const N = args[1] != null ? Number(args[1]) : 100;

const cpp = spawnSync("/tmp/l0_rng_raw", [String(seed), String(N)], { encoding: "utf8" });
if (cpp.status !== 0) {
  console.error("/tmp/l0_rng_raw failed; build it first:\n  g++ -std=c++17 -O2 " +
                path.join(__dirname, "l0_rng_raw.cpp") + " -o /tmp/l0_rng_raw");
  process.exit(2);
}
const cppStream = cpp.stdout.trim().split("\n").map(s => Number(s) >>> 0);

globalThis.window = globalThis;
globalThis.window.COMDET = {};
const WEB = path.resolve(__dirname, "../../../../vltanh.github.io/comdet/js");
await import(path.join(WEB, "sbm/util.js"));
await import(path.join(WEB, "sbm/rng.js"));
const rng = window.COMDET.SBM.MT19937(seed);
const jsStream = Array.from({ length: N }, () => rng.raw() >>> 0);

let mismatches = 0;
for (let i = 0; i < N; ++i) {
  if (cppStream[i] !== jsStream[i]) {
    if (mismatches < 5) {
      console.log(`  L0 mismatch idx=${i} cpp=${cppStream[i]} js=${jsStream[i]}`);
    }
    mismatches++;
  }
}
console.log(`L0 (RNG raw stream): seed=${seed} N=${N} mismatches=${mismatches}/${N}`);
process.exit(mismatches === 0 ? 0 : 1);

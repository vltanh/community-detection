// L1 JS leg: replay (seed, N) using the SAME test-input seed (0xCAFEBABE)
// to derive (min, max) pairs, then draw via the libstdc++-style
// uniformInt rejection sampler (matching infomap_canon.js's implementation).
// Emits the same CSV shape: min,max,result,raw_consumed_this_draw.

import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const louvainSrc = fs.readFileSync(
  path.resolve(__dirname, "../../../../vltanh.github.io/comdet/js/louvain/louvain.js"),
  "utf-8"
);
globalThis.window = globalThis;
new Function(louvainSrc)();
const MT = globalThis.COMDET.LOUVAIN.MT19937;

const seed = process.argv[2] ? parseInt(process.argv[2], 10) : 1;
const N = process.argv[3] ? parseInt(process.argv[3], 10) : 100000;

const kernelRng = MT(seed);                    // matches cpp std::mt19937(seed)
const inputsRng = MT(0xCAFEBABE);              // matches cpp std::mt19937(0xCAFEBABE)

let counter = 0;
function rawCounted() {
  counter++;
  return kernelRng.raw() >>> 0;
}

// libstdc++-style std::uniform_int_distribution<unsigned int>(a, b) for
// std::mt19937 (urngmin=0, urngmax=2^32-1):
//   urngrange = 2^32 - 1
//   urange    = b - a
//   uerange   = urange + 1
//   if uerange = 0: return raw()  (full uint32)
//   scaling = floor(urngrange / uerange)
//   past    = uerange * scaling
//   loop: r = raw(); reject if r >= past
//   return a + floor(r / scaling)
function uniformInt(a, b) {
  // libstdc++ std::uniform_int_distribution<unsigned int>::operator() for std::mt19937
  // (__urngrange == UINT32_MAX) routes through _S_nd<uint64>(g, range) — Lemire's
  // debiased multiplication (uniform_int_dist.h:244-270). Mirror that exactly.
  //   range = b - a + 1
  //   product = uint64(g()) * uint64(range)
  //   low = uint32(product)
  //   if low < range:
  //     threshold = (-range) % range  // unsigned modular negation
  //     while low < threshold: redraw
  //   ret = uint32(product >> 32)
  //   return a + ret
  const range = ((b - a) >>> 0) + 1;          // b - a + 1, may be 0 if (b-a) == 2^32-1; never in our test
  const r64 = BigInt(range);
  // cpp `(-range) % range` evaluates in UNSIGNED uint32 arithmetic to (2^32 - range) % range = 2^32 % range.
  // BigInt is signed, so `(-r64) % r64` would yield 0 (BigInt % returns sign of dividend). Compute 2^32 % range
  // directly instead.
  const t = (1n << 32n) % r64;
  let product, low;
  product = BigInt(rawCounted()) * r64;
  low = product & 0xFFFFFFFFn;
  if (low < r64) {
    while (low < t) {
      product = BigInt(rawCounted()) * r64;
      low = product & 0xFFFFFFFFn;
    }
  }
  return a + Number(product >> 32n);
}

process.stdout.write("min,max,result,raw_consumed_this_draw\n");
for (let i = 0; i < N; i++) {
  const a = (inputsRng.raw() >>> 0) & 0xFFFF;
  let b = (inputsRng.raw() >>> 0) & 0x1FFFF;
  if (b < a) b = a;
  const before = counter;
  const r = uniformInt(a, b);
  const consumed = counter - before;
  process.stdout.write(`${a},${b},${r},${consumed}\n`);
}

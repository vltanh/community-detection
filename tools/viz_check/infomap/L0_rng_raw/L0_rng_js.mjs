// L0 RNG raw stream JS leg: print first N raw uint32 from LOUVAIN.MT19937(seed).
// Loads vltanh.github.io/comdet/js/louvain/louvain.js via window-shim and
// calls LOUVAIN.MT19937(seed).raw() N times.
// Usage: node L0_rng_js.mjs <seed> <N>

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
globalThis.console = console;
new Function(louvainSrc)();

const seed = process.argv[2] ? parseInt(process.argv[2], 10) : 1;
const N = process.argv[3] ? parseInt(process.argv[3], 10) : 1000;
const rng = globalThis.COMDET.LOUVAIN.MT19937(seed);
for (let i = 0; i < N; i++) {
  process.stdout.write(String(rng.raw() >>> 0) + "\n");
}

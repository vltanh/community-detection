// L2 JS leg: read xbits from cpp output, compute Math.log2(x) in JS, emit logbits.
// Driver compares ulps.
// Usage: node L2_log2_js.mjs < cpp.csv > js.csv

import { stdin, stdout } from "process";

const buf = new ArrayBuffer(8);
const f64 = new Float64Array(buf);
const u64 = new BigUint64Array(buf);

function bitsToDouble(b) { u64[0] = b; return f64[0]; }
function doubleToBits(x) { f64[0] = x; return u64[0]; }

let chunks = [];
stdin.on("data", c => chunks.push(c));
stdin.on("end", () => {
  const text = Buffer.concat(chunks).toString("utf-8");
  const lines = text.split("\n");
  let out = "xbits,logbits\n";
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    if (i === 0 || line === "") continue;
    const parts = line.split(",");
    if (parts.length < 2) continue;
    const xb = BigInt(parts[0]);
    const x = bitsToDouble(xb);
    const y = Math.log2(x);
    const yb = doubleToBits(y);
    out += xb.toString() + "," + yb.toString() + "\n";
  }
  stdout.write(out);
});

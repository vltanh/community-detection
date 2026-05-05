// L2 alt JS leg: read xbits, compute jsLog2(x) = (x===1) ? 0 : Math.log(x) * Math.LOG2E.
// Math.LOG2E in V8 == 1.4426950408889634 (== exact V8 constant).

import { stdin, stdout } from "process";

const buf = new ArrayBuffer(8);
const f64 = new Float64Array(buf);
const u64 = new BigUint64Array(buf);
function bitsToDouble(b) { u64[0] = b; return f64[0]; }
function doubleToBits(x) { f64[0] = x; return u64[0]; }

function jsLog2(x) {
  if (x === 1.0) return 0.0;
  return Math.log(x) * Math.LOG2E;
}

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
    const y = jsLog2(x);
    const yb = doubleToBits(y);
    out += xb.toString() + "," + yb.toString() + "\n";
  }
  stdout.write(out);
});

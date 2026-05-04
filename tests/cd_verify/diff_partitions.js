/* Diff two clustering CSVs by partition equivalence (cluster ids may
 * differ; node-to-cluster mapping must match up to relabel).
 *
 * Computes: identical?, ARI, per-cluster overlap matrix.
 *
 * Usage: node diff_partitions.js <a.csv> <b.csv> [labelA] [labelB]
 */
"use strict";
const fs = require("fs");

function load(p) {
  const text = fs.readFileSync(p, "utf8").trim();
  const map = new Map();
  text.split(/\r?\n/).forEach(function (line, i) {
    if (i === 0 || !line) return;
    const parts = line.split(",");
    map.set(parts[0], parts[1]);
  });
  return map;
}

const argv = process.argv.slice(2);
if (argv.length < 2) {
  console.error("usage: diff_partitions.js <a.csv> <b.csv> [labelA] [labelB]");
  process.exit(1);
}
const A = load(argv[0]);
const B = load(argv[1]);
const labA = argv[2] || "A";
const labB = argv[3] || "B";

// Restrict to common keys.
const keys = Array.from(A.keys()).filter(function (k) { return B.has(k); });
const aOnly = Array.from(A.keys()).filter(function (k) { return !B.has(k); });
const bOnly = Array.from(B.keys()).filter(function (k) { return !A.has(k); });

console.log(labA + ": " + A.size + " nodes; " + labB + ": " + B.size + " nodes");
if (aOnly.length) console.log("  in " + labA + " only:", aOnly.join(","));
if (bOnly.length) console.log("  in " + labB + " only:", bOnly.join(","));

// Build cluster-cluster overlap matrix.
const aClusters = new Map(), bClusters = new Map();
keys.forEach(function (k) {
  const ca = A.get(k), cb = B.get(k);
  if (!aClusters.has(ca)) aClusters.set(ca, new Set());
  if (!bClusters.has(cb)) bClusters.set(cb, new Set());
  aClusters.get(ca).add(k);
  bClusters.get(cb).add(k);
});
const aIds = Array.from(aClusters.keys()).sort();
const bIds = Array.from(bClusters.keys()).sort();
console.log(labA + " clusters: " + aIds.length + "; " + labB + " clusters: " + bIds.length);

// ARI: Hubert + Arabie 1985.
function ari(memA, memB, keys) {
  const aMap = new Map(); const bMap = new Map();
  keys.forEach(function (k) {
    aMap.set(memA.get(k), (aMap.get(memA.get(k)) || 0) + 1);
    bMap.set(memB.get(k), (bMap.get(memB.get(k)) || 0) + 1);
  });
  const cont = new Map();
  keys.forEach(function (k) {
    const key = memA.get(k) + "|" + memB.get(k);
    cont.set(key, (cont.get(key) || 0) + 1);
  });
  function comb2(n) { return n * (n - 1) / 2; }
  let sumNij = 0;
  cont.forEach(function (n) { sumNij += comb2(n); });
  let sumAi = 0;
  aMap.forEach(function (n) { sumAi += comb2(n); });
  let sumBj = 0;
  bMap.forEach(function (n) { sumBj += comb2(n); });
  const N = keys.length;
  const totalPairs = comb2(N);
  if (totalPairs === 0) return 1.0;
  const expected = (sumAi * sumBj) / totalPairs;
  const max = (sumAi + sumBj) / 2;
  if (max === expected) return 1.0;
  return (sumNij - expected) / (max - expected);
}

const ariValue = ari(A, B, keys);
console.log("ARI(" + labA + "," + labB + ") = " + ariValue.toFixed(6));

// Identity check (cluster-id-aware).
let identical = true;
for (let i = 0; i < keys.length; i++) {
  if (A.get(keys[i]) !== B.get(keys[i])) { identical = false; break; }
}
console.log("identical cluster_ids: " + identical);

// Show overlap matrix when small.
if (aIds.length <= 12 && bIds.length <= 12) {
  console.log("");
  console.log("overlap matrix (" + labA + " rows, " + labB + " cols):");
  const header = "        " + bIds.map(function (b) { return b.padStart(4); }).join(" ");
  console.log(header);
  aIds.forEach(function (a) {
    let row = a.padStart(6) + " |";
    bIds.forEach(function (b) {
      let count = 0;
      keys.forEach(function (k) {
        if (A.get(k) === a && B.get(k) === b) count += 1;
      });
      row += " " + (count ? String(count).padStart(3) : "  .");
    });
    console.log(row);
  });
}

process.exit(identical ? 0 : (ariValue >= 0.999 ? 0 : 2));

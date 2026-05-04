/* Run CC / WCC / CM JS kernel on the 32-node fixture, write output in
 * the same node_id,cluster_id format as the binary.
 *
 * Usage:
 *   node run_kernel.js cc <out.csv>
 *   node run_kernel.js wcc <out.csv> [<criterion>]
 *   node run_kernel.js cm  <out.csv> [<criterion>] [<algo>] [<resolution>]
 */
"use strict";
const fs = require("fs");
const path = require("path");

global.window = global;
const WEB = "../../../../web/vltanh.github.io/comdet/js";
require(path.join(__dirname, WEB, "fixture.js"));
require(path.join(__dirname, WEB, "mincut.js"));
require(path.join(__dirname, WEB, "louvain/louvain.js"));
require(path.join(__dirname, WEB, "leiden/leiden.js"));
require(path.join(__dirname, WEB, "cc/cc.js"));
require(path.join(__dirname, WEB, "wcc/wcc.js"));
require(path.join(__dirname, WEB, "cm/cm.js"));

const F = window.COMDET.FIXTURE;

function writeAssign(outPath, finalAssign) {
  // CC + WCC: emit consecutive 0..N-1 ids matching the binary's
  // WriteClusterQueue<vector<int>> overload (no lineage).
  const buckets = new Map();
  finalAssign.forEach(function (cid, i) {
    if (cid < 0) return;
    if (!buckets.has(cid)) buckets.set(cid, []);
    buckets.get(cid).push(F.nodes[i]);
  });
  const lines = ["node_id,cluster_id"];
  const ids = Array.from(buckets.keys()).sort(function (a, b) { return a - b; });
  ids.forEach(function (cid) {
    buckets.get(cid).forEach(function (n) {
      lines.push(n + "," + cid);
    });
  });
  fs.writeFileSync(outPath, lines.join("\n") + "\n");
}

function writeAssignWithLineage(outPath, survivors) {
  // CM: emit the lineage id from each survivor (binary's WriteCluster
  // Queue<pair<vector<int>,int>> preserves CM's parent/child cluster
  // ids). Survivors land in queue order (= the order CM declared them
  // well-connected).
  const lines = ["node_id,cluster_id"];
  survivors.forEach(function (s) {
    s.nodes.forEach(function (n) { lines.push(n + "," + s.id); });
  });
  fs.writeFileSync(outPath, lines.join("\n") + "\n");
}

const argv = process.argv.slice(2);
const algo = argv[0];
const out = argv[1] || "out.csv";

if (algo === "cc") {
  const r = COMDET.CC.runCC(F.gt);
  writeAssign(out, r.finalAssign);
  console.log("CC wrote", out, "numClusters=" + r.numClusters);
} else if (algo === "wcc") {
  const criterion = argv[2] || "1log_10(n)";
  const r = COMDET.WCC.runWCC(F.gt, { criterion: criterion });
  writeAssign(out, r.finalAssign);
  console.log("WCC wrote", out, "criterion=" + criterion, "numClusters=" + r.numClusters);
} else if (algo === "cm") {
  const criterion = argv[2] || "1log_10(n)";
  const baseAlgo = argv[3] || "leiden-cpm";
  const resolution = argv[4] != null ? parseFloat(argv[4]) : 0.0001;
  const r = COMDET.CM.runCM(F.gt, {
    criterion: criterion, algorithm: baseAlgo, resolution: resolution, seed: 0,
  });
  writeAssignWithLineage(out, r.survivors);
  console.log("CM wrote", out, "criterion=" + criterion,
              "algo=" + baseAlgo, "res=" + resolution,
              "numClusters=" + r.numClusters);
} else {
  console.error("usage: run_kernel.js {cc|wcc|cm} <out.csv> [args...]");
  process.exit(1);
}

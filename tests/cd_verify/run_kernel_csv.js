/* Load an edgelist + clustering pair from CSV and run a JS kernel over
 * them. Mirrors the binary's input map: nodes get new ids in the order
 * encountered while parsing edge.csv, exactly like
 * `ConstrainedClustering::GetOriginalToNewIdMap` (constrained.cpp:32-64).
 *
 * Usage:
 *   node run_kernel_csv.js cc  <edge.csv> <com.csv> <out.csv>
 *   node run_kernel_csv.js wcc <edge.csv> <com.csv> <out.csv> [<criterion>]
 *   node run_kernel_csv.js cm  <edge.csv> <com.csv> <out.csv>
 *                              [<criterion>] [<algo>] [<resolution>]
 *
 * Output format matches the binary's WriteClusterQueue: header row +
 * cluster lines in queue iteration order. CC/WCC use consecutive ids,
 * CM preserves CM lineage ids per-pair queue.
 */
"use strict";
const fs = require("fs");
const path = require("path");

global.window = global;
window.COMDET = window.COMDET || {};
window.COMDET.FIXTURE = { nodes: [], edges: [], gt: [] }; // placeholder

const WEB = "../../../../web/vltanh.github.io/comdet/js";
require(path.join(__dirname, WEB, "mincut.js"));
require(path.join(__dirname, WEB, "louvain/louvain.js"));
require(path.join(__dirname, WEB, "leiden/leiden.js"));
require(path.join(__dirname, WEB, "comdet/page_helpers.js"));
require(path.join(__dirname, WEB, "cc/cc.js"));
require(path.join(__dirname, WEB, "wcc/wcc.js"));
require(path.join(__dirname, WEB, "cm/cm.js"));

function detectDelimiter(line) {
  if (line.indexOf(",") >= 0) return ",";
  if (line.indexOf("\t") >= 0) return "\t";
  if (line.indexOf(" ") >= 0) return " ";
  throw new Error("could not detect delimiter on first line: " + line);
}

function loadGraphAndClustering(edgePath, comPath) {
  const edgeText = fs.readFileSync(edgePath, "utf8");
  const lines = edgeText.split(/\r?\n/).filter(function (l) { return l.length > 0; });
  if (lines.length === 0) throw new Error("empty edge file");
  const delim = detectDelimiter(lines[0]);
  // ConstrainedClustering::GetOriginalToNewIdMap: insert in row order;
  // first row is header (skipped). new_id assigned per encountered
  // string in column order: source first, target second, then advance
  // to next row.
  const original_to_new = new Map();
  const edges = []; // pairs of new_ids
  for (let i = 1; i < lines.length; i++) {
    const cols = lines[i].split(delim);
    const s = cols[0];
    const t = cols[1];
    if (!original_to_new.has(s)) original_to_new.set(s, original_to_new.size);
    if (!original_to_new.has(t)) original_to_new.set(t, original_to_new.size);
    edges.push([original_to_new.get(s), original_to_new.get(t)]);
  }
  const new_to_original = new Array(original_to_new.size);
  original_to_new.forEach(function (newId, origStr) { new_to_original[newId] = origStr; });

  // ReadCommunities (constrained.h:71-97). For each row in com.csv,
  // skip if node_id absent from original_to_new map; else partition_map
  // [new_node_id] = cluster_id.
  const comText = fs.readFileSync(comPath, "utf8");
  const comLines = comText.split(/\r?\n/).filter(function (l) { return l.length > 0; });
  const comDelim = detectDelimiter(comLines[0]);
  const partition = new Int32Array(original_to_new.size);
  // Sentinel: any node not in com gets -2 ("absent"); the binary treats
  // absent nodes as not part of any cluster, so they contribute no
  // intra-cluster edges (they're effectively dropped).
  for (let i = 0; i < partition.length; i++) partition[i] = -2;
  for (let i = 1; i < comLines.length; i++) {
    const cols = comLines[i].split(comDelim);
    const nodeStr = cols[0];
    const cid = parseInt(cols[1], 10);
    const newId = original_to_new.get(nodeStr);
    if (newId == null) continue;
    partition[newId] = cid;
  }

  return {
    nodes: Array.from({ length: original_to_new.size }, function (_, i) { return i; }),
    edges: edges,
    membership: partition,
    new_to_original: new_to_original,
  };
}

function writeAssignByNew(outPath, finalAssign, new_to_original) {
  const buckets = new Map();
  finalAssign.forEach(function (cid, newId) {
    if (cid < 0) return;
    if (!buckets.has(cid)) buckets.set(cid, []);
    buckets.get(cid).push(newId);
  });
  const lines = ["node_id,cluster_id"];
  const ids = Array.from(buckets.keys()).sort(function (a, b) { return a - b; });
  ids.forEach(function (cid) {
    buckets.get(cid).forEach(function (newId) {
      lines.push(new_to_original[newId] + "," + cid);
    });
  });
  fs.writeFileSync(outPath, lines.join("\n") + "\n");
}

function writeAssignByLineage(outPath, survivors, new_to_original) {
  const lines = ["node_id,cluster_id"];
  survivors.forEach(function (s) {
    s.nodes.forEach(function (newId) {
      lines.push(new_to_original[newId] + "," + s.id);
    });
  });
  fs.writeFileSync(outPath, lines.join("\n") + "\n");
}

const argv = process.argv.slice(2);
const algo = argv[0];
const edgePath = argv[1];
const comPath = argv[2];
const outPath = argv[3];

if (!algo || !edgePath || !comPath || !outPath) {
  console.error("usage: run_kernel_csv.js {cc|wcc|cm} <edge.csv> <com.csv> <out.csv> [args...]");
  process.exit(1);
}

const G = loadGraphAndClustering(edgePath, comPath);
const fixture = { nodes: G.nodes, edges: G.edges, gt: Array.from(G.membership) };

if (algo === "cc") {
  const r = COMDET.CC.runCC(G.membership, { fixture: fixture });
  writeAssignByNew(outPath, r.finalAssign, G.new_to_original);
  console.log("CC: nodes=" + G.nodes.length, "edges=" + G.edges.length, "out_clusters=" + r.numClusters);
} else if (algo === "wcc") {
  const criterion = argv[4] || "1log_10(n)";
  const r = COMDET.WCC.runWCC(G.membership, { fixture: fixture, criterion: criterion });
  writeAssignByNew(outPath, r.finalAssign, G.new_to_original);
  console.log("WCC: nodes=" + G.nodes.length, "edges=" + G.edges.length,
              "criterion=" + criterion, "out_clusters=" + r.numClusters);
} else if (algo === "cm") {
  const criterion = argv[4] || "1log_10(n)";
  const baseAlgo = argv[5] || "leiden-cpm";
  const resolution = argv[6] != null ? parseFloat(argv[6]) : 0.0001;
  const r = COMDET.CM.runCM(G.membership, {
    fixture: fixture, criterion: criterion,
    algorithm: baseAlgo, resolution: resolution, seed: 0,
  });
  writeAssignByLineage(outPath, r.survivors, G.new_to_original);
  console.log("CM: nodes=" + G.nodes.length, "edges=" + G.edges.length,
              "criterion=" + criterion, "algo=" + baseAlgo, "res=" + resolution,
              "out_clusters=" + r.numClusters);
} else {
  console.error("unknown algorithm: " + algo);
  process.exit(1);
}

/* Emit the 32-node comdet fixture as constrained_clustering input files.
 *
 * Outputs (under cwd):
 *   edge.csv        — header `node_id1,node_id2` + 52 edge rows
 *   com_gt.csv      — header `node_id,cluster_id` + 32 rows (planted GT)
 *
 * The fixture lives at vltanh.github.io/comdet/js/fixture.js. We require
 * it, populate window.COMDET, then walk the loaded data structures.
 *
 * Usage: node emit_fixture.js [outdir]
 */
"use strict";
const fs = require("fs");
const path = require("path");

global.window = global;
require("../../../../web/vltanh.github.io/comdet/js/fixture.js");

const F = window.COMDET.FIXTURE;
const outDir = process.argv[2] || ".";
fs.mkdirSync(outDir, { recursive: true });

const edgePath = path.join(outDir, "edge.csv");
const comPath  = path.join(outDir, "com_gt.csv");

const edgeLines = ["node_id1,node_id2"];
F.edges.forEach(function (e) { edgeLines.push(e[0] + "," + e[1]); });
fs.writeFileSync(edgePath, edgeLines.join("\n") + "\n");

const comLines = ["node_id,cluster_id"];
F.nodes.forEach(function (id, i) { comLines.push(id + "," + F.gt[i]); });
fs.writeFileSync(comPath, comLines.join("\n") + "\n");

console.log("emitted:");
console.log("  " + edgePath + " (" + F.edges.length + " edges)");
console.log("  " + comPath + "  (" + F.nodes.length + " node-cluster rows)");

/* IKC hook-installed-vs-uninstalled equivalence test.
 *
 * Verifies the gold-standard playbook contract: the hook gate must be a
 * zero-cost no-op when no harness is installed, and behaviour must be
 * byte-identical with vs without the hook installed.
 *
 * Loads ikc.js twice in fresh vm contexts:
 *   1. without globalThis.__IKC_HOOK -> capture return value.
 *   2. with __IKC_HOOK as a no-op recorder -> capture return value.
 *
 * Asserts both returns are deep-equal (membership Map, accepted array,
 * dropped array, iterations array structure). Prints PASS / FAIL.
 */
import { readFile } from "node:fs/promises";
import vm from "node:vm";

const KERNEL_PATH = "/home/vltanh/Documents/web/vltanh.github.io/comdet/js/ikc/ikc.js";
const FIXTURE_EDGES = "/home/vltanh/Documents/netsci-research/community-detection/tests/cd_verify/edge.csv";

async function loadFixture() {
  const csv = await readFile(FIXTURE_EDGES, "utf8");
  const lines = csv.trim().split(/\r?\n/).slice(1);
  const seen = new Set();
  const order = [];
  const edges = [];
  for (const line of lines) {
    if (!line) continue;
    const cols = line.split(/[,\t ]/);
    const a = parseInt(cols[0], 10);
    const b = parseInt(cols[1], 10);
    if (!seen.has(a)) { seen.add(a); order.push(a); }
    if (!seen.has(b)) { seen.add(b); order.push(b); }
    edges.push([a, b]);
  }
  return { nodes: order, edges };
}

async function runOnce(installHook) {
  const ctx = vm.createContext({ window: {}, globalThis: undefined });
  ctx.globalThis = ctx;
  ctx.window.COMDET = {};
  if (installHook) {
    // No-op recorder: counts events but doesn't mutate kernel inputs/outputs.
    const captured = [];
    ctx.globalThis.__IKC_HOOK = function (eventName, payload) {
      captured.push({ event: eventName });
    };
    ctx.__captured = captured;
  }
  const src = await readFile(KERNEL_PATH, "utf8");
  vm.runInContext(src, ctx);
  const IKC = ctx.window.COMDET.IKC;
  const { nodes, edges } = await loadFixture();
  const result = IKC.runIKC(nodes, edges, { kFloor: 2, canonicalGate: true, fixture: "fixture32" });
  return { result, captured: ctx.__captured };
}

function normalizeMembership(m) {
  // Map -> sorted [[k,v]...] for deep-equal comparison.
  const arr = [];
  m.forEach(function (v, k) { arr.push([k, v]); });
  arr.sort(function (a, b) { return a[0] - b[0]; });
  return arr;
}

function normalizeIterations(iters) {
  // Strip non-canonical structure that the tests don't care about
  // (already deterministic without hook installed); compare core fields.
  return iters.map(function (it) {
    return {
      iteration: it.iteration,
      maxK: it.maxK,
      bailed: it.bailed,
      residualNodes: it.residualNodes,
      kcoreNodes: it.kcoreNodes,
      components: it.components.map(function (c) {
        return {
          nodes: c.nodes,
          kValid: c.kValid,
          modularity: c.modularity,
          modularityPass: c.modularityPass,
          accepted: c.accepted,
          fateReason: c.fateReason,
        };
      }),
      acceptedSizes: it.accepted.map(function (a) { return a.nodes.length; }),
    };
  });
}

function deepEq(a, b, path) {
  path = path || "";
  if (a === b) return null;
  if (a == null || b == null) return path + ": " + JSON.stringify(a) + " vs " + JSON.stringify(b);
  if (typeof a !== typeof b) return path + ": type " + typeof a + " vs " + typeof b;
  if (Array.isArray(a)) {
    if (!Array.isArray(b)) return path + ": array vs non-array";
    if (a.length !== b.length) return path + ": length " + a.length + " vs " + b.length;
    for (let i = 0; i < a.length; i++) {
      const e = deepEq(a[i], b[i], path + "[" + i + "]");
      if (e) return e;
    }
    return null;
  }
  if (typeof a === "object") {
    const ka = Object.keys(a).sort();
    const kb = Object.keys(b).sort();
    if (ka.length !== kb.length || ka.some(function (k, i) { return k !== kb[i]; })) {
      return path + ": keys " + JSON.stringify(ka) + " vs " + JSON.stringify(kb);
    }
    for (const k of ka) {
      const e = deepEq(a[k], b[k], path + "." + k);
      if (e) return e;
    }
    return null;
  }
  return path + ": " + JSON.stringify(a) + " vs " + JSON.stringify(b);
}

const noHook = await runOnce(false);
const withHook = await runOnce(true);

let fail = false;

// 1. Same accepted clusters (member content + iteration + k + modularity).
const accA = noHook.result.accepted.map(function (cl) {
  return { iteration: cl.iteration, k: cl.k, members: cl.members.slice(), modularity: cl.modularity };
});
const accB = withHook.result.accepted.map(function (cl) {
  return { iteration: cl.iteration, k: cl.k, members: cl.members.slice(), modularity: cl.modularity };
});
let err = deepEq(accA, accB, "accepted");
if (err) { console.error("FAIL accepted:", err); fail = true; }

// 2. Same dropped list.
err = deepEq(noHook.result.dropped, withHook.result.dropped, "dropped");
if (err) { console.error("FAIL dropped:", err); fail = true; }

// 3. Same membership map.
err = deepEq(
  normalizeMembership(noHook.result.membership),
  normalizeMembership(withHook.result.membership),
  "membership",
);
if (err) { console.error("FAIL membership:", err); fail = true; }

// 4. Same iterations structure.
err = deepEq(
  normalizeIterations(noHook.result.iterations),
  normalizeIterations(withHook.result.iterations),
  "iterations",
);
if (err) { console.error("FAIL iterations:", err); fail = true; }

// Sanity: hook actually fired.
if (!withHook.captured || withHook.captured.length === 0) {
  console.error("FAIL: hook installed but no events captured");
  fail = true;
}

if (fail) {
  console.error("FAIL hook equivalence");
  process.exit(1);
} else {
  console.log("PASS hook equivalence (events captured: " + withHook.captured.length + ")");
}

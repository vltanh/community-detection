// VieCut JS replay leg.
//
// Reads tracer JSON (cactus tree + canonical bipartition), runs the JS
// port of `balanced_cut_dfs` + `most_balanced_minimum_cut::findCutFromCactus`
// for each candidate `start_vertex` in 0..|cactus|-1, and asserts that
// the resulting bipartition byte-matches the canonical's.
//
// The cactus is structurally unique up to start-vertex choice; the JS
// replay sweeps start_vertex because canonical's choice depends on
// VieCut's std::mt19937 state at balanced_cut_dfs invocation, which we
// cannot reconstruct without re-running the full cactus_mincut path.
// A single matching start_vertex is sufficient: the DFS implementation
// is what the test exercises; tie-break invariance is canonical's
// own behaviour.

import { readFileSync } from "node:fs";

// [UPSTREAM common/definitions.h:50-55] DFSVertexStatus
const UNDISCOVERED = 0;
const ACTIVE = 1;
const CYCLE = 2;
const FINISHED = 3;

// ---- minimal mutable_graph subset matching balanced_cut_dfs's reads ----
//
// Each cactus node carries a list of contained original ids and a
// node-local edge list of {target, weight, rev}. balanced_cut_dfs
// reads via: edges_of(n), getEdgeTarget(n, e), getEdgeWeight(n, e),
// get_first_invalid_edge(n), getReverseEdge(n, e), containedVertices(n),
// getOriginalNodes(), n().
//
// most_balanced::findCutFromCactus additionally reads/writes
// node_in_cut on the original graph and reads cactus's
// containedVertices.
class CactusGraph {
  constructor(json, n_original) {
    this.n_orig = n_original;
    this.contained = json.nodes.map((nd) => nd.contained.slice());
    this.adj = json.adj.map((row) => row.map((e) => ({ ...e })));
    this._n = json.n;
  }
  n() { return this._n; }
  number_of_nodes() { return this._n; }
  containedVertices(node) { return this.contained[node]; }
  edges_of(node) {
    const len = this.adj[node].length;
    const out = new Array(len);
    for (let i = 0; i < len; i++) out[i] = i;
    return out;
  }
  getEdgeTarget(node, edge) { return this.adj[node][edge].target; }
  getEdgeWeight(node, edge) { return this.adj[node][edge].weight; }
  get_first_invalid_edge(node) { return this.adj[node].length; }
  getReverseEdge(node, edge) { return this.adj[node][edge].rev; }
  getOriginalNodes() { return this.n_orig; }
}

// ---- balanced_cut_dfs.h port ------------------------------------------
//
// [UPSTREAM lib/algorithms/global_mincut/cactus/balanced_cut_dfs.h]
// Verbatim translation. Recursion is iterative-via-stack to avoid
// blowing the JS call stack on deep cacti; semantics preserved.
function runBalancedCutDFS(G, mincut, start_vertex) {
  const n = G.n();
  const status = new Int8Array(n).fill(UNDISCOVERED);
  for (let i = 0; i < n; i++) status[i] = UNDISCOVERED;
  const subtree_weight = new Array(n).fill(-1);
  const parent = new Array(n).fill(-1);
  const outgoing_cycles = Array.from({ length: n }, () => []);

  let best_in_cycle = false;
  let best_n = 0, best_e = 0, best_n2 = 0, best_e2 = 0, best_weight = 0;

  // Total weight = original-graph node count (we don't simulate
  // optimize_conductance here; default code path).
  function totalWeight() { return G.getOriginalNodes(); }
  function findVertexWeight(node) { return G.containedVertices(node).length; }
  function lighterBlock(w) { return Math.min(w, totalWeight() - w); }

  // [UPSTREAM lines 145-201] investigateCycle
  function investigateCycle(t, start) {
    const cycle_vertices = [t];
    const cycle_weight = [subtree_weight[t]];
    while (cycle_vertices[cycle_vertices.length - 1] !== start) {
      const par = parent[cycle_vertices[cycle_vertices.length - 1]];
      cycle_weight.push(
        subtree_weight[par] -
        subtree_weight[cycle_vertices[cycle_vertices.length - 1]]
      );
      cycle_vertices.push(par);
    }
    cycle_weight[cycle_weight.length - 1] =
      totalWeight() -
      subtree_weight[cycle_vertices[cycle_vertices.length - 2]];

    const length = cycle_weight.length;
    let back = length;
    let front = 1;
    let in_weight = cycle_weight[0];

    while (back <= 2 * length) {
      if (lighterBlock(in_weight) >= best_weight) {
        best_weight = lighterBlock(in_weight);
        best_in_cycle = true;

        // front edge
        const fn = cycle_vertices[((front - 1) % length + length) % length];
        const ftgt = cycle_vertices[((front) % length + length) % length];
        for (const e of G.edges_of(fn)) {
          if (G.getEdgeTarget(fn, e) === ftgt) {
            best_n = fn;
            best_e = e;
            break;
          }
        }
        // back edge
        const bn = cycle_vertices[((back - 1) % length + length) % length];
        const btgt = cycle_vertices[((back) % length + length) % length];
        for (const e of G.edges_of(bn)) {
          if (G.getEdgeTarget(bn, e) === btgt) {
            best_n2 = bn;
            best_e2 = e;
            break;
          }
        }
      }
      if (in_weight * 2 >= totalWeight()) {
        in_weight -= cycle_weight[((back) % length + length) % length];
        back++;
      } else {
        in_weight += cycle_weight[((front) % length + length) % length];
        front++;
      }
    }
  }

  function checkForCycles(node) {
    for (const t of outgoing_cycles[node]) investigateCycle(t, node);
  }

  // [UPSTREAM lines 71-119] processVertex (recursive C++ -> explicit stack)
  // We emulate recursion frame-by-frame: the C++ function does work both
  // before and after each recursive call (the post-recursion children-
  // weight accumulation and best update). We model frames with state
  // {node, edgeIdx, children_weight, current_e_target_post}.
  const stack = [];
  function pushFrame(node) {
    status[node] = ACTIVE;
    if (G.get_first_invalid_edge(node) === 1 && node !== start_vertex) {
      // leaf: subtree_weight = findVertexWeight(node); FINISHED.
      subtree_weight[node] = findVertexWeight(node);
      status[node] = FINISHED;
      return;
    }
    stack.push({ node, edgeIdx: 0, children_weight: 0, awaiting: -1 });
  }

  pushFrame(start_vertex);

  while (stack.length > 0) {
    const f = stack[stack.length - 1];
    if (f.awaiting >= 0) {
      // We just returned from a recursive call on edge f.awaiting.
      const e = f.awaiting;
      const t = G.getEdgeTarget(f.node, e);
      // post-recursion best update
      if (lighterBlock(subtree_weight[t]) > best_weight
          && G.getEdgeWeight(f.node, e) === mincut) {
        best_in_cycle = false;
        best_n = f.node;
        best_e = e;
        best_weight = lighterBlock(subtree_weight[t]);
      }
      f.children_weight += subtree_weight[t];
      f.awaiting = -1;
      f.edgeIdx++;
      continue;
    }

    if (f.edgeIdx >= G.get_first_invalid_edge(f.node)) {
      // All edges processed. Finalize this frame.
      if (status[f.node] === CYCLE) checkForCycles(f.node);
      subtree_weight[f.node] = f.children_weight + findVertexWeight(f.node);
      status[f.node] = FINISHED;
      stack.pop();
      continue;
    }

    const e = f.edgeIdx;
    const t = G.getEdgeTarget(f.node, e);
    switch (status[t]) {
      case UNDISCOVERED:
        parent[t] = f.node;
        // recurse into t
        if (G.get_first_invalid_edge(t) === 1 && t !== start_vertex) {
          // leaf shortcut (matches pushFrame's leaf branch)
          status[t] = ACTIVE;
          subtree_weight[t] = findVertexWeight(t);
          status[t] = FINISHED;
          // post-recursion best update
          if (lighterBlock(subtree_weight[t]) > best_weight
              && G.getEdgeWeight(f.node, e) === mincut) {
            best_in_cycle = false;
            best_n = f.node;
            best_e = e;
            best_weight = lighterBlock(subtree_weight[t]);
          }
          f.children_weight += subtree_weight[t];
          f.edgeIdx++;
        } else {
          status[t] = ACTIVE;
          stack.push({ node: t, edgeIdx: 0, children_weight: 0, awaiting: -1 });
          f.awaiting = e;  // park; resume after t finishes
        }
        break;
      case ACTIVE:
      case CYCLE:
        if (parent[f.node] !== t) {
          status[t] = CYCLE;
          outgoing_cycles[t].push(f.node);
        }
        f.edgeIdx++;
        break;
      default:
        f.edgeIdx++;
        break;
    }
  }

  if (best_in_cycle) {
    const r2 = G.getEdgeTarget(best_n2, best_e2);
    const e2r = G.getReverseEdge(best_n2, best_e2);
    return { best_n, best_e, best_n2: r2, best_e2: e2r, best_in_cycle: true,
             best_weight };
  }
  return { best_n, best_e, best_n2: best_n, best_e2: best_e,
           best_in_cycle: false, best_weight };
}

// ---- most_balanced_minimum_cut::findCutFromCactus port ----------------
//
// [UPSTREAM lib/algorithms/global_mincut/cactus/most_balanced_minimum_cut.h:25-113]
// BFS from {n1, n2} over cactus blocking on {rev_n1, rev_n2}; flag
// originals contained in visited cactus-nodes as inCut; final
// bipartition is the inCut array per original-id.
function findBipartitionFromCactus(cactus, n_orig, dfsOut) {
  const { best_n, best_e, best_n2, best_e2 } = dfsOut;
  const n1 = best_n;
  const n2 = best_n2;
  const rev_n1 = cactus.getEdgeTarget(n1, best_e);
  const rev_n2 = cactus.getEdgeTarget(n2, best_e2);

  const inCut = new Int8Array(n_orig);
  const checked = new Uint8Array(cactus.n());
  const q = [n1];
  checked[n1] = 1;
  if (n1 !== n2) {
    q.push(n2);
    checked[n2] = 1;
  }
  checked[rev_n1] = 1;
  checked[rev_n2] = 1;

  while (q.length > 0) {
    const top = q.shift();
    for (const v of cactus.containedVertices(top)) {
      inCut[v] = 1;
    }
    for (const e of cactus.edges_of(top)) {
      const t = cactus.getEdgeTarget(top, e);
      if (!checked[t]) {
        q.push(t);
        checked[t] = 1;
      }
    }
  }
  return Array.from(inCut);
}

function tryCluster(cl) {
  const { tag, n_local, mincut, cactus, bipartition } = cl;
  const G = new CactusGraph(cactus, n_local);
  const target = bipartition;
  const flipped = target.map((x) => 1 - x);
  let lastJs = null;

  for (let sv = 0; sv < G.n(); sv++) {
    const dfsOut = runBalancedCutDFS(G, mincut, sv);
    const js = findBipartitionFromCactus(G, n_local, dfsOut);
    lastJs = js;
    let ex = true, fl = true;
    for (let i = 0; i < js.length; i++) {
      if (js[i] !== target[i]) ex = false;
      if (js[i] !== flipped[i]) fl = false;
      if (!ex && !fl) break;
    }
    if (ex || fl) {
      return { ok: true, sv, mode: ex ? "exact" : "flipped" };
    }
  }
  return { ok: false, target, lastJs };
}

function main() {
  const jsonPath = process.argv[2];
  if (!jsonPath) {
    console.error("usage: kernel_check.mjs <tracer.json>");
    process.exit(2);
  }
  const payload = JSON.parse(readFileSync(jsonPath, "utf-8"));
  let pass = 0, fail = 0;
  const failures = [];
  for (const cl of payload.clusters) {
    const r = tryCluster(cl);
    if (r.ok) {
      pass++;
    } else {
      fail++;
      failures.push({ tag: cl.tag, n: cl.cactus.n, mincut: cl.mincut,
                      target: r.target, lastJs: r.lastJs });
    }
  }
  console.log(`replay summary: ${pass}/${pass + fail} clusters PASS`);
  if (fail > 0) {
    for (const f of failures) {
      console.error(`  FAIL ${f.tag} cactus_n=${f.n} mincut=${f.mincut}`);
      console.error(`    target : ${f.target.join("")}`);
      console.error(`    last_js: ${(f.lastJs || []).join("")}`);
    }
    process.exit(1);
  }
}

main();

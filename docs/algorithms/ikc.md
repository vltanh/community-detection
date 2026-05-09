# IKC

[← back to index](../algorithms.md)

Companion to [`comdet/ikc.html`](https://vltanh.me/comdet/ikc.html).

Iterative k-core Clustering. Wedell et al. 2022 (QSS 3(1):289-314,
[doi:10.1162/qss_a_00184](https://doi.org/10.1162/qss_a_00184)).

The page at `vltanh.github.io/comdet/ikc.html` walks the algorithm on the
32-node fixture; this file is the technical companion: where the code
lives, what each stage does, how the shipped binary differs from the
paper, and how the JS port is verified against canonical.

## Entrypoint

`iterative_k_core_decomposition_MCS_ES(graph, k, inverted_orig_node_ids)`
at [`src/ikc/run_ikc.py:81`](../../src/ikc/run_ikc.py#L81). Driven by
the wrapper `main(args)` at [`src/ikc/run_ikc.py:13`](../../src/ikc/run_ikc.py#L13)
which loads the edgelist, normalises node ids via `format_graph` (line
301), runs the core loop, and writes `com.csv` with consecutive
0-indexed cluster ids (singletons filtered).

## CLI flags

| Flag | What it sets |
| --- | --- |
| `-e / --edgelist` | Path to the input CSV (header `source,target`) |
| `-o / --output-directory` | Output dir; produces `com.csv` |
| `-k / --kvalue` | Connectivity floor; defaults to `0` (= accept everything) |
| `-q / --quiet` | Suppresses per-iteration log lines |

The CM-paper benchmark sweeps `k = 10` (Park 2024 §4); the page shows
`k ∈ [2, 5]` because the 32-node fixture has no 5-core (max core
number = 4 from A's K_5).

## Outer loop

[`run_ikc.py:103-204`](../../src/ikc/run_ikc.py#L103). Each pass:

1. **Compute the current residual's core decomposition.** Calls
   `kc(graph)` at [`run_ikc.py:222`](../../src/ikc/run_ikc.py#L222),
   which delegates to `nk.centrality.CoreDecomposition`. Returns the
   subgraph induced by every node with core number `≥ max_k`, the
   current `max_k`, and the raw `kc` object.
2. **Bail check.** If `max_k < k` (line 114), every remaining node is
   emitted as a singleton (with a degree-aware modularity score that
   has no downstream effect since singletons are filtered in
   `print_clusters`).
3. **Connected-components split.** `nk.components.WeaklyConnectedComponents`
   on the kcore subgraph (line 125-127). Each component is a
   candidate cluster.
4. **k-validity gate.** `k_valid(component, subgraph, k)` at
   [`run_ikc.py:264`](../../src/ikc/run_ikc.py#L264): every node in
   the component must have `degreeIn + degreeOut ≥ k` *inside the kcore
   subgraph*. Stricter than membership in the global k-core, since
   peeling could leave a node with low residual degree even after it
   crossed the global core threshold.
5. **Modularity gate.** `modular(component, orig_graph,
   inverted_orig_node_ids)` at
   [`run_ikc.py:278`](../../src/ikc/run_ikc.py#L278). The first line of
   that function is `return POSITIVE_VALUE = 1`; the paper's formula
   `mod(s) = l_s/L − (d_s/2L)²` is dead code below the early return.
   Ship-hard divergence from the paper: in practice every k-valid
   component passes this gate.
6. **Emit + remove.** Surviving components are appended to
   `final_clusters` with their `max_k` value (the iteration's k, used
   downstream as a per-cluster k-bin label). Their nodes are removed
   from the residual graph; the loop iterates with one fewer max core.
7. **Compaction.** Lines 195-204: NetworKit's continuous-id requirement
   forces a re-pack of node ids after each removal (`getCompactedGraph`
   + the inverse-id-dict roll-forward).

## kc() core extractor

[`src/ikc/run_ikc.py:222-261`](../../src/ikc/run_ikc.py#L222). Wraps
`nk.centrality.CoreDecomposition(graph, storeNodeOrder=True)`. The
output is a `nk.structures.Partition` keyed by node id, value = core
number. The function extracts every node with core number in
`[k, max_k]` (line 254-256) and returns the induced subgraph. With
`k = max_k` (default when called without an explicit `k`), this is
exactly the `max_k`-core.

The JS port mirrors the Batagelj-Zaversnik bucket-queue at
[`comdet/js/ikc/ikc.js:54-96`](https://github.com/vlttnh/community-detection/blob/main/web/comdet/js/ikc/ikc.js):
pop-min, decrement neighbours, repeat. Same core numbers as
NetworKit's CoreDecomposition modulo iteration-order canonicalisation
(see "Container iteration order" below).

## Invariants (every iteration boundary)

1. `residual_nodes ⊆ original_nodes`; once a node is removed, it
   never re-enters the residual.
2. `accepted_clusters` are pairwise disjoint.
3. `accepted_clusters ∪ dropped_nodes ⊆ original_nodes`; the union
   grows monotonically across iterations.
4. Every accepted cluster is k-valid: every node has `≥ k`
   neighbours within the cluster.
5. Every accepted cluster has size `≥ 2`. Singletons cannot be
   k-valid for `k ≥ 1`; for `k = 0` the gate trivially passes but
   `print_clusters` filters singletons at output time.
6. `max_k` decreases monotonically across iterations:
   `max_k(it+1) ≤ max_k(it)`, strictly when any node is peeled.
7. `modularity = 1` for every accepted cluster in shipped output
   (dead-gate constant from
   [`run_ikc.py:280`](../../src/ikc/run_ikc.py#L280)). Paper-formula
   values exist only on the page when `canonicalGate=false` (the
   page-only educational toggle).

## Variable name mapping

| canonical (`run_ikc.py`) | JS port (`ikc.js`) | page label |
| --- | --- | --- |
| `graph` (residual) | `remG` + `remaining[]` mask | residual |
| `orig_graph` | `top` | original graph |
| `max_k` | `maxK` | K* (current max core) |
| `k` (CLI floor) | `kFloor` | k floor |
| `subgraph` (kcore extract) | `kcoreMask` + induced edges | (max_k)-core |
| `components` | `compsLocal` | components |
| `component` | `compLocal` | candidate cluster |
| `nodes_to_remove` | inline `remaining[i] = 0` | peeled |
| `final_clusters` | `accepted` | accepted clusters |
| `singletons` | `dropped` | unclustered nodes |
| `inverted_orig_node_ids` | `nodeIds[]` | id map (compact → orig) |
| `L` (full-graph edges) | `fullL` | L |
| `l_s` | `lS[ci]` | intra-edges |
| `d_s` | `dS[ci]` | intra-degree sum |
| `cluster_id_counter` | implicit (post-bijection) | cluster id |

## Paper-vs-shipped divergences

The shipped `run_ikc.py` implements a strict subset of Wedell 2022.
Four places where the binary diverges from the paper:

1. **Modularity gate is dead.** `modular()` at
   [`run_ikc.py:278-282`](../../src/ikc/run_ikc.py#L278) begins with
   `return POSITIVE_VALUE = 1`, so the paper's
   `mod(s) = l_s/L − (d_s/2L)²` at
   [`run_ikc.py:284-296`](../../src/ikc/run_ikc.py#L284) is never
   reached. K-validity is the only real filter. The page computes the
   paper formula for display only; its "strict mod gate" toggle
   enforces `mod(s) > 0` for educational comparison (page-only;
   canonical never runs it). On the 32-node fixture every accepted
   cluster has positive paper-modularity already, so the toggle leaves
   the partition unchanged; on larger benchmarks it shifts.
2. **m-validity is never enforced.** Paper §2.2.2 defines a cluster
   as m-valid iff its core-induced subgraph is connected and has
   positive modularity. The connectedness half is satisfied by
   construction since the gate input is already a weakly-connected
   component of the kcore subgraph. The modularity half rides on the
   dead gate above, so the m-valid predicate as a whole reduces to
   "connected", which is always true.
3. **p-validity is not implemented.** Paper §2.2.3 defines a four-stage
   `kmp-clustering` pipeline; p-validity is the Stage 3 noncore
   augmentation that grows each accepted core cluster by attaching
   outside nodes whose connection to the cluster meets a separate
   threshold. Shipped `src/ikc/` covers Stage 1 only, so every accepted
   cluster is exactly its core; no peripheral nodes get reattached.
4. **Modularity column in `com.csv` is a placeholder.** Each accepted
   cluster is written with `modularity = 1`, the dead-gate constant,
   not a measurement. Downstream consumers reading the column see 1
   for every cluster regardless of structure.

## Modularity-gate dead code

The paper's gate (Wedell 2022 §2.2.2):

```
mod(s) = ℓ_s / L − (d_s / 2L)²
```

where `ℓ_s` = intra-edges of cluster `s`, `d_s` = sum of intra-degrees,
`L` = total edges of the original graph. Implemented at
[`run_ikc.py:282-294`](../../src/ikc/run_ikc.py#L282) but unreachable
thanks to [`run_ikc.py:280`](../../src/ikc/run_ikc.py#L280):

```python
def modular(component, orig_graph, inverted_orig_node_ids):
    POSITIVE_VALUE = 1
    return POSITIVE_VALUE  # ← paper's formula below this line is dead
```

Net effect: IKC's only effective filter is k-validity, and every
accepted cluster is logged with `modularity = 1`, which shows up in
any post-processing that consumes the per-cluster modularity column.

The page's stage-2 walker computes the paper formula for display, with
a "strict mod gate" toggle that re-enables `mod(s) > 0` enforcement.
On the 32-node fixture every accepted cluster has positive modularity
already, so the toggle does not move the partition. On larger
benchmarks the divergence shows up.

## k_validity iterates the subgraph, not the component

A subtle quirk of [`run_ikc.py:266-277`](../../src/ikc/run_ikc.py#L266):
the function takes both `component` and `subgraph` (= the full kcore
subgraph) and iterates `subgraph.iterNodes()`, inspecting only nodes
that happen to be in `component`. Equivalent to iterating the
component directly, but stylistically odd. The degree threshold is
`degreeIn + degreeOut ≥ k` measured *in the kcore subgraph*; since
`component` is a WCC of `subgraph`, intra-subgraph degree equals
intra-component degree for any node in the component, so the predicate
matches paper §2.2.2's intra-component requirement.

## format_graph quirks

[`run_ikc.py:303-330`](../../src/ikc/run_ikc.py#L303) is the
canonical-side normaliser run before the kernel sees anything:

- **Directed weighted promotion.** The input edgelist is loaded as a
  directed weighted NetworKit graph. Edge weight is set to
  `degreeIn(u)` for each `(u, v)`. The kernel never reads the weight;
  it only ever queries `degreeIn` + `degreeOut`. Because the graph is
  directed-weighted, each undirected input edge appears once in each
  direction, so `k_valid`'s `degreeIn + degreeOut` sum counts each
  undirected edge exactly once.
- **Self-loop strip.** Line 326 calls `removeSelfLoops()` before the
  kernel runs. Self-loops never reach `kc()` or the gates.
- **Compact ids.** `pd.unique` over the source/target columns
  establishes a stable compact-id space; `inverted_orig_node_ids` is
  the inverse map back to original names.

The JS port mirrors all three behaviours in `compactSubgraph`
(self-loops dropped, undirected adjacency built once per endpoint pair,
compact ids match `pd.unique`'s F-order).

## Container iteration order

NetworKit's natural BFS order on the recompacted directed graph is
sensitive to edge-insertion order, which both sides canonicalise to
**lex-smallest BFS** for the JSON trace (BFS from smallest unseen
compact id, neighbours expanded ASC). The JS kernel sorts each node's
adjacency ASC in `compactSubgraph` + `buildResidualG`; the Python
canonical-tracer post-processes `cc.getComponents()` with a
`lex_smallest_bfs` helper to match.

For the CSV path (the `run_ikc.py`-byte-equal target on `com.csv`),
both sides keep NetworKit-natural component order so byte-equality
against the canonical binary's own `com.csv` holds. Both orderings hold
simultaneously on every test cell.

## Output

`com.csv` written by `print_clusters`
([`src/ikc/run_ikc.py:43-80`](../../src/ikc/run_ikc.py#L43)):

| Column | Meaning |
| --- | --- |
| `node_id` | Original node id (post-inverse-map) |
| `cluster_id` | Consecutive 0-indexed cluster label |
| `modularity` | Per-cluster modularity (always `1` from the dead gate) |
| `k` | Iteration's `max_k` at the moment of acceptance |

Singleton clusters (size ≤ 1) are dropped in the writer (line 65),
so dropped + bailed nodes never appear in `com.csv`.

## Place in the kmp-clustering pipeline

Wedell 2022 §2.2.3 defines a 4-stage `kmp-clustering` framework:

1. **Stage 1**: cluster `N` into k-valid + m-valid groups.
   Implementation = IKC (this page).
2. **Stage 2** (optional): break each non-singleton cluster from Stage
   1 into smaller k-valid pairwise disjoint clusters via Graclus.
3. **Stage 3** (optional): augment each non-singleton cluster with
   unclustered nodes as noncore (peripheral) members, provided they're
   adjacent to ≥ p core nodes in the chosen cluster.
4. **Stage 4**: assign core / noncore status + retain only kmp-valid
   clusters.

Only Stage 1 is implemented in `src/ikc/`; Stages 2-4 live in
`src/cm/` and the CM pipeline at
[`run_cd.sh`](../../run_cd.sh) (the post-processing chain that wraps
IKC with WCC / CM filters per Park 2024).

## Verification

Gold-standard 2-leg byte-equal pipeline. Per-step JSON trace bit-equal
across canonical-tracer (Python instrumented fork) and JS kernel
(`runIKC` + `__IKC_HOOK`). 63 of 63 cells PASS across the
`cd_stress_tiers` panel (T1 + T2 + T3, n = 800..23k,
`k_floor ∈ {2, 3, 5, 8, 10}`), 0 mismatches over 10,481 cumulative
records. Wall: 152s.

IKC has zero RNG calls, so audit rows A (RNG byte stream), B (draw
count + order), C (distribution mapping) collapse, and the playbook's
≥1M-record FP/seed bar is heuristic-only. The operative bar is full-panel
byte-equal under matching input.

Pipeline layout:

- [`tools/viz_check/ikc/TRACE_SCHEMA.md`](../../tools/viz_check/ikc/TRACE_SCHEMA.md):
  contract. Pins lex-smallest BFS for `members_iter_order` (the H-row
  closure rule).
- [`tools/viz_check/ikc/instrumented/kernel_check.py`](../../tools/viz_check/ikc/instrumented/kernel_check.py):
  Python canonical-tracer. `TRACER_MODE` env flag (vacuous for IKC
  since there's no RNG / FP swap to gate; only JSON emission).
  Lex-BFS canonicalisation for `members_iter_order` and
  `accepted_canonical_order.members_compact_in_orig_id`.
- `vltanh.github.io/comdet/js/ikc/ikc.js`: JS kernel + 6 hook events.
  Lex-smallest BFS via sorted adjacency in `compactSubgraph` +
  `buildResidualG`. Hook-installed-vs-uninstalled byte-equal final
  state.
- [`tools/viz_check/ikc/diff.mjs`](../../tools/viz_check/ikc/diff.mjs):
  Node harness. Spawns Python tracer in `nwbench` env, captures stdout
  JSON; loads `ikc.js` in vm with `__IKC_HOOK` recorder; reshapes JS
  event stream to schema-shaped object; walks lockstep + bit-compares
  via uint64 reinterpret on every numeric. Stops at first divergence,
  dumps path + values + bits + 5-element context.
- [`tools/viz_check/ikc/stress.py`](../../tools/viz_check/ikc/stress.py):
  stress runner over `cd_stress_tiers` panel × `k_floor` sweep.
- [`tools/viz_check/ikc/test_hook_equivalence.mjs`](../../tools/viz_check/ikc/test_hook_equivalence.mjs):
  hook-installed-vs-uninstalled equivalence test.

Equivalence tests:

| Test | Status | Notes |
| --- | --- | --- |
| (a) build-pair: TRACER_MODE=0 CSV vs `run_ikc.py` CSV | PASS | fixture32 + dnc |
| (b) self-determinism: TRACER_MODE=1 ×2 same input | PASS | fixture32 + dnc |
| (c) structural sanity: TRACER_MODE=1 CSV vs `run_ikc.py` CSV | PASS | subsumes (a) |
| (d) JS hook-installed-vs-uninstalled final state | PASS | 18 events captured, return value identical |

H-row closure: schema contract pins both `members_iter_order` and
`accepted_canonical_order.members_compact_in_orig_id` to lex-smallest
BFS. JS kernel sorts adjacency ASC; Python tracer re-runs BFS over
`cc.getComponents()` membership sets. CSV path keeps NetworKit-natural
order so `com.csv` stays byte-equal against the canonical binary.

Run:

```
cd community-detection
node tools/viz_check/ikc/diff.mjs <edge.csv> <k_floor> --fixture-name <name>
python tools/viz_check/ikc/stress.py [--tiers T1,T2,T3] [--ks 2,3,5,8,10]
```

(set `IKC_CONDA_ENV=<env>` if `nwbench` isn't right.)
</content>
</invoke>
# CC: technical reference

[← back to index](../algorithms.md)

Connected-Component split. Per-cluster BFS that catches algorithms
which emitted a label without checking that the cluster's induced
subgraph is actually connected. Park 2025 / Vu-Le 2026 introduced CC +
[WCC](./wcc.md) as standalone post-processors (no re-clustering loop,
unlike [CM](./cm.md)).

Companion to [`comdet/cc.html`](https://vltanh.me/comdet/cc.html). The
page explains the algorithm in plain English; this file holds the
implementation detail and source-code pointers.

## Provenance

- Papers: Park 2025 + Vu-Le 2026; full citations + DOIs in
  [`wcc.md` § Provenance](./wcc.md#provenance) (CC and WCC share both
  papers).
- Binary: same `constrained_clustering` C++ binary that hosts WCC and
  CM (Park 2024). CC is invoked as `MincutOnly --connectedness-criterion 0`;
  there is no separate `CC` subcommand.
- JS port: [`vltanh.github.io/comdet/js/cc/cc.js`](https://github.com/vltanh/vltanh.github.io/blob/main/comdet/js/cc/cc.js).
  Byte-equal vs the binary on 306 cells (17 fixtures × 9 seeds × two
  partition sources: legacy partition-synth and SBM-Flat-PP), 429,192
  cumulative output records, 0 mismatches. Verification harness at
  [`tools/viz_check/cc/`](../../tools/viz_check/cc/) (`stress_matrix.sh`
  for the legacy panel, `stress_3tier.py` for the SBM-Flat-PP panel).
  CC's `--connectedness-criterion` is fixed at `0`, so no parser walker
  is needed.

## Entrypoint

CLI:

```
constrained_clustering MincutOnly \
    --edgelist <path>.csv \
    --existing-clustering <path>.csv \
    --output-file com.csv \
    --log-file cc.log \
    --connectedness-criterion 0 \
    --num-processors <int>
```

Pipeline wrapper:
[`src/cc/pipeline.sh`](../../src/cc/pipeline.sh) fixes
`--connectedness-criterion 0` (the `Simple` branch in the binary) and
forwards the rest from `run_cd.sh`.

C++ entry: `MincutOnly::main` at
[`constrained-clustering/src/mincut_only.cpp:3`](../../constrained-clustering/src/mincut_only.cpp#L3).
With `criterion = "0"` the criterion parser falls into the `Simple`
branch ([`constrained.cpp:201-249`](../../constrained-clustering/src/constrained.cpp#L201)),
which short-circuits the recursive mincut loop: every connected
component of every input cluster's induced subgraph is emitted directly
([`mincut_only.cpp:42-46`](../../constrained-clustering/src/mincut_only.cpp#L42)).

## Stages

### Stage 1: load + remap

[`mincut_only.cpp:18-29`](../../constrained-clustering/src/mincut_only.cpp#L18).
Read edgelist, build an `original_id → consecutive_int` map, load the
graph through igraph. Read the existing clustering into a
`node_id → cluster_id` map via `ReadCommunities` at
[`mincut_only.cpp:29`](../../constrained-clustering/src/mincut_only.cpp#L29).

### Stage 2: strip inter-cluster edges

[`constrained.h:123`](../../constrained-clustering/includes/constrained.h#L123)
(`RemoveInterClusterEdges`). Iterates every edge; deletes any edge whose
endpoints differ in `cluster_id`. The graph still has the same vertex
set; only intra-cluster edges remain.

### Stage 3: connected components

[`constrained.h:393`](../../constrained-clustering/includes/constrained.h#L393)
(`GetConnectedComponents`). Wraps `igraph_connected_components(...,
IGRAPH_WEAK)`. Returns a `std::vector<std::vector<int>>` where each
inner vector is one component's node-id list. Singletons (component
size 1) are dropped at this layer
([`constrained.h:409`](../../constrained-clustering/includes/constrained.h#L409):
`if (size > 1)`).

### Stage 4: direct emit

[`mincut_only.cpp:43-45`](../../constrained-clustering/src/mincut_only.cpp#L43).
Under `Simple`, every component goes straight onto
`done_being_mincut_clusters`; no mincut, no recursion, no threshold
check. The output writer
([`constrained.cpp:135`](../../constrained-clustering/src/constrained.cpp#L135),
`WriteClusterQueue` in singleton-id form) assigns consecutive
0-indexed cluster ids on the fly.

## Output

`com.csv` (`node_id,cluster_id`, comma-separated, header row). Cluster
ids are renumbered consecutively by the writer
([`constrained.cpp:149`](../../constrained-clustering/src/constrained.cpp#L149)),
so they need not match the input partition's ids.

A history file is **not** written for CC (only CM produces
`history.log`).

## CLI flags

| Flag | What it sets |
| --- | --- |
| `--edgelist` | Input edgelist CSV (header `source,target`). |
| `--existing-clustering` | Input partition CSV (header `node_id,cluster_id`). |
| `--output-file` | Output partition. |
| `--log-file` | Log destination. Log level 1 = info; 2 = debug (per-component dump). |
| `--connectedness-criterion` | Fixed at `0` for CC by `src/cc/pipeline.sh`. |
| `--num-processors` | Worker thread count for the binary's mincut pool. Unused under `Simple` (no mincuts to dispatch). |
| `--mincut-type` | Binary default `cactus`; not consumed under `Simple`. |

Values added by the wrapper but not consumed by CC: `--mincut-type` and
`--num-processors > 1` exist for code-path symmetry with WCC + CM.

## Determinism

- igraph's BFS over `connected_components(IGRAPH_WEAK)` is deterministic
  in node-id order, given the same edge insertion order. The edge order
  comes from the input edgelist + the `original_to_new_id_map`
  (read-once map population in
  [`constrained.cpp:32-64`](../../constrained-clustering/src/constrained.cpp#L32)).
- `random_functions::setSeed(0)` at
  [`main.cpp:138`](../../constrained-clustering/src/main.cpp#L138) is
  set for binary symmetry but never consumed by the `Simple` branch.
- Output cluster id assignment is order-of-iteration over the
  `component_id_to_member_vector_map` (a `std::map`, sorted by component
  id ascending). Deterministic.

Same input → bit-identical `com.csv`, no seed knob needed.

## Difference from WCC and CM

| | CC | [WCC](./wcc.md) | [CM](./cm.md) |
|---|---|---|---|
| Threshold | `Simple` (= 0; trip on disconnect only) | `f(n) = log_10(n)` (default) | `f(n) = log_10(n)` (default; CM/pipeline.sh sets `0.2n^0.5`) |
| Loop | one pass | recursive split until well-connected | recursive split + re-cluster halves |
| Re-runs base method? | no | no | yes |
| Min cluster size `B` filter | optional (post-hoc) | optional (post-hoc) | yes (default 11; not enforced by binary, applied by upstream filter) |
| Pre-prune low-degree nodes? | no | no | yes (with `--prune`) |
| Heaviest? | lightest | medium | heaviest |

## Behaviour on the comdet 32-node fixture

Fixture at
[`vltanh.github.io/comdet/js/fixture.js`](../../vltanh.github.io/comdet/js/fixture.js):
32 nodes, 52 edges, four planted communities (A 12 + B 8 + C 6 + D 4)
plus two outliers.

If the input partition is the planted ground-truth (treating outliers
30, 31 as a singleton "cluster" or as their own labels):

| Cluster | Internal edges after stripping inter-cluster | Components |
| --- | --- | --- |
| A (12 nodes) | K_5 + 7-node periphery; every periphery node has a path to the K_5 | 1 |
| B (8 nodes)  | two K_4 plus the bridge edge 15–16 | 1 |
| C (6 nodes)  | two 3-cycles {20,21,22} and {23,24,25}, no internal edge between them | 2 |
| D (4 nodes)  | edges (26-27), (28-29), (27-28); a path | 1 |

CC splits C into `{20,21,22}` and `{23,24,25}`. A, B, D pass through.
Singleton outliers drop. Output partition has 5 clusters (A, B, C₁, C₂, D).

The single-cluster fates per Park 2025 / Vu-Le 2026 terminology
(extant, reduced, split) collapse to extant + split for CC, since there
is no threshold to "reduce" against.

## Step trace and tie-breaks

CC produces three record kinds per run, in this order:

| Record | Source | Field highlights |
| --- | --- | --- |
| `VISIT` | one per BFS pop inside `igraph_i_connected_components_weak` ([`components.c:147-176`](../../constrained-clustering/external_libs/igraph/src/connectivity/components.c#L147)) | `target_id`, `current_root`, `bfs_parent`, `comp_id`, `comp_size_so_far` |
| `FINALIZE_COMP` | one per completed BFS opening ([`components.c:179-182`](../../constrained-clustering/external_libs/igraph/src/connectivity/components.c#L179)) | `target_id = no_of_components`, `current_root`, final `comp_size` |
| `EMIT_CLUSTER` | one per surviving component popped from `done_being_mincut_clusters` ([`constrained.cpp:135-152`](../../constrained-clustering/src/constrained.cpp#L135)) | `target_id = cluster_id`, member node id list |

CC has zero tie-break decision sites. Every choice the algorithm makes
resolves to a node-id-ASC total order:

- BFS root selection: outer for-loop iterates `first_node = 0..vcount-1`
  ([`components.c:144`](../../constrained-clustering/external_libs/igraph/src/connectivity/components.c#L144)).
- Within-BFS pop order: FIFO queue + `igraph_neighbors(IGRAPH_ALL,
  IGRAPH_NO_LOOPS, IGRAPH_MULTIPLE)` adjacency order. The bucket-fill
  loop at [`constrained.h:403-411`](../../constrained-clustering/includes/constrained.h#L403)
  then overwrites discovery order with node-id-ASC contents.
- Component-id assignment: by counter increment, monotone in BFS-root
  order.
- Output cluster-id: by FIFO pop position in `WriteClusterQueue`.

The only adjacency-order knob is `igraph_neighbors`'s return order,
which is fixed by edge-insertion order at `igraph_add_edges` time
([`constrained.cpp:97`](../../constrained-clustering/src/constrained.cpp#L97)).
Canonical inserts edges in CSV row order; JS reads edges in array order
from the same CSV; both sides get the same adjacency order.

## Chain composition

CC sits downstream of a base community-detection algorithm in the
chained verification harness. The stress matrix wires CC against four
base algorithms (matching the WCC, CM, and VieCut chain sets so the
cross-algorithm coverage is parity-equivalent):

| Base | Source | Stress harness |
| --- | --- | --- |
| Leiden-Mod | `ModularityVertexPartition` from libleidenalg; see [`leiden.md`](./leiden.md). | `stress_3tier.py --partition leiden-mod` |
| Leiden-CPM(0.5) | `CPMVertexPartition` at resolution 0.5; see [`leiden.md`](./leiden.md). | `stress_3tier.py --partition leiden-cpm --resolution 0.5` |
| Infomap | v2.9.2 multi-level Louvain on the map equation; see [`infomap.md`](./infomap.md). | (planned, parity with WCC and CM)  |
| SBM-Flat-PP | graph-tool's `minimize_blockmodel_dl` with planted-partition prior; see [`sbm.md`](./sbm.md). | `stress_3tier.py --partition sbm-flat-pp` |

For each (base, fixture, seed) triple, the base produces a partition;
CC post-processes it; the survivor set is recorded in the audit grid.
The 161-fixture bumped 3-tier panel (T1+T2+T3, n ranging 800-23k) at
50 seeds per cell exercises all four chains under matching seeds.

The chain itself extends downstream: CC's output can be passed through
[`wcc.md`](./wcc.md) for the mincut-threshold pass, then through
[`cm.md`](./cm.md) for the re-clustering pass. Each post-proc consumes
a partition and emits a partition, so the chain is composable in any
prefix.

## Paper-vs-binary divergences

CC has no paper. The Park 2025 / Vu-Le 2026 papers describe CC as the
"Simple" or trivial baseline at the level of "split each cluster into
its connected components" and reserve their analysis for WCC and CM.
The shipping binary adds details the papers do not specify:

| Paper position | Binary behaviour |
| --- | --- |
| Singleton handling unspecified | Binary drops every component of size 1 at [`constrained.h:409`](../../constrained-clustering/includes/constrained.h#L409) (`csize > 1` filter). Nodes in singleton components end up unlabelled in `com.csv`. |
| Component-id assignment unspecified | Binary collects components in `std::map<int,vector<int>>` keyed by component id ([`constrained.h:415-417`](../../constrained-clustering/includes/constrained.h#L415)), then `WriteClusterQueue` ([`constrained.cpp:135-152`](../../constrained-clustering/src/constrained.cpp#L135)) assigns dense 0-indexed cluster ids in queue-pop order. |
| BFS root order unspecified | Binary delegates to `igraph_i_connected_components_weak`'s outer for-loop, which iterates `first_node = 0..vcount-1`. Lowest unvisited node-id seeds every BFS. |

## JS-port divergences

The JS port at [`comdet/js/cc/cc.js`](../../vltanh.github.io/comdet/js/cc/cc.js)
mirrors the binary's `MincutOnly::main` under the `Simple` branch.
Divergences relative to the canonical binary, all behaviourally
equivalent:

| Layer | Cpp canonical | JS port |
| --- | --- | --- |
| Adjacency iteration order | `igraph_neighbors(IGRAPH_ALL, IGRAPH_NO_LOOPS, IGRAPH_MULTIPLE)` returns out-edges (sorted ASC by other endpoint) concatenated with in-edges (also ASC) | `sortAdjForIgraphOrder` sorts each per-node adjacency in ASC order ([`cc.js:82-86`](../../vltanh.github.io/comdet/js/cc/cc.js#L82)), collapsing the two-list concat to a single ASC list for simple undirected graphs. Byte-equal on the per-VISIT record. |
| BFS queue | `igraph_dqueue_int_t` with `pop_front` | `let head = 0; q[head++]` over a JS array. Same FIFO semantics. |
| Bucket-fill | `std::map<int, vector<int>>` (cid-ASC iteration) | `Map<int, int[]>` keyed by cid; keys are pulled via `Array.from(buckets.keys()).sort((a,b)=>a-b)` ([`cc.js:282-284`](../../vltanh.github.io/comdet/js/cc/cc.js#L282)). |
| Output cluster ids | `WriteClusterQueue` FIFO pop with `current_cluster_id++` | `allCompsByIdx.forEach((m, _) => finalAssign[m[k]] = outId; outId++)` ([`cc.js:340-354`](../../vltanh.github.io/comdet/js/cc/cc.js#L340)). |
| Multi-threading | thread pool exists in the binary but the `Simple` branch never dispatches work | single-threaded inline drain (equivalent under `--num-processors 1`). |
| Floating-point | none on the CC path | none on the CC path. |

## Verification posture

Byte-equal vs the binary on 306 cells (17 fixtures × 9 seeds × two
partition sources: legacy partition-synth and SBM-Flat-PP), 429,192
cumulative output records, 0 mismatches. The bumped 3-tier panel (161
fixtures × 50 seeds × 4 base partitioners) runs through the same audit
harness with the chain inputs above.

Tracer harness at
[`tools/viz_check/cc/instrumented/`](../../tools/viz_check/cc/instrumented/)
builds a per-step probe-emitting fork of the binary. The JS port's
`globalThis.CC_DUMP_PROBES = true` gate emits the same record format
([`cc.js:50-58`](../../vltanh.github.io/comdet/js/cc/cc.js#L50)); the
two streams diff to zero on every cell of the audit panel. CC's audit
grid (rows A through I) holds the full breakdown.

## Cross-references

- Sibling post-procs: [`wcc.md`](./wcc.md), [`cm.md`](./cm.md).
- Mincut backend (used by WCC and CM, not CC): [`viecut.md`](./viecut.md).
- Base clustering bases (chained pipeline): [`leiden.md`](./leiden.md),
  [`infomap.md`](./infomap.md), [`sbm.md`](./sbm.md).


# CC — technical reference

[← back to index](../algorithms.md)

Connected-Component split. Per-cluster BFS that catches algorithms which
emitted a label without checking that the cluster's induced subgraph is
actually connected. Park 2025 / Vu-Le 2026 introduced CC + WCC as
standalone post-processors (no re-clustering loop, unlike CM).

The page at [`comdet/cc.html`](https://vltanh.me/comdet/cc.html) walks
the algorithm on the 32-node fixture; this file is the technical
companion: where the code lives, what each stage does, and how the
shipped binary differs from the paper.

## Provenance

- Paper (canonical): Park M, Tabatabaee Y, Ramavarapu V, Liu B,
  Pailodi VK, Ramachandran R, Korobskiy D, Ayres F, Chacko G, Warnow T.
  "Improved community detection using stochastic block models."
  *Complex Networks and their Applications XII*, Springer, 2025.
  DOI: <https://doi.org/10.1007/978-3-031-82435-7_9>.
- Paper (extension): Vu-Le T-A, Park M, Chen E, Warnow T.
  "Using stochastic block models for community detection."
  *Applied Network Science* 11:2, 2026.
  DOI: <https://doi.org/10.1007/s41109-025-00747-2>.
  Local PDF: `~/Downloads/Research/s41109-025-00747-2.pdf`.
- Binary: same `constrained_clustering` C++ binary that hosts WCC and
  CM (Park 2024). CC is invoked as `MincutOnly --connectedness-criterion 0`;
  there is no separate `CC` subcommand.
- JS port (planned): `vltanh.github.io/comdet/js/cc/cc.js`. Walker
  pending the Phase 4c kernel port.

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
[`src/cc/pipeline.sh`](../../src/cc/pipeline.sh) — fixes
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

### Stage 1 — load + remap

[`mincut_only.cpp:18-23`](../../constrained-clustering/src/mincut_only.cpp#L18).
Read edgelist, build an `original_id → consecutive_int` map, load the
graph through igraph. Read the existing clustering into a
`node_id → cluster_id` map.

### Stage 2 — strip inter-cluster edges

[`constrained.h:123`](../../constrained-clustering/includes/constrained.h#L123)
(`RemoveInterClusterEdges`). Iterates every edge; deletes any edge whose
endpoints differ in `cluster_id`. The graph still has the same vertex
set; only intra-cluster edges remain.

### Stage 3 — connected components

[`constrained.h:393`](../../constrained-clustering/includes/constrained.h#L393)
(`GetConnectedComponents`). Wraps `igraph_connected_components(...,
IGRAPH_WEAK)`. Returns a `std::vector<std::vector<int>>` where each
inner vector is one component's node-id list. Singletons (component
size 1) are dropped at this layer
([`constrained.h:409`](../../constrained-clustering/includes/constrained.h#L409):
`if (size > 1)`).

### Stage 4 — direct emit

[`mincut_only.cpp:43-45`](../../constrained-clustering/src/mincut_only.cpp#L43).
Under `Simple`, every component goes straight onto
`done_being_mincut_clusters`; no mincut, no recursion, no threshold
check. The output writer
([`constrained.cpp:135`](../../constrained-clustering/src/constrained.cpp#L135),
`WriteClusterQueue` — singleton-id form) assigns consecutive
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

## Difference from CM (Park 2024)

CC is "the no-threshold, no-recursion subset of WCC". The full table:

| | CC | WCC | CM |
|---|---|---|---|
| Threshold | `Simple` (= 0; trip on disconnect only) | `f(n) = log_10(n)` (default) | `f(n) = log_10(n)` (default; CM/pipeline.sh sets `0.2n^0.5`) |
| Loop | one pass | recursive split until well-connected | recursive split + re-cluster halves |
| Re-runs base method? | no | no | yes |
| Min cluster size `B` filter | optional (post-hoc) | optional (post-hoc) | yes (default 11; not enforced by binary, applied by upstream filter) |
| Pre-prune low-degree nodes? | no | no | yes (with `--prune`) |
| Heaviest? | lightest | medium | heaviest |

CC is the cheapest of the three. Cost = one BFS pass per cluster. No
mincut backend touched.

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

The single-cluster fates per Park 2025 / Vu-Le 2026 terminology — extant,
reduced, split — collapse to extant + split for CC since there is no
threshold to "reduce" against.

## When CC is enough vs when WCC adds value

Vu-Le 2026 §4: on graph-tool flat dc/ndc/pp + nested SBMs,
**CC and WCC both improve accuracy** on most synthetic benchmarks; WCC's
gain is uniformly larger because it also catches the weak-cut case (a
cluster held together by a single bridge passes CC but fails WCC). The
exception is sparse near-empty large clusters where WCC over-splits;
there CC alone is the safer choice.

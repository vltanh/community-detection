# WCC: technical reference

[← back to index](../algorithms.md)

Well-Connected Clusters. Per-cluster mincut + threshold check; failing
clusters get cut + recursed. Stronger than [CC](./cc.md): catches the
case where a cluster is connected but only by a small edge cut.
Lighter than [CM](./cm.md): does not re-cluster the cut halves.

Companion to [`comdet/wcc.html`](https://vltanh.me/comdet/wcc.html).
The page explains the algorithm in plain English; this file holds the
implementation detail and source-code pointers.

## Provenance

- Paper (canonical): Park M, Tabatabaee Y, Ramavarapu V, Liu B,
  Pailodi VK, Ramachandran R, Korobskiy D, Ayres F, Chacko G, Warnow T.
  "Improved community detection using stochastic block models."
  *Complex Networks and their Applications XII*, Springer, 2025.
  DOI: <https://doi.org/10.1007/978-3-031-82435-7_9>.
- Paper (extension, user co-first author): Vu-Le T-A, Park M, Chen E,
  Warnow T. "Using stochastic block models for community detection."
  *Applied Network Science* 11:2, 2026.
  DOI: <https://doi.org/10.1007/s41109-025-00747-2>.
  Local PDF: `~/Downloads/Research/s41109-025-00747-2.pdf`.
- Threshold definition (canonical): Park M, et al. "Well-connectedness
  and community detection." *PLOS Complex Systems* 1(3):e0000009, 2024.
  DOI: <https://doi.org/10.1371/journal.pcsy.0000009>.
- Mincut backend (canonical): VieCut. Henzinger M, Noe A, Schulz C,
  Strash D. "Practical Minimum Cut Algorithms." *ACM J. Exp.
  Algorithmics* 23 (2018), Article 1.8. Cloned at
  [`community-detection/VieCut/`](../../VieCut/).
- Binary: `constrained_clustering MincutOnly` (see
  [`main.cpp:16`](../../constrained-clustering/src/main.cpp#L16):
  `mincut_only.add_description("WCC")`).
- JS port (planned): `vltanh.github.io/comdet/js/wcc/wcc.js` +
  shared `mincut.js`. Walker pending the Phase 4c kernel port.

## Entrypoint

CLI:

```
constrained_clustering MincutOnly \
    --edgelist <path>.csv \
    --existing-clustering <path>.csv \
    --output-file com.csv \
    --log-file wcc.log \
    --connectedness-criterion "1log_10(n)" \
    --mincut-type cactus \
    --num-processors <int>
```

Pipeline wrapper:
[`src/wcc/pipeline.sh`](../../src/wcc/pipeline.sh) defaults
`--connectedness-criterion` to `1log_10(n)`. Override with
`--criterion` on the wrapper. Sqrt variant per Park 2024 = `0.2n^0.5`.

C++ entry: `MincutOnly::main` at
[`constrained-clustering/src/mincut_only.cpp:3`](../../constrained-clustering/src/mincut_only.cpp#L3).

## Stages

### Stage 1: load + remap + strip inter-cluster

Same as CC ([`mincut_only.cpp:18-37`](../../constrained-clustering/src/mincut_only.cpp#L18)).
Edgelist + existing-clustering loaded; intra-cluster edges only
retained via `RemoveInterClusterEdges`.

### Stage 2: connected components seed

[`mincut_only.cpp:40`](../../constrained-clustering/src/mincut_only.cpp#L40).
The post-strip components seed the work queue
`MincutOnly::to_be_mincut_clusters`. Each component is a candidate
cluster that may or may not survive the mincut threshold.

### Stage 3: recursive mincut loop

The outer driver
([`mincut_only.cpp:51-95`](../../constrained-clustering/src/mincut_only.cpp#L51))
keeps spawning `num_processors` worker threads
([`mincut_only.cpp:66-77`](../../constrained-clustering/src/mincut_only.cpp#L66)),
each running `MinCutWorker`, until the work queue empties.

`MinCutWorker`
([`includes/mincut_only.h:39`](../../constrained-clustering/includes/mincut_only.h#L39)).
Per cluster pulled off the queue:

1. Build the cluster's induced subgraph
   ([`mincut_only.h:55-66`](../../constrained-clustering/includes/mincut_only.h#L55)).
2. Run VieCut on it via `MinCutCustom::ComputeMinCut`
   ([`src/mincut_custom.cpp:3`](../../constrained-clustering/src/mincut_custom.cpp#L3)).
   Returns `(in_partition, out_partition, edge_cut_size)`: the most
   balanced minimum cut, with the partition assignment per node.
3. Threshold check via `IsWellConnected`
   ([`includes/constrained.h:427`](../../constrained-clustering/includes/constrained.h#L427)).
4. If pass: cluster joins `done_being_mincut_clusters`.
   If fail: split into the two halves; for each half, apply
   `GetConnectedComponentsOnPartition`
   ([`includes/mincut_only.h:13`](../../constrained-clustering/includes/mincut_only.h#L13))
   (one extra BFS, since the half could itself be disconnected once the
   cut edges are removed) + push every component of size > 1 back onto
   the work queue.

The recursion terminates because every iteration strictly reduces
cluster size, and a cluster of size 1 short-circuits at
[`mincut_only.h:48`](../../constrained-clustering/includes/mincut_only.h#L48).

### Stage 4: emit

[`mincut_only.cpp:99-100`](../../constrained-clustering/src/mincut_only.cpp#L99).
`WriteClusterQueue` (singleton-id form,
[`constrained.cpp:135`](../../constrained-clustering/src/constrained.cpp#L135))
emits every member of `done_being_mincut_clusters` with consecutive
0-indexed cluster ids.

## Threshold parser

[`constrained.cpp:201-249`](../../constrained-clustering/src/constrained.cpp#L201)
(`InitializeConnectednessCriterion`). Parses one of three string
shapes:

| Shape | Meaning | `ConnectednessCriterion` enum |
| --- | --- | --- |
| `"0"` | Cut > 0 (i.e. just structural connectivity = CC mode) | `Simple` |
| `"<C>log_<x>(n)"` | `cut > C * log_x(n)` | `Logarithimic` |
| `"<C>n^<x>"` | `cut > C * n^x` | `Exponential` |
| `"piecewise"` | Cluster-size-banded thresholds (see below) | `Custom` |

`IsWellConnected`
([`constrained.h:427-471`](../../constrained-clustering/includes/constrained.h#L427)):

```c++
threshold_value = pre_computed_log * std::log(in_partition_size + out_partition_size);
is_close = std::abs(threshold_value - edge_cut_size) <= 1e-9;
node_connectivity = !is_close && threshold_value < edge_cut_size;
return node_connectivity;
```

Where `pre_computed_log = C / std::log(x)`, so `threshold_value =
C * log_x(n_cluster)`. `is_close` guards against cuts that round to
exactly the threshold under floating-point arithmetic; those are
treated as fail (not pass).

`piecewise` (custom)
([`constrained.h:451-462`](../../constrained-clustering/includes/constrained.h#L451)):

| Cluster size n | Required cut |
| --- | --- |
| n < 100 | ≥ 1 |
| 100 ≤ n ≤ 500 | ≥ 2 |
| 500 < n ≤ 999 | ≥ 3 |
| n > 999 | ≥ ceil(sqrt(n) / 10) |

## Mincut backend

`MinCutCustom::ComputeMinCut`
([`src/mincut_custom.cpp`](../../constrained-clustering/src/mincut_custom.cpp))
constructs a VieCut `mutable_graph`, then dispatches by `--mincut-type`:

| `--mincut-type` | Backend | File |
| --- | --- | --- |
| `cactus` (default) | `cactus_mincut<...>` from VieCut | `external_libs/VieCut/lib/algorithms/global_mincut/cactus/` |
| `noi` | `noi_minimum_cut<...>` | `external_libs/VieCut/lib/algorithms/global_mincut/noi_minimum_cut.h` |

Both emit a balanced minimum cut + the bipartition. VieCut's
`find_most_balanced_cut = true`
([`mincut_custom.cpp:30`](../../constrained-clustering/src/mincut_custom.cpp#L30))
selects the most-balanced cut among ties. Without the balance
constraint, an arbitrary tie-break could peel one node at a time and
stall the recursion.

VieCut's RNG is `random_functions::setSeed(0)` at
[`main.cpp:138`](../../constrained-clustering/src/main.cpp#L138).
Hardcoded to 0; not exposed as a CLI flag.

## CLI flags

| Flag | What it sets |
| --- | --- |
| `--edgelist` | Input edgelist CSV. |
| `--existing-clustering` | Input partition CSV. |
| `--output-file` | Output partition CSV. |
| `--log-file` | Log destination. |
| `--log-level` | 0 = silent, 1 = info, 2 = debug. |
| `--connectedness-criterion` | Threshold string (default `1log_10(n)`). |
| `--mincut-type` | `cactus` (default) or `noi`. |
| `--num-processors` | Number of mincut worker threads. The thread-pool path runs whenever `to_be_mincut_clusters.size() > 1` ([`mincut_only.cpp:64`](../../constrained-clustering/src/mincut_only.cpp#L64)); the inline single-threaded fallback at [`mincut_only.cpp:78-81`](../../constrained-clustering/src/mincut_only.cpp#L78) fires only when the queue holds at most one cluster, regardless of `--num-processors`. |

## Output

`com.csv` (consecutive 0-indexed cluster ids; header
`node_id,cluster_id`). No `history.log` (only CM emits one).

## Determinism

| Source | Behaviour |
| --- | --- |
| Cactus / NOI mincut | Deterministic given graph + VieCut seed. Seed is hardcoded to 0. |
| Tie-break among equally balanced cuts | VieCut picks by internal node-id; depends on insertion order, stable given the same input. |
| Worker thread interleaving | Cluster ids in `com.csv` depend on which thread pops next. Output partition (the partition itself) is stable; cluster id labels are not, under `--num-processors > 1`. Under `--num-processors 1`, two runs on the same input give byte-identical `com.csv`. |

## Behaviour on the comdet 32-node fixture

Topology per [cc.md § Behaviour on the comdet 32-node
fixture](./cc.md#behaviour-on-the-comdet-32-node-fixture). Below:
mincut + threshold pass/fail under `1log_10(n_cluster)`, the binary's
default.

Walking through the four planted clusters of the input partition. The
seed step (mincut_only.cpp:40) splits C into its two 3-cycles before
the mincut loop sees it, so C never enters as a single n=6 cluster:

| Cluster | n | log_10(n) | Mincut of induced subgraph | Pass? | Output |
| --- | --- | --- | --- | --- | --- |
| A     | 12 | 1.079 | 1 (peel any of the four 1-edge periphery nodes 5, 6, 7, 11) | fail | recurses |
| B     | 8  | 0.903 | 1 (the bridge 15–16) | pass (cut > log) | extant |
| C₁    | 3  | 0.477 | 2 (each 3-cycle is 2-edge-connected) | pass | extant |
| C₂    | 3  | 0.477 | 2 | pass | extant |
| D     | 4  | 0.602 | 1 (edge 27–28) | pass (cut > log) | extant |

A's recursion peels three 1-edge periphery nodes one at a time (some
permutation of 5, 6, 7, 11). Each peeled side is a singleton and is
dropped by the size-1 short-circuit at
[`mincut_only.h:48`](../../constrained-clustering/includes/mincut_only.h#L48).
Peeling stops at n=9: the residual still has one pendant (mincut 1),
but log_10(9) ≈ 0.954 and 1 > 0.954, so the 9-node residual passes.
Empirically (cactus mincut, num-processors 1), A's residual is
`{0,1,2,3,4,8,9,10,11}`: 5, 6, 7 peeled; 11 retained because the
threshold falls below 1 once n drops to 9.

End state: A becomes its 9-node residual (K_5 + nodes 8, 9, 10, 11).
B extant. C splits into two 3-cycles via the seed step. D extant.
Singleton outliers + the three peeled periphery pendants vanish from
the output.

Under the `0.2n^0.5` (sqrt) criterion the threshold drops on small
clusters (n=12: 0.693; n=8: 0.566; n=4: 0.4). Every cluster passes on
first mincut: A's cut 1 > 0.693, so no peeling; B, C₁, C₂, D as before.
The two criteria disagree on A in this fixture and agree on the rest.


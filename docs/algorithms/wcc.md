# WCC — technical reference

[← back to index](../algorithms.md)

Well-Connected Clusters. Per-cluster mincut + threshold check; failing
clusters get cut + recursed. Stronger than [CC](./cc.md): catches the
case where a cluster is connected but only by a small edge cut.

The page at [`comdet/wcc.html`](https://vltanh.me/comdet/wcc.html) walks
the algorithm on the 32-node fixture; this file is the technical
companion.

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
- JS port (planned): `vltanh.github.io/comdet/js/cc/wcc.js` +
  `mincut.js`. Walker pending the Phase 4c kernel port.

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
[`src/wcc/pipeline.sh`](../../src/wcc/pipeline.sh) — defaults
`--connectedness-criterion` to `1log_10(n)`. Override with
`--criterion` on the wrapper. Sqrt variant per Park 2024 = `0.2n^0.5`.

C++ entry: `MincutOnly::main` at
[`constrained-clustering/src/mincut_only.cpp:3`](../../constrained-clustering/src/mincut_only.cpp#L3).

## Stages

### Stage 1 — load + remap + strip inter-cluster

Same as CC ([`mincut_only.cpp:18-37`](../../constrained-clustering/src/mincut_only.cpp#L18)).
Edgelist + existing-clustering loaded; intra-cluster edges only
retained via `RemoveInterClusterEdges`.

### Stage 2 — connected components seed

[`mincut_only.cpp:40`](../../constrained-clustering/src/mincut_only.cpp#L40).
The post-strip components seed the work queue
`MincutOnly::to_be_mincut_clusters`. Each component is a candidate
cluster that may or may not survive the mincut threshold.

### Stage 3 — recursive mincut loop

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
   Returns `(in_partition, out_partition, edge_cut_size)` — the most
   balanced minimum cut, with the partition assignment per node.
3. Threshold check via `IsWellConnected`
   ([`includes/constrained.h:427`](../../constrained-clustering/includes/constrained.h#L427)).
4. If pass: cluster joins `done_being_mincut_clusters`.
   If fail: split into the two halves; for each half, apply
   `GetConnectedComponentsOnPartition`
   ([`includes/mincut_only.h:13`](../../constrained-clustering/includes/mincut_only.h#L13))
   (one extra BFS — the half could itself be disconnected once the cut
   edges are removed) + push every component of size > 1 back onto the
   work queue.

The recursion terminates because every iteration strictly reduces
cluster size, and a cluster of size 1 short-circuits at
[`mincut_only.h:48`](../../constrained-clustering/includes/mincut_only.h#L48).

### Stage 4 — emit

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
| `noi` | `noi_minimum_cut<...>` | `external_libs/VieCut/lib/algorithms/global_mincut/noi.h` |

Both emit a balanced minimum cut + the bipartition. VieCut's
`find_most_balanced_cut = true` (set at
[`mincut_custom.cpp:30`](../../constrained-clustering/src/mincut_custom.cpp#L30))
selects the most-balanced cut among ties — important for the recursion
to make progress on adversarial graphs.

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
| `--num-processors` | Number of mincut worker threads (≥ 2 spawns the thread pool path; 1 runs single-threaded inline at [`mincut_only.cpp:78-80`](../../constrained-clustering/src/mincut_only.cpp#L78)). |

## Output

`com.csv` (consecutive 0-indexed cluster ids; header
`node_id,cluster_id`). No `history.log` (only CM emits one).

## Determinism

| Source | Behaviour |
| --- | --- |
| Cactus / NOI mincut | Deterministic given graph + VieCut seed. Seed is hardcoded to 0. |
| Tie-break among equally balanced cuts | VieCut picks by internal node-id; depends on insertion order — stable given the same input. |
| Worker thread interleaving | Cluster ids in `com.csv` depend on which thread pops next. **Output partition (the partition itself) is stable; cluster id labels are not** under `--num-processors > 1`. Set `--num-processors 1` for byte-stable output. |

Empirical: under `--num-processors 1`, two runs on the same input give
byte-identical `com.csv`.

## Behaviour on the comdet 32-node fixture

Fixture at
[`vltanh.github.io/comdet/js/fixture.js`](../../vltanh.github.io/comdet/js/fixture.js):
32 nodes, 52 edges. Threshold sweep below uses `1log_10(n_cluster)`,
the binary's default.

Walking through the four planted clusters of the input partition (after
inter-cluster edges removed):

| Cluster | n | log_10(n) | Mincut of induced subgraph | Pass? | Output |
| --- | --- | --- | --- | --- | --- |
| A | 12 | 1.079 | 1 (cut off any periphery node attached by a single edge: 5, 6, 7, 11) | fail | recurses |
| B | 8 | 0.903 | 1 (the bridge 15–16) | pass (cut > log) | extant |
| C | 6 | 0.778 | 0 (two disconnected 3-cycles after strip) | fail | splits + each 3-cycle re-evaluated |
| D | 4 | 0.602 | 1 (edge 27–28) | pass (cut > log) | extant |

A's recursion peels periphery nodes one at a time until the residual
is a 5-node K_5 (whose mincut is 4 ≥ log_10(5) ≈ 0.699 → pass). Each
peeled-off pendant has size 1 and is dropped at the size-1 short-circuit
([`mincut_only.h:48`](../../constrained-clustering/includes/mincut_only.h#L48)).

C's recursion: split into {20,21,22} and {23,24,25} (both 3-cycles,
mincut 2 each). 2 > log_10(3) ≈ 0.477 → both pass.

End state: A loses every periphery node, becoming the K_5 only. B
extant. C splits into two 3-cycles. D extant. Outliers + dropped
periphery nodes vanish from the output.

Under the `0.2n^0.5` (sqrt) criterion the threshold rises faster: A's
K_5 (n=5, threshold 0.447, mincut 4) still passes; B (n=8, threshold
0.566, mincut 1) still passes (1 > 0.566); C (n=3, threshold 0.346,
mincut 2) passes; D (n=4, threshold 0.4, mincut 1) passes (1 > 0.4).

## When WCC adds value over CC

Vu-Le 2026 §4: WCC's recursive split-on-weak-cut catches the
"connected but only just" case (cluster B in this fixture before being
ground-truth-aligned: a single bridge edge linking two K_4's). CC misses
this. The cost is: WCC may over-split when a cluster's true ground
truth has a sparse core (e.g. tree-like communities whose mincut is
1 by design); the paper recommends WCC only when the input clustering
is suspected to over-merge.

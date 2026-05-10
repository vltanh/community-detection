# CM: technical reference

[← back to index](../algorithms.md)

Connectivity Modifier. The full Park 2024 pipeline: [WCC](./wcc.md)'s
recursive mincut, plus a re-clustering pass on each split half via a
base algorithm (Leiden CPM, Leiden Mod, Louvain, Infomap). Heaviest of
the three connectivity post-procs ([CC](./cc.md), WCC, CM).

Companion to [`comdet/cm.html`](https://vltanh.me/comdet/cm.html). The
page explains the algorithm in plain English; this file holds the
implementation detail and source-code pointers.

Pipeline shape per Park 2024:

1. Initial base clustering produces the input partition.
2. Filter pass: drop clusters of size below `B` (default 11) plus
   tree-shaped clusters.
3. CM loop: per cluster, repeat {mincut, threshold check, base-method
   recluster of any side > 1} until every leaf cluster is
   well-connected.
4. Final filter: drop sub-clusters that ended up below `B`.

Stages 2 and 4 live in `cm_pipeline`'s Python orchestration around the
binary; the C++ binary (and the JS port that mirrors it) implements
stage 3 plus the initial connected-components seed step.

## Provenance

- Paper (canonical): Park M, Tabatabaee Y, Ramavarapu V, Liu B,
  Pailodi VK, Ramachandran R, Korobskiy D, Ayres F, Chacko G, Warnow T.
  "Well-connectedness and community detection."
  *PLOS Complex Systems* 1(3):e0000009, 2024.
  DOI: <https://doi.org/10.1371/journal.pcsy.0000009>.
  Local PDF: `~/Downloads/Research/journal.pcsy.0000009.pdf`.
- Reference pipeline: <https://github.com/illinois-or-research-analytics/cm_pipeline>
  (`cm_pipeline`, the bash + python orchestration around the binary).
- Binary: `constrained_clustering CM` at
  [`constrained-clustering/src/cm.cpp:3`](../../constrained-clustering/src/cm.cpp#L3).
- Mincut backend: VieCut, same as [WCC](./wcc.md).
- Base re-clustering algorithm: Leiden via libleidenalg `Optimiser`
  ([`constrained.h:301`](../../constrained-clustering/includes/constrained.h#L301)
  for `leiden-{cpm,mod}`); Louvain and Infomap listed in `main.cpp`
  route through igraph's bundled implementations
  ([`constrained.h:243, 279`](../../constrained-clustering/includes/constrained.h#L243)),
  not libleidenalg. The wrapper at `src/cm/pipeline.sh` accepts
  `leiden-cpm`, `leiden-mod`, and `louvain`; `infomap` is rejected.
- JS port: [`vltanh.github.io/comdet/js/cm/cm.js`](https://github.com/vltanh/vltanh.github.io/blob/main/comdet/js/cm/cm.js).
  L4 self-RNG byte-equal vs canonical tracer (TRACER_MODE swapped
  build) across 360 stress cells (legacy 6-fixture × 9-seed + 3-tier
  17-fixture × 9-seed × {Leiden input, SBM-Flat-PP input} × Leiden-Mod
  base). See `community-detection/tools/viz_check/cm/` for the harnesses.

## Entrypoint

CLI:

```
constrained_clustering CM \
    --edgelist <path>.csv \
    --existing-clustering <path>.csv \
    --algorithm leiden-cpm \
    --clustering-parameter 0.01 \
    --output-file com.csv \
    --history-file history.log \
    --log-file cm.log \
    --connectedness-criterion "0.2n^0.5" \
    --mincut-type cactus \
    --num-processors <int>
```

Pipeline wrapper:
[`src/cm/pipeline.sh`](../../src/cm/pipeline.sh) defaults
`--connectedness-criterion` to `0.2n^0.5` (sqrt criterion, the CM
study's default). `--base-algo` accepts `leiden-cpm`, `leiden-mod`,
or `louvain` ([`pipeline.sh:53-57`](../../src/cm/pipeline.sh#L53));
`--base-algo leiden-cpm` requires `--base-resolution`.

C++ entry: `CM::main` at
[`constrained-clustering/src/cm.cpp:3`](../../constrained-clustering/src/cm.cpp#L3).

## Stages

### Stage 1: load + initial partition

[`cm.cpp:4-26`](../../constrained-clustering/src/cm.cpp#L4).
Edgelist + existing-clustering loaded.
`RemoveInterClusterEdges` strips inter-cluster edges (same as CC, WCC).
`cluster_id_to_node_id_map` = reverse map. `next_cluster_id` = max
existing id + 1; reserved for newly-spawned subclusters during the
loop.

If `--existing-clustering` is empty the binary runs the base algorithm
on the input graph directly to seed the partition
([`cm.cpp:18-19`](../../constrained-clustering/src/cm.cpp#L18)). The
`src/cm/pipeline.sh` wrapper always passes `--existing-clustering`, so
this branch is dead in our deployment.

### Stage 2: seed the work queue

[`cm.cpp:29-57`](../../constrained-clustering/src/cm.cpp#L29). For each
weakly-connected component of the post-strip graph:

- If the component matches an existing cluster exactly (same node set),
  reuse the existing cluster id; this component is the "root" of that
  cluster's lineage in `parent_to_child_map`.
- Otherwise the component is a sub-component of a CC-style split (the
  same cluster has multiple components). The first such sub-component
  inherits the existing id; the rest get `next_cluster_id++` and are
  recorded as children of the original id.

Every component pushed onto `to_be_mincut_clusters` as
`(member_vector, cluster_id)`.

### Stage 3: modify-or-recluster loop

[`cm.cpp:58-97`](../../constrained-clustering/src/cm.cpp#L58). The
outer loop spawns `num_processors` workers per round; each runs
`MinCutOrClusterWorker`. A round ends when every worker pulls a
sentinel `({-1}, -1)`.

`MinCutOrClusterWorker`
([`includes/cm.h:41`](../../constrained-clustering/includes/cm.h#L41)).
Per cluster:

1. Build induced subgraph
   ([`cm.h:54-72`](../../constrained-clustering/includes/cm.h#L54)).
2. **Inner mincut-prune loop**
   ([`cm.h:76-121`](../../constrained-clustering/includes/cm.h#L76)).
   Repeat:
   - Run mincut.
   - If well-connected: break (cluster passes).
   - If trivial cut (one side has size 1) and `--prune` is on: remove
     the singleton from the induced subgraph + retry mincut.
     Equivalent to "iteratively peel low-degree nodes whose removal
     won't help cluster cohesion": the Park 2024 *Stage 3 pre-prune*.
   - Else: keep the cut; break.

   With `--prune` off (the wrapper's default for now), the loop runs
   exactly one mincut: any cut, trivial or not, is taken.
3. **If well-connected** (final state of step 2): emit the cluster
   onto `done_being_clustered_clusters`.
4. **Else** (a non-trivial cut survived): for each side of the cut
   that has size > 1, run the base algorithm on the induced subgraph
   restricted to that side
   (`RunClusterOnPartition` at [`cm.h:12`](../../constrained-clustering/includes/cm.h#L12)),
   then push every produced sub-cluster onto
   `to_be_clustered_clusters` for the next round.

Between rounds
([`cm.cpp:88-95`](../../constrained-clustering/src/cm.cpp#L88)) the
driver drains `to_be_clustered_clusters` into `to_be_mincut_clusters`,
assigning new cluster ids + recording parent links.

The recursion terminates when no cluster fails the threshold check;
i.e. every leaf cluster is well-connected under the criterion.

### Stage 4: emit + history

[`cm.cpp:100-102`](../../constrained-clustering/src/cm.cpp#L100).
`WriteClusterQueue` (pair form,
[`constrained.cpp:116`](../../constrained-clustering/src/constrained.cpp#L116))
writes the partition with each cluster's CM-tracked id (not renumbered
by the writer; the binary keeps the lineage ids). `WriteClusterHistory`
([`constrained.cpp:101`](../../constrained-clustering/src/constrained.cpp#L101))
writes `parent:child1,child2,...` lines, one per parent.

The history file is the cluster-fate audit trail per Park 2024 §4.2:
unchanged → entry maps the cluster's original id to itself; reduced /
split → entry lists the new children. Trees in the history file are
rooted at `-1` (the binary's "root" placeholder).

## Cluster fate vocabulary (Park 2024 §4.2)

Reading `history.log` yields the four canonical fates per input
cluster:

| Fate | History pattern |
| --- | --- |
| **Extant** | one child only, same id as parent |
| **Reduced** | one child, smaller node set (some nodes dropped via prune or below-`B` filter) |
| **Split** | two or more children |
| **Degraded** | every leaf descendant has size below the post-CM filter `B` (default 11). The binary does not apply `B`; `cm_pipeline` applies it downstream of the binary. Whether to apply it in this gallery is an analysis-side choice. |

## CLI flags

| Flag | What it sets |
| --- | --- |
| `--edgelist` | Input edgelist CSV. |
| `--existing-clustering` | Input partition CSV. Required by `src/cm/pipeline.sh` (the no-existing-clustering branch is unused). |
| `--algorithm` | Base re-clustering algorithm: `leiden-cpm`, `leiden-mod`, `louvain`, `infomap` per [`main.cpp:24`](../../constrained-clustering/src/main.cpp#L24). The wrapper accepts the first three; `infomap` is rejected. |
| `--clustering-parameter` | Resolution `r` for `leiden-cpm`. Defaults to 0.01 in the binary. Required by the wrapper for cpm. |
| `--output-file` | Output partition CSV. |
| `--history-file` | Lineage tree file. |
| `--log-file` | Log destination. |
| `--connectedness-criterion` | Same parser as WCC. Wrapper default `0.2n^0.5`. |
| `--mincut-type` | `cactus` or `noi`. |
| `--num-processors` | Worker thread count. |
| `--prune` | Implicit `true`. Enables the iterative singleton-prune branch in `MinCutOrClusterWorker`. Off by default; not enabled by the wrapper. |

## Output

| File | Form |
| --- | --- |
| `com.csv` | `node_id,cluster_id` rows; cluster ids are CM lineage ids (not renumbered to 0..K). |
| `history.log` | One line per parent: `<parent>:<child1>,<child2>,...`. `-1` is the synthetic root for input-cluster ids. |
| `cm.log` | Run log; per-iteration counts (info level). |

## Determinism

| Source | Behaviour |
| --- | --- |
| VieCut mincut (cactus or noi) | Deterministic given graph + seed. `random_functions::setSeed(0)` is called once at [`main.cpp:125`](../../constrained-clustering/src/main.cpp#L125); state carries across all Stage 3 rounds, so VieCut RNG is per-binary-run, not per-cluster-call. |
| Re-clustering pass | The base algorithm's RNG is re-seeded to 0 per call. For Leiden, `RunLeidenAndUpdatePartition` constructs a fresh `Optimiser` and calls `set_rng_seed(seed)` each time at [`constrained.h:302-303`](../../constrained-clustering/includes/constrained.h#L302); `seed = 0` set per worker iteration at [`cm.cpp:71`](../../constrained-clustering/src/cm.cpp#L71). Louvain and Infomap re-seed igraph's RNG via `igraph_rng_seed(..., seed)` at [`constrained.h:242, 278`](../../constrained-clustering/includes/constrained.h#L242). |
| Worker thread ordering | Cluster id assignment depends on which thread reaches the queue first; partition itself is stable, ids depend on `--num-processors`. Under `--num-processors 1`, two runs on the same input give byte-identical `com.csv`. |

## Behaviour on the comdet 32-node fixture

Topology per [cc.md § Behaviour on the comdet 32-node
fixture](./cc.md#behaviour-on-the-comdet-32-node-fixture). Two
threshold choices give two different stories on this fixture; both
walked below.

### Under `0.2n^0.5` (the wrapper default)

Every planted cluster passes on first mincut: A (n=12, threshold 0.693,
cut=1 → pass), B (n=8, threshold 0.566, cut=1 → pass), C₁/C₂ (n=3,
threshold 0.346, cut=2 → pass), D (n=4, threshold 0.4, cut=1 → pass).
CM never invokes the re-cluster pass; output partition equals the seed
partition (with C split by the connected-components seed step that runs
before the mincut loop). Every input cluster's fate is **extant** (or
**split** for C, which the seed step splits before CM sees it).

### Under `1log_10(n)` (Park 2024 canonical)

The threshold rises sharply for small clusters: log_10(12) ≈ 1.079,
log_10(8) ≈ 0.903. Now A fails (cut=1 < 1.079), and the
modify-or-recluster loop proceeds. The trace below collapses several
rounds; CM's per-cluster handling differs from WCC: a failing cut
sends each side (size > 1) through `RunClusterOnPartition` first
([`cm.h:130-148`](../../constrained-clustering/includes/cm.h#L130)),
and only the resulting Leiden communities re-enter the mincut queue
in the next round
([`cm.cpp:88-95`](../../constrained-clustering/src/cm.cpp#L88)).

1. Stage 2 seeds: A (12), B (8), C₁ (3), C₂ (3), D (4); C is split by
   the connected-components seed step before the mincut loop sees it.
2. Stage 3 unfolds (cactus mincut, Leiden-CPM r=0.01, num-processors 1):
   - A: cut 1 < 1.079 → fail. Cut peels a 1-edge pendant; the 1-node
     side is filtered by the size>1 guard at
     [`cm.h:136`](../../constrained-clustering/includes/cm.h#L136)
     and never re-enters the queue. The other side (now n=11) goes
     through `RunClusterOnPartition`: Leiden-CPM runs on the 11-node
     induced subgraph; the resulting communities go onto
     `to_be_clustered_clusters` and drain into the mincut queue for
     the next round. The Leiden re-cluster + mincut loop iterates
     until A's residual passes log_10. Empirically (binary run): 3
     successive A-reductions, lineage `0 → 6 → 7 → 8`, terminating at
     the 9-node residual `{0,1,2,3,4,8,9,10,11}`. Mincut of that
     residual is 1 (pendant 11), but log_10(9) ≈ 0.954 and 1 > 0.954
     → pass.
   - B: cut 1, threshold 0.903 → pass; extant.
   - C₁, C₂: cut 2, threshold log_10(3) ≈ 0.477 → pass; extant.
   - D: cut 1, threshold log_10(4) ≈ 0.602 → pass; extant.
3. Total CM iterations on this fixture: 4 (per `cm.log`). Output: 5
   clusters (A's 9-node residual, B, C₁, C₂, D). The 3 peeled
   periphery pendants (some permutation of 5, 6, 7, 11) re-cluster on
   their 1-node sides and vanish since size-1 sides never re-enter.

The log and sqrt criteria cross at n=100 (log_10(100) = 2 = 0.2·√100);
below n=100, log is the stricter threshold and produces more cuts.


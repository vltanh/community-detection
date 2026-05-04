# CM — technical reference

[← back to index](../algorithms.md)

Connectivity Modifier. The full Park 2024 pipeline: WCC's
recursive mincut + a re-clustering pass on each split half via a base
algorithm (Leiden CPM/Mod, Infomap, Louvain). Heaviest of the three
post-procs.

The page at [`comdet/cm.html`](https://vltanh.me/comdet/cm.html) walks
the algorithm on the 32-node fixture; this file is the technical
companion.

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
- Base re-clustering algorithm: libleidenalg `Optimiser` directly
  (`leiden-cpm` or `leiden-mod` for now; `louvain` and `infomap` listed
  in `main.cpp` but the wrapper at `src/cm/pipeline.sh` rejects anything
  other than `leiden-{cpm,mod}` per the active session's scope).
- JS port (planned): `vltanh.github.io/comdet/js/cc/cm.js`. Walker
  pending the Phase 4c kernel port + the Leiden kernel port.

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
[`src/cm/pipeline.sh`](../../src/cm/pipeline.sh) — defaults
`--connectedness-criterion` to `0.2n^0.5` (sqrt criterion, the CM
study's default). `--base-algo` constrained to `leiden-cpm` /
`leiden-mod` ([`pipeline.sh:48-52`](../../src/cm/pipeline.sh#L48));
`--base-algo leiden-cpm` requires `--base-resolution`.

C++ entry: `CM::main` at
[`constrained-clustering/src/cm.cpp:3`](../../constrained-clustering/src/cm.cpp#L3).

## Stages

### Stage 1 — load + initial partition

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

### Stage 2 — seed the work queue

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

### Stage 3 — modify-or-recluster loop

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
     won't help cluster cohesion" — the Park 2024 *Stage 3 pre-prune*.
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

The recursion terminates when no cluster fails the threshold check —
i.e. every leaf cluster is well-connected under the criterion.

### Stage 4 — emit + history

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
| **Degraded** | every leaf descendant has size below the post-CM filter `B` (default 11; not enforced by the binary itself, applied by an upstream filter) |

Cluster size filter `B` is **not** part of the binary (it ships as
post-processing in `cm_pipeline`); applying it in this gallery is an
analysis-side choice.

## CLI flags

| Flag | What it sets |
| --- | --- |
| `--edgelist` | Input edgelist CSV. |
| `--existing-clustering` | Input partition CSV. Required by `src/cm/pipeline.sh` (the no-existing-clustering branch is unused). |
| `--algorithm` | Base re-clustering algorithm: `leiden-cpm`, `leiden-mod`, `louvain`, `infomap` per [`main.cpp:24`](../../constrained-clustering/src/main.cpp#L24). The wrapper restricts to leiden. |
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
| VieCut mincut (cactus or noi) | Deterministic given graph + seed; seed hardcoded to 0 at [`main.cpp:125`](../../constrained-clustering/src/main.cpp#L125). |
| Re-clustering pass | The base algorithm's RNG. For Leiden, the binary calls `RunLeidenAndUpdatePartition` ([`constrained.h:301`](../../constrained-clustering/includes/constrained.h#L301)) which constructs an `Optimiser` directly with `seed = 0` ([`cm.cpp:71`](../../constrained-clustering/src/cm.cpp#L71)). |
| Worker thread ordering | Cluster id assignment depends on which thread reaches the queue first; partition itself is stable but ids depend on `--num-processors`. Use `--num-processors 1` for byte-stable output. |

Practical note: `random_functions::setSeed(0)` is set once for the
binary's lifetime; it's not re-seeded between the `Stage 3` rounds, so
later rounds inherit RNG state from earlier ones. This makes
reproducibility per-binary-run, not per-cluster-call.

## Behaviour on the comdet 32-node fixture

With the planted partition as input, threshold `0.2n^0.5`, base
algorithm `leiden-cpm` at resolution 0.01:

1. Stage 2 seeds: A (12), B (8), C₁ (3), C₂ (3), D (4) — C is split by
   the connected-components seed step before the mincut loop sees it.
2. Stage 3:
   - A: mincut 1, threshold 0.2·√12 ≈ 0.693 → fail. Cut peels a
     periphery pendant (e.g. node 5, 6, 7, or 11). Re-cluster the
     11-node residual + the 1-node side. The 1-node side falls below
     the singleton size 1 short-circuit and disappears. The 11-node
     residual goes back into the queue; recurses until residual is the
     K_5 (mincut 4, threshold 0.2·√5 ≈ 0.447 → pass).
   - B: mincut 1, threshold 0.2·√8 ≈ 0.566 → 1 > 0.566 so pass; extant.
   - C₁, C₂: mincut 2 each, threshold 0.2·√3 ≈ 0.346 → pass; extant.
   - D: mincut 1, threshold 0.2·√4 = 0.4 → 1 > 0.4 so pass; extant.
3. Output: A's K_5 alone (with peeled periphery emitted as separate
   leiden-cpm-clustered fragments — each fragment's own mincut likely
   passes on the small fragment, so they survive as singletons or
   small clusters depending on Leiden's call), B extant, C₁ extant,
   C₂ extant, D extant.

Under the `1log_10(n)` criterion thresholds drop and B fails: 1 ≤
log_10(8) only if log_10(8) ≥ 1, but log_10(8) ≈ 0.903 < 1, so B passes
under log too. The two thresholds disagree only on cluster sizes where
log_10(n) crosses 0.2·√n (around n ≈ 25); for sub-25-node clusters in
this fixture the two criteria mostly agree.

## CM vs WCC — why the re-clustering pass matters

WCC just cuts. CM cuts + re-clusters each side via the base algorithm.
The re-cluster pass lets the base algorithm decide whether the cut
edges (now removed) actually mean two coherent communities or whether
the cluster should be re-merged differently after the cut. Park 2024
§4.4 shows this matters most for Leiden-CPM at small `r` (heavily
over-merged at low resolution; CM peels off real sub-communities) and
matters least for IKC (already conservative).

## Place in the comdet pipeline

The CD pipeline cascade
([`run_cd.sh`](../../run_cd.sh), `--postproc cm` switch) wires CM
downstream of any base algorithm. Most useful as
`leiden-{cpm,mod} → cm`; less common but supported as
`sbm-* → cc → cm` (SBM tends to need CC first to fix internally
disconnected blocks before CM operates on coherent inputs).

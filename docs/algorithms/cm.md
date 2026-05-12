# CM: technical reference

[← back to index](../algorithms.md)

Connectivity Modifier. The full Park 2024 post-processing pipeline:
[WCC](./wcc.md)'s recursive mincut, plus a re-clustering pass on each
split half via a base algorithm (Leiden CPM, Leiden Mod, Louvain,
Infomap). Heaviest of the three connectivity post-procs
([CC](./cc.md), WCC, CM). The "modifier" framing matters: CM does not
just filter or split, it replaces failing clusters with whatever the
base algorithm decides after seeing each cut half in isolation.

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
binary. The C++ binary (and the JS port that mirrors it) implements
stage 3 plus the initial connected-components seed step. The size-`B`
filter and tree-cluster filter are not in the binary; they sit in the
Python wrapper that ships with `cm_pipeline`. Anything labelled
"degraded" in Park 2024 §4.2 originates from the wrapper's filter, not
the binary.

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
- Mincut backend: VieCut cactus mincut, same as [WCC](./wcc.md). See
  [`viecut.md`](./viecut.md) for the per-cluster cactus + most-balanced
  bipartition that CM consumes.
- Base re-clustering algorithm: Leiden via libleidenalg `Optimiser`
  ([`constrained.h:301`](../../constrained-clustering/includes/constrained.h#L301)
  for `leiden-{cpm,mod}`); Louvain and Infomap listed in
  [`main.cpp:24`](../../constrained-clustering/src/main.cpp#L24) route
  through igraph's bundled implementations
  ([`constrained.h:243, 278`](../../constrained-clustering/includes/constrained.h#L243)),
  not libleidenalg. The wrapper at `src/cm/pipeline.sh` accepts
  `leiden-cpm`, `leiden-mod`, and `louvain`; `infomap` is rejected at
  [`pipeline.sh:53-57`](../../src/cm/pipeline.sh#L53).
- JS port: [`vltanh.github.io/comdet/js/cm/cm.js`](https://github.com/vltanh/vltanh.github.io/blob/main/comdet/js/cm/cm.js).
  L4 self-RNG byte-equal vs canonical tracer (TRACER_MODE swapped
  build) across the stress matrix described under
  [§ Stress matrix](#stress-matrix). Harnesses live at
  [`community-detection/tools/viz_check/cm/`](../../tools/viz_check/cm/).

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
`--connectedness-criterion` to `0.2n^0.5` (the wrapper default; the
binary's own default is `1log_10(n)`, per
[`main.cpp:55`](../../constrained-clustering/src/main.cpp#L55), which
matches Park 2024). `--base-algo` accepts `leiden-cpm`, `leiden-mod`,
or `louvain` ([`pipeline.sh:53-57`](../../src/cm/pipeline.sh#L53));
`--base-algo leiden-cpm` requires `--base-resolution`.

C++ entry: `CM::main` at
[`constrained-clustering/src/cm.cpp:3`](../../constrained-clustering/src/cm.cpp#L3).

## Stages

### Stage 1: load + initial partition

[`cm.cpp:4-26`](../../constrained-clustering/src/cm.cpp#L4).
Edgelist + existing-clustering loaded.
`RemoveInterClusterEdges`
([`constrained.h:123`](../../constrained-clustering/includes/constrained.h#L123))
strips inter-cluster edges (same primitive as CC, WCC).
`cluster_id_to_node_id_map` = reverse map. `next_cluster_id` = max
existing id + 1; reserved for newly-spawned subclusters during the
loop.

If `--existing-clustering` is empty the binary runs the base algorithm
on the input graph directly to seed the partition
([`cm.cpp:18-19`](../../constrained-clustering/src/cm.cpp#L18)). The
`src/cm/pipeline.sh` wrapper always passes `--existing-clustering`, so
this branch is dead in our deployment.

### Stage 2: seed the work queue (lineage id assignment)

[`cm.cpp:29-57`](../../constrained-clustering/src/cm.cpp#L29).
`GetConnectedComponents`
([`constrained.h:393`](../../constrained-clustering/includes/constrained.h#L393))
returns the weakly-connected components of the post-strip graph,
iterated in `std::map<int,vector<int>>` cid-ASC order. For each
component:

- Let `first_node = component[0]` (the lowest node id in the
  component, since the BFS bucket-fill at `constrained.h:403` walks
  `node_id = 0..vcount-1` ASC).
- Let `orig_cid = node_id_to_cluster_id_map[first_node]` and
  `orig_size = cluster_id_to_node_id_map[orig_cid].size()`.
- If `orig_size == component.size()`, the component IS the entire
  original cluster: keep its id (`current_cluster_id = orig_cid`,
  `parent_cluster_id = -1`).
- Else the component is a strict subset of `orig_cid`'s nodes: assign
  `current_cluster_id = next_cluster_id++` and record
  `(parent = orig_cid, child = current_cluster_id)` in the lineage
  tree. The first fragment of each `orig_cid` also records
  `(parent = -1, child = orig_cid)` once (linear-scan dedupe at
  [`cm.cpp:42-50`](../../constrained-clustering/src/cm.cpp#L42)).

Every component is pushed onto `to_be_mincut_clusters` as
`(member_vector, cluster_id)`. The invariant going into stage 3: every
queue entry carries either an unchanged original cluster id or a fresh
id whose lineage parent points back to the original cluster.

### Stage 3: modify-or-recluster loop

[`cm.cpp:58-97`](../../constrained-clustering/src/cm.cpp#L58). The
outer loop spawns `num_processors` workers per round; each runs
`MinCutOrClusterWorker`. A round ends when every worker pulls a
sentinel `({-1}, -1)`.

`MinCutOrClusterWorker`
([`includes/cm.h:41`](../../constrained-clustering/includes/cm.h#L41)).
Per cluster:

1. Build induced subgraph
   ([`cm.h:54-72`](../../constrained-clustering/includes/cm.h#L54)) via
   `igraph_induced_subgraph_map(..., FROM_SCRATCH, ...,
   &new_id_to_old_id_vector_map)`. This emits a sub-id ↔ global-id
   translation that the worker uses to lift cut results back to global
   ids.
2. **Inner mincut-prune loop**
   ([`cm.h:76-121`](../../constrained-clustering/includes/cm.h#L76)).
   Repeat:
   - Run `MinCutCustom::ComputeMinCut`
     ([`mincut_custom.cpp:3`](../../constrained-clustering/src/mincut_custom.cpp#L3))
     → cactus mincut, returns `(edge_cut_size, in_partition,
     out_partition)`.
   - `IsWellConnected`
     ([`constrained.h:427`](../../constrained-clustering/includes/constrained.h#L427)):
     evaluate `threshold = pre_computed_log * log(in_size + out_size)`,
     check `cut > threshold` (with 1e-9 epsilon guard).
   - If `--prune` is on AND the cut is trivial (one side has size 1)
     AND the cluster is not yet well-connected: remove the singleton
     from the induced subgraph and retry mincut. Equivalent to Park
     2024 §3's *Stage 3 pre-prune*, which iteratively peels nodes whose
     degree inside the cluster is below `f(n)`. The binary's prune
     branch only handles the size-1 trivial-cut variant; the full
     degree-band prune lives in `cm_pipeline`'s wrapper.
   - Else: break with the current cut.

   With `--prune` off (the wrapper's default for now,
   [`pipeline.sh:72-83`](../../src/cm/pipeline.sh#L72) does not pass
   the flag), the loop runs exactly one mincut: any cut, trivial or
   not, is taken.
3. **If well-connected** (final state of step 2): emit the cluster
   onto `done_being_clustered_clusters`. The cluster is converted from
   `std::set<int>` to `std::vector<int>` at
   [`cm.h:125`](../../constrained-clustering/includes/cm.h#L125), so
   the emitted node list is sorted ASC.
4. **Else** (a non-trivial cut survived): for each side of the cut
   that has size > 1, run the base algorithm on the induced subgraph
   restricted to that side
   (`RunClusterOnPartition` at
   [`cm.h:12`](../../constrained-clustering/includes/cm.h#L12)). The
   produced sub-clusters are pushed onto `to_be_clustered_clusters`
   for the next round. Each push inherits `current_cluster_id` as its
   lineage parent.

Between rounds
([`cm.cpp:88-95`](../../constrained-clustering/src/cm.cpp#L88)) the
driver drains `to_be_clustered_clusters` into
`to_be_mincut_clusters`, assigning fresh ids from `next_cluster_id++`
and appending `(parent → child)` rows to `parent_to_child_map`. Rounds
proceed FIFO: order in `to_be_mincut` = order in which clusters were
pushed into `to_be_clustered` during the previous round.

The recursion terminates when no cluster fails the threshold check;
i.e. every leaf cluster is well-connected under the criterion.

### RunClusterOnPartition (the re-cluster subroutine)

[`includes/cm.h:12-39`](../../constrained-clustering/includes/cm.h#L12).
For one side of a failing cut:

1. Build a sub-induced-subgraph from the side's nodes (so the base
   algorithm sees only intra-side edges).
2. `GetCommunities` dispatches by `--algorithm`:
   - `leiden-cpm` → `CPMVertexPartition(resolution)` +
     `RunLeidenAndUpdatePartition` with `num_iter = 2`
     ([`constrained.h:301-324, 364-366`](../../constrained-clustering/includes/constrained.h#L301)).
   - `leiden-mod` → `ModularityVertexPartition` + same
     `RunLeidenAndUpdatePartition` with `num_iter = 2`
     ([`constrained.h:378-380`](../../constrained-clustering/includes/constrained.h#L378)).
     Distinct from Louvain's `Modularity` wrapper; this is
     libleidenalg's shape.
   - `louvain` → `igraph_community_multilevel`
     ([`constrained.h:275-298`](../../constrained-clustering/includes/constrained.h#L275)).
   - `infomap` → `igraph_community_infomap`
     ([`constrained.h:240-272`](../../constrained-clustering/includes/constrained.h#L240)).
3. `RemoveInterClusterEdges` on the sub-graph using the Leiden / Louvain
   partition, then `GetConnectedComponents` on the residual: any
   community that returned disconnected gets split into its connected
   pieces here.
4. Translate every node id sub → global via
   `new_id_to_old_id_map` and return the list of cluster vectors.

Why Leiden (or Louvain), not the original base algorithm globally?
The re-cluster runs on the induced subgraph of one cut half, so it
re-optimises the base objective restricted to that subgraph. A failing
cut means the base algorithm's global optimum bundled two real
communities together; restricting to the side gives the algorithm a
second look without the noise of the rest of the graph. Leiden
specifically is preferred over Louvain because Leiden's refinement
phase guarantees every output community is internally connected, so
the subsequent `GetConnectedComponents` step usually splits nothing
further. Louvain has no such guarantee, so the extra CC pass after
re-cluster matters more for Louvain-base CM runs.

### Stage 4: emit + history

[`cm.cpp:100-102`](../../constrained-clustering/src/cm.cpp#L100).
`WriteClusterQueue` (pair form,
[`constrained.cpp:116`](../../constrained-clustering/src/constrained.cpp#L116))
writes the partition with each cluster's CM-tracked id (not renumbered
by the writer; the binary keeps the lineage ids).
`WriteClusterHistory`
([`constrained.cpp:101`](../../constrained-clustering/src/constrained.cpp#L101))
writes `parent:child1,child2,...` lines, one per parent, iterated
parent-id ASC via `std::map`.

The history file is the cluster-fate audit trail per Park 2024 §4.2:
extant → entry maps the cluster's original id to itself; reduced or
split → entry lists the new children. Trees in the history file are
rooted at `-1` (the binary's "root" placeholder); every original
cluster id that ever spawned a fresh child shows up exactly once under
`-1`'s row.

## Cluster fate vocabulary (Park 2024 §4.2)

Reading `history.log` yields the four canonical fates per input
cluster:

| Fate | History pattern | Where the filter lives |
| --- | --- | --- |
| **Extant** | one child only, same id as parent | n/a (the cluster passed every threshold check) |
| **Reduced** | one child, smaller node set (some nodes dropped via `--prune` or post-CM size filter) | `--prune` (binary) or `cm_pipeline` post-filter `B` |
| **Split** | two or more children | Stage 3 base-algo recluster |
| **Degraded** | every leaf descendant has size below the post-CM filter `B` (default 11) | `cm_pipeline` post-filter only; the binary itself never marks a cluster as degraded. |

The "degraded" fate is exclusively a wrapper-side label: the binary
emits every survivor regardless of size, and the wrapper drops the
ones below `B` afterwards. Whether to apply `B` in this gallery is an
analysis-side choice; the JS walker leaves filtering off so every
survivor is visible.

## CLI flags

| Flag | What it sets |
| --- | --- |
| `--edgelist` | Input edgelist CSV. |
| `--existing-clustering` | Input partition CSV. Required by `src/cm/pipeline.sh` (the no-existing-clustering branch is unused). |
| `--algorithm` | Base re-clustering algorithm: `leiden-cpm`, `leiden-mod`, `louvain`, `infomap` per [`main.cpp:24`](../../constrained-clustering/src/main.cpp#L24). The wrapper accepts the first three; `infomap` is rejected. |
| `--clustering-parameter` | Resolution `r` for `leiden-cpm`. Defaults to 0.01 in the binary ([`main.cpp:30-33`](../../constrained-clustering/src/main.cpp#L30)). Required by the wrapper for cpm. Ignored for leiden-mod and louvain. |
| `--output-file` | Output partition CSV. |
| `--history-file` | Lineage tree file. |
| `--log-file` | Log destination. |
| `--connectedness-criterion` | Same parser as WCC. Binary default `1log_10(n)`; wrapper default `0.2n^0.5`. |
| `--mincut-type` | `cactus` (default) or `noi`. |
| `--num-processors` | Worker thread count. |
| `--prune` | Implicit `true`. Enables the iterative singleton-prune branch in `MinCutOrClusterWorker`. Off by default; not enabled by the wrapper. |

## Output

| File | Form |
| --- | --- |
| `com.csv` | `node_id,cluster_id` rows; cluster ids are CM lineage ids (not renumbered to 0..K). FIFO order from `done_being_clustered_clusters`. |
| `history.log` | One line per parent: `<parent>:<child1>,<child2>,...`. `-1` is the synthetic root for input-cluster ids. Parent rows in `std::map` ASC order; within-row in insertion order. |
| `cm.log` | Run log; per-iteration counts (info level). |

## Determinism

CM has two independent RNG streams:

| Stream | Engine | Seeding behaviour |
| --- | --- | --- |
| VieCut mincut | libstdc++ `std::mt19937` ([`tools/random_functions.h:25-27`](../../constrained-clustering/external_libs/VieCut/lib/tools/random_functions.h#L25)) | `random_functions::setSeed(0)` is called ONCE at [`main.cpp:125`](../../constrained-clustering/src/main.cpp#L125). The per-mincut re-seed at [`mincut_custom.cpp:37`](../../constrained-clustering/src/mincut_custom.cpp#L37) is commented out, so the VieCut RNG state DRIFTS across pops within a single binary run. Each pop draws from wherever the previous pop left the state. |
| Leiden / Louvain / Infomap (base algo) | igraph MT19937 (via libleidenalg `Optimiser` for Leiden; via igraph's bundled RNG for Louvain + Infomap) | Re-seeded to 0 per call. For Leiden, `RunLeidenAndUpdatePartition` constructs a fresh `Optimiser` and calls `set_rng_seed(seed)` each time at [`constrained.h:302-303`](../../constrained-clustering/includes/constrained.h#L302); `seed = 0` set per worker iteration at [`cm.cpp:71`](../../constrained-clustering/src/cm.cpp#L71). Louvain and Infomap re-seed igraph's RNG via `igraph_rng_seed(..., seed)` at [`constrained.h:242, 278`](../../constrained-clustering/includes/constrained.h#L242). |

| Other source | Behaviour |
| --- | --- |
| Worker thread ordering | Cluster id assignment depends on which thread reaches the queue first; partition itself is stable, ids depend on `--num-processors`. Under `--num-processors 1`, two runs on the same input give byte-identical `com.csv`. |
| Threshold tie | The cut value is an integer; the threshold is a double. The `is_close = abs(threshold - cut) <= 1e-9` guard at [`constrained.h:431`](../../constrained-clustering/includes/constrained.h#L431) catches cases where rounding makes the comparison fragile. Treated as fail (not well-connected), so the cluster is split. |
| FP path | The only FP primitive on the CM-specific path is `std::log` inside `IsWellConnected`. CPM / Modularity `diff_move` use only `+ - * /`. VieCut cactus mincut is integer arithmetic. |

igraph's RNG has a quirk worth flagging:
`igraph_rng_seed(rng, 0)` actually maps to internal seed 4357 (see
`igraph/src/random/rng_mt19937.c:92-94`). So when the binary asks for
seed 0, the Leiden / Louvain / Infomap streams are effectively seeded
4357. The JS port either uses 0 directly or applies the same
translation; the stress matrix avoids seed=0 to side-step this quirk.

## JS port pipeline + divergences from canonical

`vltanh.github.io/comdet/js/cm/cm.js` ports `cm.cpp` +
`includes/cm.h:41` `MinCutOrClusterWorker` + `includes/cm.h:12`
`RunClusterOnPartition`. It chains:

- `C.WCC.bfsComponents` for the residual-graph CC seed step.
- `C.WCC.parseCriterion` + `C.WCC.threshold` + `C.WCC.isWellConnected`
  for the threshold check (mirrors `constrained.h:427-433`).
- `C.MINCUT.viecut` (default) or `C.MINCUT.stoerWagner` (fallback) for
  the per-pop mincut.
- `C.LEIDEN.CPM` / `C.LEIDEN.LeidenMod` / `C.LOUVAIN.Modularity` for
  the re-cluster step, fed into `C.LEIDEN.optimisePartition` twice
  (`num_iter = 2`) to match `RunLeidenAndUpdatePartition` at
  `constrained.h:301-324`.

The port mirrors the canonical kernel under matching seed across the
stress matrix in [§ Stress matrix](#stress-matrix). Two structural
divergences from the unmodified canonical pipeline are preserved
under TRACER_MODE swaps but DO produce a bit-different end state if
the unmodified canonical is run side by side:

| Site | Canonical | JS port |
| --- | --- | --- |
| Inner mincut-prune loop ([`cm.h:91-115`](../../constrained-clustering/includes/cm.h#L91)) | iterates trivial-cut singleton removal when `--prune true` | not ported; the loop body always exits on the first mincut, mirroring `--prune false` (the wrapper default per [`pipeline.sh`](../../src/cm/pipeline.sh)) |
| `std::log` in `IsWellConnected` ([`constrained.h:430`](../../constrained-clustering/includes/constrained.h#L430)) | glibc faithfully-rounded `std::log` | V8 fdlibm `Math.log` (correctly-rounded for inputs that match canonical; differs on ~0.09% of inputs per glibc-vs-V8 audit memory) |
| Leiden RNG iter1→iter2 continuity | one `Optimiser` instance, RNG state carries across both calls | fresh MT19937 per `optimisePartition` call; tracer-side re-seeds before iter 2 to compensate |

The leiden iter1→iter2 RNG-continuity divergence is sanctioned: the
TRACER_MODE swap re-seeds Leiden between iter1 and iter2 so the
canonical-tracer matches the JS port bit-equal, while the unmodified
production canonical retains the original cross-iter RNG continuity.

JS port architectural notes:

- `runCM` accepts two optional oracles for the verification harness:
  `opts.cutOracle(nodes, sub) -> {cutValue, inPartition, outPartition}`
  feeds canonical mincut bipartitions directly; `opts.baseAlgoFn(side,
  sideEdges, ctx) -> Array<Array<id>>` feeds canonical Leiden output
  per side. With both oracles dropped, the JS chain runs end-to-end
  on its own MT19937 streams + fdlibm `Math.log`, and the verification
  is L4 self-RNG byte-equal.
- The `__CM_HOOK_*` callbacks (LD_BEGIN/END, INIT_LINEAGE,
  END_ROUND_DRAIN, POP_SINGLETON_COUNT, THR_DECOMP, PTC_SNAPSHOT,
  VC_BEGIN/END, PUSH) mirror the canonical tracer's `[TRACE-CM]`
  stderr probes, so `self_rng_check.mjs` can bit-compare each list.

## Paper vs binary divergences (Park 2024 → `constrained_clustering` CM)

| Concern | Park 2024 (paper) | Binary / wrapper |
| --- | --- | --- |
| Threshold default | `f(n) = log_10(n)` (§3) | binary: `1log_10(n)`; wrapper `pipeline.sh:21`: `0.2n^0.5` |
| Pre-prune (degree-band peel) | "iteratively remove every node with degree ≤ f(n) inside the cluster" (§3) | not in the binary; only the singleton-trivial-cut variant is in `--prune`, and the wrapper does not pass it. The Python wrapper `cm_pipeline` runs the full degree-band prune as a separate step. |
| Initial size filter B | `B = 11` (drop clusters below before stage 3) | not in the binary; lives in `cm_pipeline`'s Python orchestration. |
| Final size filter B | `B = 11` (drop sub-clusters below after stage 3) | not in the binary; the binary emits every survivor; `cm_pipeline` filters post-binary. |
| Tree-cluster filter | "drop tree-shaped clusters before stage 3 (mincut = 1 trivially)" | not in the binary; `cm_pipeline` filters post-stage-1. |
| Singleton push behaviour | not explicitly addressed | `cm.h:138-148` pushes EVERY base-algo output component to `to_be_clustered` regardless of size. A size-1 component re-enters next round, where its mincut on n=1 is degenerate (cut = 0, threshold > 0 → not well-connected); the singleton then re-enters the recluster step on a size-1 side, which is filtered by `cm.h:136` (`if (current_partition.size() > 1)`) and never pushed back. So singletons die after one extra round, not infinite-loop. JS mirrors. |
| Cluster fate labelling | Park 2024 §4.2 derives fates from history | the binary writes raw `history.log`; the four labels are computed by a separate analysis script. |

## Chain composition (CM consumes a base partition)

CM is a post-processor: it modifies an existing clustering. The chain
inputs that exercise CM in this gallery's stress matrix come from
four base partition sources, per
[chained_base_algos](../../memory/community-detection/cc-wcc-cm/chained_base_algos.md):

| Base partition source | Notes |
| --- | --- |
| Leiden-Mod (libleidenalg `ModularityVertexPartition`) | Maximises Newton-Girvan modularity. Tends to over-merge at the resolution limit; CM splits the over-merged blobs. |
| Leiden-CPM at resolution 0.5 | High-resolution CPM; produces small dense clusters. CM mostly leaves these extant. |
| Leiden-CPM at resolution 0.0001 | Low-resolution CPM; produces large clusters. CM aggressively splits them. The historical default in the CM paper's empirical study. |
| Infomap | Map-equation-based; produces structurally-motivated clusters. CM intervenes less here than on Leiden inputs. |
| SBM-Flat-PP | Planted-partition SBM. The block boundaries are by construction; CM stress-tests CM's behaviour when the input is internally consistent. |

The stress matrix runs each base partition × 17 fixtures × 9 seeds ×
CM with Leiden-Mod re-cluster. End-to-end byte-equal between the JS
walker and the canonical-tracer binary under matching seed (see
[§ Stress matrix](#stress-matrix)).

## Stress matrix

L4 self-RNG byte-equal verification (JS walker vs canonical-tracer
under matching seed, no oracles injected):

| Panel | Cells | Records | Mismatches |
| --- | --- | --- | --- |
| Legacy 6-fixture × 9-seed × leiden-cpm@0.0001 | 54 | 4,421 | 0 |
| 3-tier 17-fixture × 9-seed × leiden-mod ← leiden-mod input | 153 | 304,359 | 0 |
| 3-tier 17-fixture × 9-seed × leiden-mod ← SBM-Flat-PP input | 153 | 174,628 | 0 |
| 3-tier 17-fixture × 9-seed × leiden-cpm@0.5 ← leiden-cpm@0.5 input | 153 | 210,423 | 0 |

Cumulative coverage on the non-delegated CM path: ~694K records
across 513 cells (lineage assignment, FIFO queue dynamics, threshold
scalar). Mincut + Leiden + threshold-log primitives are
chain-delegated to [VieCut](./viecut.md), [Leiden](./leiden.md), and
[WCC](./wcc.md)'s `std::log` audit, each of which closes its own
≥1M-record floor.

Re-verified 2026-05-10 against fresh tracer builds; see
[`tools/viz_check/cm/`](../../tools/viz_check/cm/) for the harnesses
and [`tools/viz_check/cm/stress_3tier.py`](../../tools/viz_check/cm/stress_3tier.py)
for the 3-tier panel driver.

## Behaviour on the comdet 32-node fixture

Topology per [cc.md § Behaviour on the comdet 32-node
fixture](./cc.md#behaviour-on-the-comdet-32-node-fixture). Two
threshold choices give two different stories on this fixture; both
walked below.

### Under `0.2n^0.5` (the wrapper default)

Every planted cluster passes on first mincut: A (n=12, threshold
0.693, cut=1 → pass), B (n=8, threshold 0.566, cut=1 → pass), C₁/C₂
(n=3, threshold 0.346, cut=2 → pass), D (n=4, threshold 0.4, cut=1 →
pass). CM never invokes the re-cluster pass; output partition equals
the seed partition (with C split by the connected-components seed
step that runs before the mincut loop). Every input cluster's fate is
**extant** (or **split** for C, which the seed step splits before CM
sees it).

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
2. Stage 3 unfolds (cactus mincut, Leiden-CPM r=0.0001,
   num-processors 1):
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

The log and sqrt criteria cross at n=100 (log_10(100) = 2 =
0.2·√100); below n=100, log is the stricter threshold and produces
more cuts.

### What changes when the base algorithm changes

The 32-node fixture is small enough that Leiden-CPM at r=0.0001
returns the entire 11-node side as one community (its CPM optimum at
that resolution is a single cluster). Leiden-CPM at r=0.5 returns
multiple smaller communities; the queue then carries finer-grained
work into the next round. Leiden-Mod returns one community of size 11
on this fixture (the modularity optimum bundles the same side as
CPM). Louvain (modularity-greedy, not refined) produces the same
single 11-node community here. The difference between base algorithms
matters more on graphs where each cut side has internal structure the
base algorithm can resolve; on the 32-node fixture, every cut side is
either dense enough to remain one community or sparse enough to peel
into singletons.

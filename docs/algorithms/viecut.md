# VieCut: technical reference

[← back to index](../algorithms.md)

Global-minimum-cut primitive. Cactus tree (Karzanov 1973) over the
input graph plus a DFS-based most-balanced bipartition selector. Used
as the mincut backend under [WCC](./wcc.md) and [CM](./cm.md).

Companion to [`comdet/viecut.html`](https://vltanh.me/comdet/viecut.html).
The page explains the algorithm in plain English; this file holds the
implementation detail and source-code pointers.

## Provenance

- Papers:
  - Henzinger, Noe, Schulz, Strash. "Practical Minimum Cut Algorithms."
    *ALENEX 2018*. DOI: <https://doi.org/10.1137/1.9781611975055.6>.
  - Henzinger, Noe, Schulz, Strash. "Shared-memory Exact Minimum Cuts."
    *SPAA 2018*. DOI: <https://doi.org/10.1145/3210377.3210393>.
  - Karzanov. "The minimum cuts and maximum flows in a graph: a
    common framework." *Soviet Math. Dokl.* 1973 (cactus structure
    theorem).
  - Nagamochi, Ono, Ibaraki. "Implementing an efficient minimum
    capacity cut algorithm." *Math. Programming* 1992 (NOI capforest).
  - Padberg, Rinaldi. "An efficient algorithm for the minimum capacity
    cut problem." *Math. Programming* 1990 (PR12/PR34 tests).
  - Goldberg, Tarjan. "A new approach to the maximum-flow problem."
    *J. ACM* 1988 (push-relabel max-flow).
- Canonical C++ implementation cloned at:
  [`community-detection/VieCut/`](../../VieCut/) (MinhyukPark fork, pin
  `bc51bc18`, branch `master`). Header-only library; relevant entry
  points:
  - [`lib/algorithms/global_mincut/cactus/cactus_mincut.h`](../../VieCut/lib/algorithms/global_mincut/cactus/cactus_mincut.h)
    (top-level driver; outer-while + four contraction phases).
  - [`lib/algorithms/global_mincut/cactus/recursive_cactus.h`](../../VieCut/lib/algorithms/global_mincut/cactus/recursive_cactus.h)
    (`flowMincut` + `recursiveCactus` + `findSTCactus`).
  - [`lib/algorithms/global_mincut/cactus/balanced_cut_dfs.h`](../../VieCut/lib/algorithms/global_mincut/cactus/balanced_cut_dfs.h)
    (most-balanced bipartition DFS).
  - [`lib/algorithms/global_mincut/noi_minimum_cut.h`](../../VieCut/lib/algorithms/global_mincut/noi_minimum_cut.h)
    (NOI capforest).
  - [`lib/algorithms/global_mincut/viecut.h`](../../VieCut/lib/algorithms/global_mincut/viecut.h)
    (LP-loop heuristic for `n > 10000`).
  - [`lib/algorithms/flow/push_relabel.h`](../../VieCut/lib/algorithms/flow/push_relabel.h).
- JS port: [`vltanh.github.io/comdet/js/viecut/`](https://github.com/vltanh/vltanh.github.io/tree/main/comdet/js/viecut)
  (≈4020 LOC across 23 files; see `codebase_map.md` in memory for the
  full module table).
- Byte-equal across **2,309 cells / 19.3M cumulative per-step records /
  0 mismatches** (combined: legacy whole-graph 1750 cells +
  3-tier × Leiden-CPM(γ=0.5) 153 cells + 3-tier × Leiden-Mod 153 cells
  + 3-tier × SBM-Flat-PP 153 cells + n &gt; 10000 LP-LCC 150 cells).
  Verification target is L4 (self-RNG end-to-end, no oracle replay).
- Bumped 3-tier panel L4 (2026-05-11, T1+T2): legacy-whole-graph
  7400/7400 PASS (9.27M records); chained-from-leiden-mod 7400/7400
  PASS (2.82M records); chained-from-leiden-cpm(0.5) T1 5850/5850 PASS
  (4.03M records); chain-from-infomap and chain-from-sbm-flat-pp queued
  for resume. See `audit.md` for the full panel breakdown.

## Entrypoint

CLI (via `constrained-clustering`):

```
constrained_clustering MinCutCustom \
    --edgelist <path>.csv \
    --existing-clustering <path>.csv \
    --output-file com.csv \
    --log-file viecut.log \
    --mincut-type cactus \
    --num-processors <int>
```

`--mincut-type cactus` selects `cactus_mincut`; `--mincut-type noi`
selects the bare NOI capforest path (debug fallback, no cactus build).

C++ wiring:
[`constrained-clustering/src/mincut_custom.cpp:78-90`](../../constrained-clustering/src/mincut_custom.cpp#L78)
constructs the chosen mincut object and calls
`mc->perform_minimum_cut(G)`; the resulting partition is recovered via
`G->getNodeInCut(node_id)` per vertex.

Default configuration set at
[`mincut_custom.cpp:30-37`](../../constrained-clustering/src/mincut_custom.cpp#L30):
`save_cut=true`, `find_most_balanced_cut=true`, `set_node_in_cut=true`,
seed left at libstdc++'s `std::mt19937` default (`5489`) unless
`--random-seed=N` is passed.

JS entry: `COMDET.MINCUT.viecut(nodeIds, edges, opts)` at
[`mincut_adapter.js`](../../vltanh.github.io/comdet/js/viecut/mincut_adapter.js).
Returns `{ cutValue, inPartition, outPartition }`. Drop-in replacement
for `COMDET.MINCUT.stoerWagner` so the WCC and CM pages route
transparently.

## Pipeline

```text
cactus_mincut::perform_minimum_cut(G)
  └── findAllMincuts(G)
        ├── viecut::perform_minimum_cut(G)            // upper-bound heuristic
        │     n <= 10000  -> noi.perform_minimum_cut(G)
        │     n  > 10000  -> LP-loop (label-prop + PR12 + PR34, iter
        │                              until n <= 10000 or no shrink)
        ├── while n_after < 0.99 * n_before:          // outer-while
        │     ├── Phase A: noi.modified_capforest          [RNG: 1 draw]
        │     ├── Phase B: degree-1 cleanup                 [no RNG]
        │     ├── Phase C: prTests12 + prTests34            [no RNG]
        │     ├── Phase D: contraction::fromUnionFind       [no RNG]
        │     └── update mincut; clear guaranteed_edges if it tightened
        ├── if n > 1: mincut = min(mincut, noi(graphs.back()))
        ├── Phase E: recursive_cactus.flowMincut(graphs)    [RNG: 1+ draws]
        │     └── solves max-flow per (s,t); SCC + findSTCactus
        ├── minimum_cut_helpers::setVertexLocations
        └── Phase F + G: balanced_cut_dfs + most_balanced  [RNG: 1 draw]
              └── DFS the cactus; pick the most-balanced cut
```

Three RNG sites under the default config:

| Site | File:line | Draw |
|------|-----------|------|
| capforest starting vertex | [`noi_minimum_cut.h:133`](../../VieCut/lib/algorithms/global_mincut/noi_minimum_cut.h#L133) | `next() % n` |
| `recursive_cactus.problem_id` | [`recursive_cactus.h:54, 67`](../../VieCut/lib/algorithms/global_mincut/cactus/recursive_cactus.h#L54) | `next()` |
| `balanced_cut_dfs` start vertex | [`balanced_cut_dfs.h:41`](../../VieCut/lib/algorithms/global_mincut/cactus/balanced_cut_dfs.h#L41) | `nextInt(0, n-1)` |

Two additional RNG sites fire on the LP-loop path (only when
`n > 10000`):

| Site | File:line | Draw |
|------|-----------|------|
| `permutate_vector_local` per-chunk shuffle | [`tools/random_functions.h:86`](../../VieCut/lib/tools/random_functions.h#L86) | `std::shuffle` over 128-element chunks; `~chunk_size/2` raw `next()` draws per chunk |
| LP body equal-weight tie-break | [`coarsening/label_propagation.h:76-81`](../../VieCut/lib/coarsening/label_propagation.h#L76) | `next() % 2`, one draw per equal-weight tie |

## Floating-point environment

Integer arithmetic only on the cactus and NOI paths. `EdgeWeight`,
`mincut`, the `r_v[]` capforest accumulator, and the push-relabel
`FlowType` are all integer (`long long`). Three FP appearances
in the kernel are non-hazardous:

1. [`cactus_mincut.h:92`](../../VieCut/lib/algorithms/global_mincut/cactus/cactus_mincut.h#L92):
   `n * 1.01 < previous_size`. The `1.01` factor is exactly
   representable in IEEE-754; the comparison promotes both sides
   consistently in V8 and libstdc++.
2. [`recursive_cactus.h:208,212,218,236`](../../VieCut/lib/algorithms/global_mincut/cactus/recursive_cactus.h#L208):
   `static_cast<double>(blocksizes[c]) <= g_n / 2.0`. Both sides are
   integer-valued doubles below \(2^{53}\); safe.
3. [`balanced_cut_dfs.h:194`](../../VieCut/lib/algorithms/global_mincut/cactus/balanced_cut_dfs.h#L194):
   `in_weight * 2 >= totalWeight()`. Integer compare.

On the LP-loop path, one additional FP site:

4. [`contract_graph.h:89`](../../VieCut/lib/coarsening/contract_graph.h#L89)
   in `findTrivialCuts`: `reverse_mapping[p].size() < std::log2(n)`.
   V8 `Math.log2` is bit-equal libstdc++ `std::log2` for integer `n` in
   normal range (verified across `n ∈ {1083, 10000, 11204, 21363,
   22963}`); both libraries share the fdlibm derivation. Audit row D
   stays N/A for VieCut.

## RNG

`tools/random_functions.h` binds `std::mt19937` (libstdc++'s textbook
32-bit MT19937) and exposes a static `m_mt` per translation unit.
`setSeed(s)` runs `srand(s)` followed by `m_mt.seed(s)`; the Knuth-LCG
init recurrence is `mt[i] = 1812433253 * (mt[i-1] ^ (mt[i-1] >> 30)) + i`,
identical to the JS port at
[`random.js:34-47`](../../vltanh.github.io/comdet/js/viecut/random.js#L34).

`std::uniform_int_distribution<unsigned int>(lo, hi)` uses libstdc++'s
floor-rejection scaling
([`bits/uniform_int_dist.h`](https://gcc.gnu.org/onlinedocs/gcc-13.2.0/libstdc++/api/a01047_source.html)):
`scaling = floor(2^32 / r); past = r * scaling`; loop `r = urng(); if
(r >= past) reject; return lo + r / scaling`. JS mirrors via BigInt at
[`random.js uniformInt`](../../vltanh.github.io/comdet/js/viecut/random.js#L96)
(BigInt because `Math.floor(2^32 / r)` loses the low bit on doubles).

Default seed quirk: `static MersenneTwister m_mt;` default-constructs
with seed `5489` per the C++ standard. The
`constrained-clustering/src/mincut_custom.cpp:37` line that sets the
seed is commented out, so under the production CLI the seed defaults
to `5489` unless `--random-seed=N` is passed. The JS port mirrors this
at
[`random.js:121-122`](../../vltanh.github.io/comdet/js/viecut/random.js#L121)
(`m_mt` initialised with `5489`).

## Variant flags

Consumed via VieCut's `configuration::getConfig()`:

| flag | default | effect |
|------|---------|--------|
| `mincut_type` | `cactus` | cactus vs noi (debug fallback) |
| `find_most_balanced_cut` | true (under `mincut_custom.cpp`) | enables Phase F + G |
| `save_cut` | true (under `mincut_custom.cpp`) | enables `setVertexLocations` + `retrieveMinimumCut` |
| `set_node_in_cut` | true (under `mincut_custom.cpp`) | per-vertex `getNodeInCut(v)` retrieval |
| `edge_selection` | `heavy_vertex` | selects `maximumWeightedFlowEdge` (no RNG); `random` selects `findFlowEdge` (RNG) |
| `pq` | `default` | priority queue choice for NOI capforest |
| `disable_limiting` | false | controls capforest skip-union short-circuit |
| `seed` | 5489 default (libstdc++ MT) | passed through `setSeed` if `--random-seed=N` invoked |
| `find_lowest_conductance` | false | balanced-DFS optimises conductance instead of cut balance |

### `pq` variants

The `pq` flag selects the priority queue used inside the NOI capforest
sweep. Four config-string values map to three priority queue classes:

| Config string | C++ class | File |
|---------------|-----------|------|
| `bqueue` (FIFO inside bucket) | `fifo_node_bucket_pq` | [`fifo_node_bucket_pq.h`](../../VieCut/lib/data_structure/priority_queues/fifo_node_bucket_pq.h) |
| `bstack` (LIFO inside bucket) | `node_bucket_pq` | [`node_bucket_pq.h`](../../VieCut/lib/data_structure/priority_queues/node_bucket_pq.h) |
| `heap` | `vecMaxNodeHeap` | [`vecMaxNodeHeap.h`](../../VieCut/lib/data_structure/priority_queues/vecMaxNodeHeap.h) |
| `default` (auto-pick) | dispatch | [`noi_minimum_cut.h:102`](../../VieCut/lib/algorithms/global_mincut/noi_minimum_cut.h#L102) |

The `default` dispatcher picks `vecMaxNodeHeap` when
`mincut > 10000 && mincut > number_of_nodes`, otherwise
`fifo_node_bucket_pq`. Under `constrained-clustering`'s call shape the
`pq` config is never overridden, and the per-cluster mincut stays well
below 10000 on every observed input, so the realised priority queue is
always `fifo_node_bucket_pq`. The JS port loads `node_bucket_pq.js`
and `fifo_node_bucket_pq.js` but `vecMaxNodeHeap.js` is not ported;
the heap branch has never been triggered in stress testing. See
[`pq_variants.md`](https://github.com/vltanh/.claude/blob/main/projects/-home-vltanh-Documents-netsci-research/memory/community-detection/viecut/pq_variants.md)
in memory for the full breakdown.

`node_bucket_pq.js` itself stays loaded because it is also the
`m_Q` queue inside the push-relabel discharge loop
([`push_relabel.js:52`](../../vltanh.github.io/comdet/js/viecut/push_relabel.js#L52))
and the priority queue inside `findSTCactus`
([`recursive_cactus.js:248`](../../vltanh.github.io/comdet/js/viecut/recursive_cactus.js#L248)).
The upstream C++ uses `maxNodeHeap` for `m_Q`; the JS port substitutes
`NodeBucketPQ` because at the push-relabel size budget (distance
bounded by `2 * n`) the bucket queue exposes identical
deleteMax-by-key-then-LIFO-within-bucket order.

## Tie-break rules

Read directly off the canonical source; the JS port mirrors strict
vs non-strict at every site. The full list (33 sites, T1-T33) lives in
[`viecut/dossier.md`](https://github.com/vltanh/.claude/blob/main/projects/-home-vltanh-Documents-netsci-research/memory/community-detection/viecut/dossier.md);
selected high-risk ones:

| # | Site | Comparator | Tie semantics |
|---|------|------------|---------------|
| T2 | [`noi_minimum_cut.h:152`](../../VieCut/lib/algorithms/global_mincut/noi_minimum_cut.h#L152) | `r_v[t] + w >= mincut` | non-strict, exact-equal triggers union |
| T3 | [`cactus_mincut.h:92`](../../VieCut/lib/algorithms/global_mincut/cactus/cactus_mincut.h#L92) | `n * 1.01 < previous_size` | strict, exactly-1% shrink does not continue |
| T5 | [`recursive_cactus.h:164`](../../VieCut/lib/algorithms/global_mincut/cactus/recursive_cactus.h#L164) | `max_flow > mincut` | strict, equality falls into SCC-and-merge branch |
| T16 | [`push_relabel.h:148`](../../VieCut/lib/algorithms/flow/push_relabel.h#L148) | `sourceDistance <= m_distance[target]` | non-strict, equal-distance push BLOCKED |
| T20 | [`balanced_cut_dfs.h:88`](../../VieCut/lib/algorithms/global_mincut/cactus/balanced_cut_dfs.h#L88) | `lighterBlock > best_weight && weight == mincut` | strict, first-improving wins |
| T21 | [`balanced_cut_dfs.h:169`](../../VieCut/lib/algorithms/global_mincut/cactus/balanced_cut_dfs.h#L169) | `lighterBlock(in_weight) >= best_weight` | **non-strict**, later-encountered cycle position overrides earlier equal best |
| T30 | [`contract_graph.h:167`](../../VieCut/lib/coarsening/contract_graph.h#L167) | `cn * 2 > G->n() \|\| !copy` | non-strict `>`; selects contractGraphSparse vs contractGraphVertexset |

Iteration-order ties (`recursive_cactus.h:519, 531, 553, 565`) resolve
by id-ASC traversal of `G->nodes()`; the JS port mirrors with
`for (let n = 0; n < N; n++)`.

## Iteration order: the std::unordered_set / std::map swap

VieCut's source uses `std::unordered_set<NodeID>` and
`std::unordered_map<NodeID, ...>` at eight sites where iteration order
feeds back into algorithm output:

| # | Site | Container |
|---|------|-----------|
| 1 | `recursive_cactus.h:298` | `unordered_set<NodeID> all_ctr` |
| 2 | `contract_graph.h:187` | `unordered_set<NodeID> vtx_to_ctr` |
| 3 | `contract_graph.h:258` | `unordered_set<NodeID> edge_positions` |
| 4 | `mutable_graph.h:580` | `unordered_map<NodeID, tuple<…>>` |
| 5 | `mutable_graph.h:660` | `unordered_set<NodeID>` (signature) |
| 6 | `mutable_graph.h:682` | `unordered_map<NodeID, tuple<…>>` |
| 7 | `heavy_edges.h:32` | `unordered_map<NodeID, vector<NodeID>>` |
| 8 | `heavy_edges.h:69` | `unordered_set<NodeID>` |

Hash-bucket iteration order is library-defined and differs from
`Map`/`Set` insertion order in JS. The byte-equal verification path
defines a `container_swap.h` header (in the tracer's instrumented
fork) with a compile-time toggle:

```cpp
#ifdef TRACER_MODE
  template<class K> using TracerSet = std::set<K>;
  template<class K, class V> using TracerMap = std::map<K, V>;
#else
  template<class K> using TracerSet = std::unordered_set<K>;
  template<class K, class V> using TracerMap = std::unordered_map<K, V>;
#endif
```

The tracer build replaces the eight sites with `TracerSet/TracerMap`,
which gives id-ASC iteration. The JS port matches this order by
sorting positions before each `contractVertexSet` call
([`contract_graph.js:67`](../../vltanh.github.io/comdet/js/viecut/contract_graph.js#L67)
and
[`contract_graph.js:113`](../../vltanh.github.io/comdet/js/viecut/contract_graph.js#L113)).
Three claims, three statuses (per the layered-canonical flag):

1. **JS visualizer == canonical-tracer (TRACER_MODE swapped build)**:
   TRUE bit-for-bit across 2,309 cells.
2. **canonical-tracer (CANONICAL_MODE build) == unmodified canonical
   pipeline**: TRUE bit-for-bit (build-pair test on
   `fixture32` + `dnc`).
3. **JS visualizer == unmodified canonical pipeline**: FALSE under
   matching seed because of the std::unordered_set → std::set
   iteration-order swap. Algorithm trajectory is identical modulo
   that swap.

## LP-loop heuristic (`n > 10000`)

[`viecut.h:54-120`](../../VieCut/lib/algorithms/global_mincut/viecut.h#L54)
runs an outer iteration that contracts the graph via label-propagation
clusters before handing off to the cactus pipeline. The loop fires
only when `graphs.back()->number_of_nodes() > 10000` and the previous
iteration actually shrank the graph.

Per iteration:

1. `label_propagation::propagate_labels(G)` runs 2 LP sweeps. Per
   sweep, walk vertices in a chunk-shuffled order (each contiguous
   chunk of 128 positions shuffled in place by `std::shuffle`); for
   each vertex, sum incoming edge weight by neighbour-label, pick
   the heaviest, tie-break by one `next() % 2` draw on equal weight.
2. `minimum_cut_helpers::remap_cluster(G, cluster_id)` compactifies
   labels into `[0, k)` by encounter order over `G->nodes()`.
3. `findTrivialCuts(G, mapping, reverse_mapping, target_mindeg)` peels
   small blocks (block size `< log2(n)`) whose contracted node degree
   falls below the running mincut bound.
4. `contractGraph(G, mapping, reverse_mapping, copy=true)` pushes the
   super-graph onto `graphs`; `updateCut` tightens the running bound.
5. `prTests12(graphs.back(), cut, false)` + `fromUnionFind` +
   `updateCut`; same for `prTests34`.

The JS port mirrors at:

| Canonical | JS file:function |
|-----------|------------------|
| `random_functions::permutate_vector_local` | [`random.js:281 permutate_vector_local`](../../vltanh.github.io/comdet/js/viecut/random.js#L281) |
| libstdc++ `std::shuffle` optimised branch | [`random.js:256 shuffleRange`](../../vltanh.github.io/comdet/js/viecut/random.js#L256) |
| `std::uniform_int_distribution` (LP path) | [`random.js:242 uniformIntViaNext`](../../vltanh.github.io/comdet/js/viecut/random.js#L242) |
| `label_propagation::propagate_labels` | [`label_propagation.js:26`](../../vltanh.github.io/comdet/js/viecut/label_propagation.js#L26) |
| `minimum_cut_helpers::remap_cluster` | [`minimum_cut_helpers.js:99`](../../vltanh.github.io/comdet/js/viecut/minimum_cut_helpers.js#L99) |
| `contraction::findTrivialCuts` | [`contract_graph.js:161`](../../vltanh.github.io/comdet/js/viecut/contract_graph.js#L161) |
| `viecut::perform_minimum_cut` LP body | [`viecut_heuristic.js:44`](../../vltanh.github.io/comdet/js/viecut/viecut_heuristic.js#L44) |

`uniformIntViaNext` routes draws through the module-level `next()` so
the observer fires per draw (the legacy `uniformInt(rng, lo, hi)` is
kept for sites where the RNG is consumed without observation).

The instrumented C++ tracer's sibling `permutate_vector_local_traced`
([`tools/viz_check/viecut/instrumented/include/coarsening/label_propagation.h`](../../tools/viz_check/viecut/instrumented/include/coarsening/label_propagation.h))
hand-replicates the libstdc++ `std::shuffle` pairwise-swap sequence
but routes uint32 draws through `random_functions::next()` so each
draw is visible in the `[TRACE-RNG]` stream, matching the JS port's
observer output draw-for-draw.

L4 stress (50-seed × 3-LCC panel, n in {11204, 21363, 22963}): 150/150
cells, 12.78M cumulative per-step records, 0 mismatches. RNG draws per
cell range 24k..99k.

## Verification

### Stress matrix

Single harness at
[`tools/viz_check/viecut/stress_matrix.sh`](../../tools/viz_check/viecut/stress_matrix.sh)
+
[`stress_3tier.py`](../../tools/viz_check/viecut/stress_3tier.py). Per
cell:

1. Spawn `/tmp/viecut_traced_swapped <metis> <seed>`; the
   instrumented tracer (TRACER_MODE build) emits `[TRACE-RNG]` lines on
   stderr + cactus JSON on stdout.
2. Run JS kernel with observer installed under matching seed; observer
   collects a JS-side `[TRACE-RNG]` array.
3. `diff_harness.mjs` walks both streams lockstep, bit-compares every
   field (uint64 reinterpret for the rare double), stops at the first
   divergent record.

| Panel | Cells | PASS | Records |
|-------|------:|-----:|--------:|
| Legacy whole-graph (35 fixtures × 50 seeds) | 1750 | 1700 + 50 SKIP_NOCUT | 1,033,212 |
| 3-tier × Leiden-CPM(γ=0.5) per-cluster (17 × 9) | 153 | 153 | 3,569,487 |
| 3-tier × Leiden-Mod per-cluster (17 × 9) | 153 | 153 | 1,920,783 |
| 3-tier × SBM-Flat-PP per-cluster (17 × 9) | 153 | 153 | 0 (trivial-skip) |
| n &gt; 10000 LP-LCC (3 × 50) | 150 | 150 | 12,776,338 |
| **Total** | **2,359** | **2,309 PASS + 50 SKIP_NOCUT** | **19,299,820** |

The `≥ 1M cumulative records` floor (per the byte-equal tracer
playbook) is cleared by 19×. SKIP_NOCUT means the graph was already
disconnected after contraction (`mincut = 0`); both sides short-circuit
to the trivial path, no records emitted.

### Repro

Build the tracer:

```sh
cd community-detection/tools/viz_check/viecut/instrumented
./build.sh                              # produces /tmp/viecut_traced_swapped
```

Run a single cell:

```sh
node tools/viz_check/viecut/diff_harness.mjs \
     tests/cd_verify/fixture32_c-1_viecut.metis 7
# diff-harness: 1/1 PASS, 0 skipped (seed=7, total_records=6)
```

Run the legacy panel:

```sh
bash tools/viz_check/viecut/stress_matrix.sh
```

Run a 3-tier panel:

```sh
python3 tools/viz_check/viecut/stress_3tier.py \
    --partition leiden-cpm   # or leiden-mod / sbm-flat-pp
```

## Tracer probe coverage

The instrumented C++ tracer + JS observer pair emit per-step probe
tags across the audit-row map (A-N). All P0 sites enumerated in the
tracer-coverage gap audit are closed via canonical-side probes plus
matching JS-side observer probes; the probes are monotonically additive
per the "tracer prints stay" discipline. New sibling headers (under
[`tools/viz_check/viecut/instrumented/include/`](../../tools/viz_check/viecut/instrumented/include/))
extend `random_functions.h` with `permutate_vector_local_traced`, add
a `push_relabel.h` sibling with T16/T17/T18 probes, and add a
`label_propagation.h` sibling for LP-body tie-break probes. Equivalence
verified by build-pair on 9 fixtures × seed=42 plus JS observer-on vs
observer-off comparison on 5 fixtures (final state bit-equal in every
case).

## Cross-references in memory

- [`viecut/codebase_map.md`](https://github.com/vltanh/.claude/blob/main/projects/-home-vltanh-Documents-netsci-research/memory/community-detection/viecut/codebase_map.md):
  repo layout, call graph, RNG sites, variant flags.
- [`viecut/audit.md`](https://github.com/vltanh/.claude/blob/main/projects/-home-vltanh-Documents-netsci-research/memory/community-detection/viecut/audit.md):
  audit grid A-N, per-row finding, diagnostic ladder L0-L4 results,
  combined stress total, bumped 3-tier panel resume notes.
- [`viecut/dossier.md`](https://github.com/vltanh/.claude/blob/main/projects/-home-vltanh-Documents-netsci-research/memory/community-detection/viecut/dossier.md):
  per-step semantics, state schema, full tie-break decision-site list
  (T1-T33), variable-name mapping.
- [`viecut/pq_variants.md`](https://github.com/vltanh/.claude/blob/main/projects/-home-vltanh-Documents-netsci-research/memory/community-detection/viecut/pq_variants.md):
  the `pq` config-string vs class-name table.
- [`viecut/tracer_coverage_gaps.md`](https://github.com/vltanh/.claude/blob/main/projects/-home-vltanh-Documents-netsci-research/memory/community-detection/viecut/tracer_coverage_gaps.md):
  P0/P1/P2 probe-coverage audit with closure status per site.

## Where to look next

- [`cactus_mincut.js`](../../vltanh.github.io/comdet/js/viecut/cactus_mincut.js):
  top-level driver.
- [`recursive_cactus.js`](../../vltanh.github.io/comdet/js/viecut/recursive_cactus.js):
  `flowMincut` + `recursiveCactus` + `findSTCactus`.
- [`noi_minimum_cut.js`](../../vltanh.github.io/comdet/js/viecut/noi_minimum_cut.js):
  modified capforest (Phase A).
- [`balanced_cut_dfs.js`](../../vltanh.github.io/comdet/js/viecut/balanced_cut_dfs.js):
  most-balanced bipartition DFS (Phase F).
- [`mincut_adapter.js`](../../vltanh.github.io/comdet/js/viecut/mincut_adapter.js):
  the `COMDET.MINCUT.viecut(nodeIds, edges, opts)` wrapper consumed by
  the WCC and CM pages.
- [`VieCut/lib/algorithms/global_mincut/cactus/cactus_mincut.h`](../../VieCut/lib/algorithms/global_mincut/cactus/cactus_mincut.h):
  canonical top-level driver.

# Leiden technical reference

Companion to [`comdet/leiden-cpm.html`](https://vltanh.me/comdet/leiden-cpm.html)
and [`comdet/leiden-mod.html`](https://vltanh.me/comdet/leiden-mod.html).
The pages explain what the algorithm does in plain English; this file
holds the implementation detail and source-code pointers.

The two pages share one kernel. They differ only in which quality
function the inner loop maximises (constant Potts model on one side,
modularity on the other). Everything below is shared unless a section
is explicitly tagged "CPM only" or "Modularity only".

## Provenance

- Paper: Traag, Waltman, van Eck. "From Louvain to Leiden: guaranteeing
  well-connected communities." Scientific Reports 9, 5233 (2019).
  DOI: <https://doi.org/10.1038/s41598-019-41695-z>.
- Local PDF: `~/Downloads/Research/s41598-019-41695-z.pdf`.
- Python wrapper cloned at
  [`community-detection/leidenalg/`](../../leidenalg/) at tag `0.11.0`
  (post-tag at 20 commits past); forwards into the C++ body via
  pybind11.
- C++ algorithm body cloned at
  [`community-detection/libleidenalg/`](../../libleidenalg/) at tag
  `0.12.0`. Source files of interest:
  - `src/Optimiser.cpp`: outer driver, `move_nodes`,
    `merge_nodes_constrained`, the multi-level `do { ... } while
    (aggregate_further)` loop (1437 LOC).
  - `src/MutableVertexPartition.cpp`: community admin (`csize`,
    `cnodes`, `total_weight_in/from/to_comm`, `move_node`,
    `cache_neigh_communities`, `add_empty_community`,
    `rank_order_communities`).
  - `src/GraphHelper.cpp`: graph wrapper, `collapse_graph`,
    Fisher-Yates `shuffle`, the `orderCSize` comparator.
  - `src/CPMVertexPartition.cpp`: CPM `diff_move` + `quality`.
  - `src/ModularityVertexPartition.cpp`: modularity `diff_move` +
    `quality`.
- JS port:
  [`vltanh.github.io/comdet/js/leiden/leiden.js`](../../vltanh.github.io/comdet/js/leiden/leiden.js)
  for the Leiden-specific algebra (`LeidenPartition`, `CPM`,
  `LeidenMod`, `moveNodes`, `mergeNodesConstrained`,
  `optimisePartition`), plus
  [`vltanh.github.io/comdet/js/common/common.js`](../../vltanh.github.io/comdet/js/common/common.js)
  for `MT19937`, `Graph`, `shuffle`.

## Quality functions

### CPM (constant Potts model)

Paper §1, eq. 2:

\[
\mathcal{H}_{\text{CPM}} = \sum_c \big[ e_c - \gamma \binom{n_c}{2} \big]
\]

The sum runs over communities. \(e_c\) is the internal edge weight of
community \(c\), \(n_c\) is its node count, \(\gamma\) is the
resolution parameter. Each community is rewarded one point per internal
edge and pays a penalty proportional to the number of node pairs it
could host.

ΔH for a single-node move from `old_comm` to `new_comm`
(`CPMVertexPartition::diff_move`, `src/CPMVertexPartition.cpp:84-110`):

\[
\Delta\mathcal{H} =
  \big( w_{\text{to,new}} + w_{\text{from,new}} + s_w \big)
  - \big( w_{\text{to,old}} + w_{\text{from,old}} - s_w \big)
  - \gamma \cdot \big[ p_{\text{new}} - p_{\text{old}} \big]
\]

where

- \(w_{\text{to,c}}\), \(w_{\text{from,c}}\) are the weighted edges
  from \(v\) into community \(c\) (on undirected graphs the two are
  equal).
- \(s_w\) is \(v\)'s self-loop weight.
- \(p_{\text{new}} = n_v \cdot (2 c_{\text{new}} + n_v - s)\) and
  \(p_{\text{old}} = n_v \cdot (2 (c_{\text{old}} - n_v) + n_v - s)\)
  are the possible-edge deltas, with \(c_{\text{new}}\),
  \(c_{\text{old}}\) the destination and source community sizes,
  \(n_v\) the node-size of \(v\), and \(s\) the
  `correct_self_loops` flag (1 if on, 0 if off).

`CPMVertexPartition::diff_move` returns the raw `diff`. The
companion `CPMVertexPartition::quality` (`src/CPMVertexPartition.cpp:122-144`)
scales by `(2 - is_directed)`, so on undirected graphs the displayed
\(\mathcal{H}\) is twice the per-pair sum the formula above writes. The
walker on the page divides \(\Delta\mathcal{H}\) by 2 for undirected
inputs to match the displayed quantity to the same scale; the tracer
mirrors cpp byte-for-byte without that halving.

### Modularity

Paper §1, eq. 1 (Newman 2006):

\[
Q = \frac{1}{2m} \sum_{ij} \big[ A_{ij} - \frac{k_i k_j}{2m} \big] \delta(c_i, c_j)
\]

\(A_{ij}\) is the adjacency, \(k_i\) the weighted degree of node \(i\),
\(m\) the total edge weight. The sum runs over node pairs sharing a
community label.

ΔQ for a single-node move
(`ModularityVertexPartition::diff_move`, `src/ModularityVertexPartition.cpp:35-120`):

\[
\Delta Q = \frac{1}{m} \Big[ \big( w_{\text{to,new}} + s_w - \tfrac{k_{\text{out}} K_{\text{in,new}}}{W} \big) + \big( w_{\text{from,new}} + s_w - \tfrac{k_{\text{in}} K_{\text{out,new}}}{W} \big) - \big( w_{\text{to,old}} - \tfrac{k_{\text{out}} K_{\text{in,old}}}{W} \big) - \big( w_{\text{from,old}} - \tfrac{k_{\text{in}} K_{\text{out,old}}}{W} \big) \Big]
\]

with \(W = m \cdot (2 - \text{is\_directed})\), \(k_{\text{in/out}}\)
the in/out strength of \(v\) (sum over adjacent edge weights with
self-loops counted under `IGRAPH_LOOPS_TWICE`), and
\(K_{\text{in/out, c}}\) the per-community total in/out weight. cpp
`diff_move` returns the bracketed expression divided by \(m\); cpp
`quality` returns the same form normalised so the global maximum is
1.

## Algorithm

Three nested phases per outer iteration, looped until aggregation stops
shrinking the graph. The outer driver matches
`Optimiser::optimise_partition` at `src/Optimiser.cpp:77-369`.

### moveNodes: fast local move with queue

Pseudo-code (matches `COMDET.LEIDEN.moveNodes` in
[`leiden.js`](../../vltanh.github.io/comdet/js/leiden/leiden.js);
canonical at `src/Optimiser.cpp:490-749`):

```text
queue   = every node, shuffled (Fisher-Yates, backwards, igraph MT19937)
isStable[v] = false for every v

while queue non-empty:
  v = queue.popFront()
  cands = { neighbour-comms(v) (via cache_neigh_communities) } ∪ { membership(v) }
  if cnodes(membership(v)) > 1 and consider_empty_community:
    cands ∪= { get_empty_community() }       # may grow the partition admin
  evaluate ΔH for each c in cands
  pick c* = argmax ΔH; require ΔH > 10·DBL_EPSILON for the move
  isStable[v] = true
  if c* != membership(v):
    move v -> c*
    for each neighbour u of v (IGRAPH_ALL):
      if isStable[u] and membership(u) != c* and not fixed[u]:
        queue.pushBack(u); isStable[u] = false
```

The acceptance threshold is `10·DBL_EPSILON` rather than literal zero so
floating-point noise in the analytic `diff_move` formula cannot trigger
a no-op move (cpp `Optimiser.cpp:641`, JS `leiden.js:926`).

Differences from Louvain's `sweep`:

- Queue + restabilisation: only neighbours of moved nodes get
  revisited; Louvain re-walks every node every pass.
- Empty-community option: a node in a community of size > 1 may move
  into a fresh empty community if that raises ΔH; Louvain never
  considers empty communities.
- Acceptance threshold `10·DBL_EPSILON` (cpp `Optimiser.cpp:641`).
  Louvain accepts on `> 0` literal.

### mergeNodesConstrained: refinement

Pseudo-code (matches `COMDET.LEIDEN.mergeNodesConstrained` in
[`leiden.js`](../../vltanh.github.io/comdet/js/leiden/leiden.js);
canonical at `src/Optimiser.cpp:1230-1437`):

```text
refined-partition = singletons over collapsed graph
shuffle nodes via igraph MT19937 (Fisher-Yates backwards)

for each node v in order:
  if cnodes(refined-membership(v)) != 1:
    continue                                  # only singletons can merge
  cands = { refined-membership(u) : u ∈ adj(v) IGRAPH_ALL,
                                     constrained(u) == constrained(v) }
        ∪ { refined-membership(v) }
  evaluate ΔH for each c in cands
  pick c* = argmax ΔH; require ΔH >= 0 (TIES ACCEPTED)
  if c* != refined-membership(v):
    move v -> c*
```

Differences from `moveNodes`:

- Single forward pass, no queue, no restabilisation.
- Only acts on nodes that are still alone in their refined sub-community
  (`cnodes(v_comm) == 1` at `Optimiser.cpp:1275`).
- Constrained: candidate sub-communities must lie inside the same
  pre-refinement community (the `constrained_partition` argument).
- Acceptance is `ΔH >= 0` (cpp `Optimiser.cpp:1374`), unlike
  `moveNodes` which requires strict positive. Ties propagate the move
  forward; with no ties, sub-communities stay isolated.
- No empty-community option.

The randomised variant in the paper (paper §C, "Refining the
partition") picks candidate \(c\) with probability proportional to
\(\exp(\Delta\mathcal{H}_c / \theta)\) for randomness parameter
\(\theta > 0\). Greedy is recovered as \(\theta \to 0\). The
randomised variant carries Leiden's asymptotic guarantees (paper
Table 1, rows 5–6: uniform γ-density and subset optimality). The
greedy variant retains γ-connectivity (the main fix). libleidenalg
0.12.0 ships only the greedy variant; the page and the doc describe
that one.

### optimisePartition: outer driver

Pseudo-code (matches `COMDET.LEIDEN.optimisePartition`; canonical at
`src/Optimiser.cpp:77-369`):

```text
collapsedG = original graph
collapsedP = singleton partition over collapsedG

loop:
  prevVcount = collapsedG.vcount()

  # Phase 1: fast local move on collapsedG
  moveNodes(collapsedP, rng)
  collapsedP.renumber()                       # rank_order_communities
  fineMembership = project collapsedP down to original-node scope

  # Phase 2: refinement
  subCollapsedP = singleton partition over collapsedG
  mergeNodesConstrained(subCollapsedP,
                        constrained = collapsedP.membership)
  subCollapsedP.renumber()
  refinedP = subCollapsedP

  # Phase 3: aggregation
  newCollapsedG = collapsedG.collapse(refinedP.membership, refinedP.ncomm)
  for each refined sub-community xi:
    newCollapsedMembership[xi] = collapsedP.membership[u]
                                 # u = any node in xi (two-label trick)
  collapsedG = newCollapsedG
  collapsedP = Partition(collapsedG, newCollapsedMembership, qualityFn)

  if not (newCollapsedG.vcount < prevVcount
          and prevVcount > collapsedP.ncomm):
    break

return Partition over original graph from fineMembership; renumber
```

The two-label trick: `newCollapsedMembership[xi] =
collapsedP.membership[u]` where `u` is any node in refined
sub-community `xi`. If the refined partition split community 5 into
pieces `{5a, 5b, 5c}`, the next-level super-graph has three super-nodes
all carrying community label 5. The next `moveNodes` pass starts with
those three super-nodes inside one super-community. If they belong
together, no move is taken. If local geometry says split, the moves
separate them. This is what carries the γ-connectivity guarantee
through every level boundary.

The cpp loop is wrapped by `Optimiser::optimise_partition(partition,
n_iterations)` in
[`Optimiser.py:252`](../../leidenalg/src/leidenalg/Optimiser.py#L252)
which runs `n_iterations` of the `optimise_partition` call back-to-back,
feeding each iteration's output partition as the next iteration's
input. `find_partition` defaults `n_iterations=2` (Python wrapper at
`functions.py:21,91`).

## Partition admin (LeidenPartition vs LV.Partition)

The JS port ships two distinct `Partition` factories. Louvain's
`LV.Partition` (in `js/louvain/louvain.js`) implements the Louvain
externals algebra (gen-louvain v0.3 `Modularity::in/tot/gain`
convention: `in[c] = 2·intra_c + Σ self-loops`). Leiden's
`LeidenPartition` (in `js/leiden/leiden.js`) implements the
libleidenalg algebra (`_total_weight_in_comm[c] = intra_c`, with
per-edge folded mode loop and rank-order renumber). The two factories
match at level 0 on graphs without self-loops; they diverge at level 1
and above because the collapsed super-graph stores per-community intra
weight as super-node self-loops.

The kernel uses `LeidenPartition` throughout. Browser pages and tracer
harnesses read `COMDET.LEIDEN.Partition`, which resolves to
`LeidenPartition`.

## Defaults (libleidenalg 0.12.0)

`Optimiser` constructor (`src/Optimiser.cpp:16-28`):

| Setting | Value | Effect |
| --- | --- | --- |
| `consider_comms` | `ALL_NEIGH_COMMS` | `moveNodes` enumerates each visit's unique neighbour-community set; no extra RNG draws. |
| `optimise_routine` | `MOVE_NODES` | Outer loop calls `move_nodes`, not `merge_nodes`. |
| `refine_consider_comms` | `ALL_NEIGH_COMMS` | Refinement enumerates neighbour comms within constrained membership. |
| `refine_routine` | `MERGE_NODES` | Refinement = `merge_nodes_constrained` (singletons-only sweep). |
| `refine_partition` | `true` | Leiden refinement phase on. |
| `consider_empty_community` | `true` | Adds empty community as candidate every visit when `cnodes(v_comm) > 1`. |
| `max_comm_size` | `0` | No community-size cap. |

Default seed: `Optimiser::Optimiser()` calls
`igraph_rng_seed(&rng, time(NULL))` at `src/Optimiser.cpp:27`.
`Optimiser::set_rng_seed(N)` overrides. The leidenalg Python wrapper
exposes `seed=None` (default) which leaves the `time(NULL)` seed in
place; passing `seed=N` calls `set_rng_seed(N)`
([`functions.py:88-89`](../../leidenalg/src/leidenalg/functions.py#L88)).

## CLI flags

### leidenalg Python (CPM and Modularity)

`find_partition(graph, partition_type, **kwargs)` at
[`functions.py:21`](../../leidenalg/src/leidenalg/functions.py#L21):

| Argument | Type | Default | Effect |
| --- | --- | --- | --- |
| `partition_type` | class | required | `CPMVertexPartition` (CPM) or `ModularityVertexPartition` (Mod). |
| `initial_membership` | list of int | None | Starting partition. Default = singletons. |
| `weights` | list / edge attribute | None | Per-edge weights. |
| `n_iterations` | int | `2` | Number of times `optimise_partition` is called back-to-back. Negative ⇒ until an iteration finds no improvement. |
| `max_comm_size` | non-negative int | `0` | Cap on total node size per community. `0` = unbounded. |
| `seed` | int or None | None | RNG seed. `None` keeps the constructor-set `time(NULL)` seed. |
| `resolution_parameter` | float (CPM) | `1.0` | CPM \(\gamma\). Not used for `ModularityVertexPartition`. |

The same `find_partition_multiplex` (line 95) accepts a list of graphs
and a per-layer weight list; out of scope for this audit since the
page and the kernel cover the single-layer path only.

### `cd_verify` C++ tracer

The tracer binary at
[`community-detection/tools/viz_check/leiden/instrumented/`](../../tools/viz_check/leiden/instrumented/)
takes positional arguments:

```text
leiden_kernel_check <edge.csv> <out.csv> <quality> <resolution> <seed> <num_iter>
```

`<quality>` is `cpm` or `mod`. `<resolution>` is the CPM \(\gamma\)
(ignored for Modularity, since libleidenalg's
`ModularityVertexPartition` has no resolution parameter and the cpp
tracer instantiates it without one). `<num_iter>` defaults to 1 in the
audit (the verification stress matrix runs single-iter cells to keep
the tracer trace one outer iteration deep).

## Output format

Tracer JSON (one object per run) at
[`main_traced.cpp:102-152`](../../tools/viz_check/leiden/instrumented/main_traced.cpp#L102):

| Field | Shape | Meaning |
| --- | --- | --- |
| `Q_init` | double | Quality of the starting partition (singletons). |
| `Q_final` | double | Quality after the outer loop returns. |
| `n_communities` | int | Final community count post-renumber. |
| `membership[]` | int[N] | Final per-node community id, after `rank_order_communities`. |
| `renum_to_orig[]` | string[N] | Map node-id back to source CSV id. |
| `passes[]` | array | One element per move or refine pass at any level. |

Each `passes[k]` carries `{pass, phase ("move"|"refine"), level,
queue, shuffled_nodes[], moves[{v, from, to, dQ, moved}],
nb_moves, total_improv, post_membership[], pre_membership[]}`. All
doubles emit at `std::setprecision(17)`.

JS `optimisePartition(graph, qfn, seed, opts)` returns:

```text
{
  partition: Partition,        // final partition over the original graph
  quality: number,             // H (CPM) or Q (Modularity) after renumber
  levels: [{
    level: 0,1,2,...,
    collapsedVcount: N,
    moveTraces: [{ v, fromComm, toComm, moved, delta, candidates }],
    moveImprov: number,
    moveCount: number,
    refineTraces: [...],
    refineImprov: number,
    refineCount: number,
    finePostMove: Int32Array,         // membership over original graph after move
    finePostRefine: Int32Array,       // membership over original graph after refine
    newCollapsedVcount: number,
  }, ...]
}
```

The page walkers read `levels[0].moveTraces` and
`levels[0].refineTraces` as the per-visit timeline; the membership
snapshot at any point is approximated by `finePostMove` (during move
events) or `finePostRefine` (during refine events). For the 32-node
fixture this is exact at the pass boundary. For per-event accuracy on
a larger graph the kernel would need to record a membership diff after
every move; the trade-off is trace size.

## Reproducibility

Three knobs control byte-equality between the JS walker and
libleidenalg under matching seed:

| Knob | Default | Effect |
| --- | --- | --- |
| `seed` (both pages) | 42 | Initial state of `MT19937`. Same seed feeds the per-pass shuffle and (in the randomised refinement variant, not exposed) the candidate pick. |
| `gamma` (CPM page) | 0.05 | CPM resolution; slider scans `[10^-3, 10^0]`. |
| Number of outer iterations | 1 | The JS kernel runs `optimisePartition` once. Python wrapper defaults to `n_iterations=2`; see "Paper vs port divergences" below. |

The JS `MT19937` factory mirrors igraph's textbook Matsumoto-Nishimura
implementation byte-for-byte. The single quirk: igraph's `mt19937`
remaps a `seed=0` request to `seed=4357`
([`rng_mt19937.c:92-94`](../../constrained-clustering/external_libs/igraph/src/random/rng_mt19937.c#L92)).
The JS port consumes the seed as supplied. The stress matrix avoids
`seed=0` for this reason; passing `seed=0` to the JS walker produces a
valid trace that does not match a cpp `seed=0` trace.

## Paper vs port divergences

| Aspect | Paper | libleidenalg 0.12.0 | JS production walker |
| --- | --- | --- | --- |
| Refinement candidate pick | Random, `P(c) ∝ exp(ΔH/θ)` with `θ > 0` | Greedy `argmax ΔH` only | Greedy `argmax ΔH` only |
| Refinement acceptance | `ΔH ≥ 0` either variant | `ΔH ≥ 0` | `ΔH ≥ 0` |
| Number of iterations | "Run until stable" (Algorithm 1) | One call to `optimise_partition` per invocation; Python wrapper defaults to `n_iterations=2` | One call per `optimisePartition` (page default: one outer iteration) |
| Asymptotic guarantees (uniform γ-density, subset optimality, paper Table 1 rows 5–6) | Hold with the randomised variant | Out of reach with greedy refinement | Out of reach with greedy refinement |
| γ-connectivity guarantee (Table 1 row 2) | Holds for any iteration | Holds | Holds |
| γ-separation guarantee (Table 1 row 1) | Holds for any iteration | Holds | Holds |
| Per-node optimality at stable iteration (row 3) | Holds | Holds when `n_iterations` is large enough to reach the fixed point | Single-iteration walker does not guarantee a stable iteration on every fixture |

The one operational gap relevant to this gallery: the page walker
shows the output of one outer iteration of Leiden. The Python
wrapper's `find_partition` default would run two. On the 32-node
fixture this rarely changes anything (Leiden's first iteration on a
small graph hits its own fixed point), but on larger inputs the
second iteration occasionally peels off extra connectivity-driven
splits.

## JS-port divergences from cpp

| Site | cpp | JS production walker | Status |
| --- | --- | --- | --- |
| `seed=0` handling | Remaps to `4357` (`rng_mt19937.c:92-94`) | Consumed as supplied | Stress matrix avoids `seed=0`. |
| `CPM.diff_move` return scaling | Returns raw `diff` (per-pair scale); `quality()` multiplies by `(2 - directed)` | Returns `diff / 2` on undirected so the visualised \(\Delta\mathcal{H}\) matches the displayed \(\mathcal{H}\) | Walker-display only; tracer `canonCPM` mirrors cpp byte-for-byte. |
| `Modularity.diff_move` return scaling | Returns `diff / m` (already normalised) | Returns `diff / m` (mirrors cpp) | Byte-equal. |
| Adjacency iteration order | `igraph_lazy_adjlist` in neighbour-id ASC | `Graph` constructor with `sortAdj: true` sorts adj by neighbour-id ASC + normalises undirected edges to `(max, min)` | Byte-equal at every level. |
| Self-loop handling in `move_node` | `IGRAPH_OUT` + `IGRAPH_IN` mode loop; each mode hits self-loop twice under `IGRAPH_LOOPS_TWICE`; `int_weight = w/(2-directed)/(u==v?2:1)` | One adj walk per mode; per-edge contribution doubled when `u===v` to mirror cpp's twice-per-mode iteration | Byte-equal. |
| `collapse_graph` self-loop weight | Halved per appearance (cpp sees self-loop twice per vertex walk under `LOOPS_TWICE`) | Stored once at full weight, no halving (JS adj has self-loop once) | Net contribution `w` on both sides. |
| `merge_nodes_constrained` candidate set construction | Walks `IGRAPH_ALL` adj, adds `membership[u]` whenever `constrained[u] == constrained[v]`; self-loop encounter inserts `v_comm` at adj-iteration position | Same enumeration, includes self-loop position, no skip on `u === v` | Byte-equal; tie-break order under `>=` matches cpp. |
| `renumber_communities` order | `csize` DESC, `cnodes` DESC, original-id ASC (`orderCSize`, `GraphHelper.cpp:16-28`) | Same comparator in `LeidenPartition.renumber` | Byte-equal. |
| `n_iterations` | Python wrapper defaults to 2 outer iterations | JS walker runs 1 outer iteration | Documented as paper-vs-port gap above. |

## Audit grid summary

The most recent end-to-end verification (memory file
`memory/community-detection/leiden/audit.md`) records L4 self-RNG
byte-equality of the JS walker vs the libleidenalg tracer at every
collapse level:

| Sweep | Cells | Cumulative moves | Result |
| --- | --- | --- | --- |
| 50 seeds × 21 fixtures × 3 quality | 3,150 | 22.7 M | PASS, 0 mismatches |
| Plus inner-level admin per-visit | 3,313 | 35.5 M | PASS, 0 mismatches |
| 3-tier panel × 9 seeds × {Mod, CPM(0.05)} re-verify | 306 | 7.9 M visits / 3.9 M moves | PASS, 0 per-visit + per-pass-scalar mismatches |
| Bumped 3-tier T1+T2 panel (CPM(0.5) + Modularity) | 14,800 | 51.1 M visits | PASS |

A re-spot-check on `dnc` (n = 906) at the time of this writing was
not re-run; the stress-matrix harness lives at
[`tools/viz_check/leiden/stress_matrix.sh`](../../tools/viz_check/leiden/stress_matrix.sh)
and the 3-tier wrapper at
[`tools/viz_check/leiden/stress_3tier.py`](../../tools/viz_check/leiden/stress_3tier.py).

## Behaviour on the comdet 32-node fixture

Leiden-CPM, default `γ = 0.05`, `seed = 42`:

- Outer iterations executed by the page kernel: 1.
- Move-phase visits at level 0: 40.
- Refine-phase visits at level 0: 21.
- Final partition: 8 communities (matches the page's stage-5 panel).
- Final \(\mathcal{H}\) (display scale, undirected halved): 36.69.

Leiden-Mod, `seed = 42`:

- Outer iterations executed: 1.
- Move-phase visits at level 0: depends on the queue trajectory; the
  page panel shows the actual count.
- Final partition: typically smaller than CPM(0.05) because modularity
  rewards merging communities whose total degree falls below
  \(\sqrt{2m} \approx 10\).
- Final \(Q\) in the page's normalisation: between 0.6 and 0.7
  depending on the specific run trajectory; the page renders the
  realised value live.

## Known issues / drift

- The `linksRow` helper in
  [`shared.js`](../../vltanh.github.io/comdet/shared.js) constructs the
  page's "notes" button URL as
  `docs/algorithms/${gen}.md` where `gen` is `leiden-cpm` or
  `leiden-mod`. Both pages link to `leiden-cpm.md` and `leiden-mod.md`
  rather than the single `leiden.md` that actually exists. The
  inline `tech-companion` aside on both pages points at `leiden.md`
  directly and works; the top "notes" pill links 404. A symlink at
  `docs/algorithms/leiden-cpm.md → leiden.md` and
  `docs/algorithms/leiden-mod.md → leiden.md` would close the gap
  without changing the kernel.
- The audit memory file references absolute paths under
  `/home/vltanh/Documents/web/vltanh.github.io/`. The active worktree
  on this host serves the JS port from
  `community-detection/vltanh.github.io/` as a submodule. The audit
  grid still holds for the worktree code, since the submodule is
  pinned to the same commit referenced in the memory.

## Cross-references

- The page walker's per-cluster stats use `COMDET.PAGE.computeClusterStats`
  in
  [`page_helpers.js`](../../vltanh.github.io/comdet/js/comdet/page_helpers.js).
- The Fig. 2 disconnected-community demo on the Leiden-CPM page is
  hand-written in
  [`page_cpm.js`](../../vltanh.github.io/comdet/js/leiden/page_cpm.js)
  (`mountFig2`); it does not call into the kernel.
- For the contrast with Louvain on the same fixture, see
  [`louvain.md`](./louvain.md).

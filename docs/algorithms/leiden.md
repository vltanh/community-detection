# Leiden — technical reference

Companion to [`comdet/leiden-cpm.html`](https://vltanh.me/comdet/leiden-cpm.html)
and [`comdet/leiden-mod.html`](https://vltanh.me/comdet/leiden-mod.html).
The pages explain what the algorithm does in plain English; this file
holds the implementation detail and source-code pointers.

## Provenance

- Paper: Traag, Waltman, van Eck. "From Louvain to Leiden: guaranteeing well-connected communities." Scientific Reports 9, 5233 (2019).
  DOI: <https://doi.org/10.1038/s41598-019-41695-z>
- Local PDF: `~/Downloads/Research/s41598-019-41695-z.pdf`
- Canonical Python wrapper cloned at:
  [`community-detection/leidenalg/`](../../leidenalg/)
  Tag `0.11.0` (post-tag at 20 commits past); the Python layer
  forwards into the C++ algorithm body.
- Canonical C++ algorithm body cloned at:
  [`community-detection/libleidenalg/`](../../libleidenalg/) tag `0.12.0`.
  Source files of interest:
  - `src/Optimiser.cpp` — outer driver + move + merge + refinement (1437 LOC)
  - `src/MutableVertexPartition.cpp` — community admin (csize, weights, neighbour cache, move\_node)
  - `src/GraphHelper.cpp` — graph wrapper, `collapse_graph`, Fisher-Yates shuffle
  - `src/CPMVertexPartition.cpp` — CPM diff\_move + quality
  - `src/ModularityVertexPartition.cpp` — modularity diff\_move + quality
- Detailed source-map audit: [`memory/audit_libleidenalg.md`](../../../netsci-research/.claude/projects/-home-vltanh-Documents-netsci-research/memory/audit_libleidenalg.md)
- JS port: [`vltanh.github.io/comdet/js/leiden/leiden.js`](../../vltanh.github.io/comdet/js/leiden/leiden.js)
  + shared substrate at [`js/louvain/louvain.js`](../../vltanh.github.io/comdet/js/louvain/louvain.js).

## Quality functions

### CPM (constant Potts model)

Paper §1, eq. 2:

\[
\mathcal{H}_{CPM} = \sum_c \big[ e_c - \gamma \binom{n_c}{2} \big]
\]

ΔH for a single-node move (libleidenalg `CPMVertexPartition::diff_move`,
`src/CPMVertexPartition.cpp:84-110`):

\[
\Delta\mathcal{H} =
  \big( w_{\text{to,new}} + w_{\text{from,new}} + sw \big)
  - \big( w_{\text{to,old}} + w_{\text{from,old}} + sw \big)
  - \gamma \cdot n_v \cdot \big[ (2c_{\text{new}} + n_v - s) - (2(c_{\text{old}} - n_v) + n_v - s) \big]
\]

where \(sw\) is the node's self-weight, \(n_v\) its node-size,
\(c_{\text{new/old}}\) the destination/source community sizes,
\(s\) the self-loop correction (1 if `correct_self_loops`, else 0).
On undirected graphs the result is halved (canonical sums each edge
twice via `w_to + w_from`, then divides at the end).

### Modularity

Paper §1, eq. 1 (same as Louvain): see
[`docs/algorithms/louvain.md`](./louvain.md). The JS implementation is
shared with Louvain via `COMDET.LOUVAIN.Modularity`; Leiden's outer
driver picks it up through `COMDET.LEIDEN.Modularity` (a re-export).

## Algorithm

Three nested phases per outer iteration, looped until aggregation
stops shrinking the graph.

### moveNodes — fast local move with queue

Pseudo-code (matches `COMDET.LEIDEN.moveNodes`,
[`leiden.js`](../../vltanh.github.io/comdet/js/leiden/leiden.js); canonical
at `src/Optimiser.cpp:490-749`):

```text
queue: every node, shuffled
isStable: every node initially false

while queue non-empty:
  v = queue.popFront()
  cands = neighbour-comms(v) ∪ {membership(v)}
  if cnodes(membership(v)) > 1: cands += emptyComm()
  evaluate ΔH for each c
  pick c* = argmax ΔH; require ΔH > 10·ε for the move
  isStable[v] = true
  if c* != membership(v):
    move v -> c*
    for each neighbour u of v:
      if isStable[u] and membership(u) != c*:
        queue.pushBack(u); isStable[u] = false
```

Differences from Louvain's `sweep`:
- Queue + restabilisation: only neighbours of moved nodes get revisited.
- "Consider empty community" option: a node in a community of size > 1
  may move into a fresh empty community if that raises ΔH.
- Strict-positive acceptance threshold of `10·ε` (Optimiser.cpp:643);
  on graphs of normal scale this matches Louvain's `ΔQ > 0` rule.

### mergeNodesConstrained — refinement

Pseudo-code (matches `COMDET.LEIDEN.mergeNodesConstrained`; canonical
at `src/Optimiser.cpp:1230-1437`):

```text
init refined-partition = singletons over collapsed graph
shuffle nodes
for each node v in order:
  if cnodes(refined-membership(v)) != 1: continue   # only singletons
  cands = { refined-membership(u) : u neighbour of v
                                  AND constrained(u) == constrained(v) }
        ∪ { refined-membership(v) }
  evaluate ΔH for each c
  pick c* = argmax ΔH; require ΔH >= 0 (TIES ACCEPTED)
  if c* != refined-membership(v): move v -> c*
```

Differences from `moveNodes`:
- Single forward pass, no queue, no restabilisation.
- Only acts on nodes that are still alone in their refined sub-community.
- Constrained: candidate sub-communities must lie inside the same
  pre-refinement community (the `constrained` mask passed in).
- Acceptance is `ΔH >= 0` (strict equality accepted), unlike `moveNodes`
  which requires `ΔH > 10·ε`.
- No empty-community option.

The paper's canonical recipe is the *randomised* variant: pick candidate
\(c\) with probability proportional to \(\exp(\Delta\mathcal{H}_c / \theta)\)
for randomness parameter \(\theta > 0\). Greedy is recovered as
\(\theta \to 0\). The randomised variant is what gives Leiden its
asymptotic guarantees (paper Table 1, rows 5–6: uniform γ-density +
subset optimality). The greedy variant retains γ-connectivity (the main
fix).

### optimisePartition — outer driver

Pseudo-code (matches `COMDET.LEIDEN.optimisePartition`; canonical at
`src/Optimiser.cpp:77-369`):

```text
collapsed = original graph
collapsedP = singleton partition

loop:
  prevVcount = collapsed.vcount()

  # Phase 1: fast local move on collapsed
  moveNodes(collapsedP, rng)
  fineMembership = project collapsedP down to original

  # Phase 2: refinement
  subCollapsedP = singleton partition over collapsed
  mergeNodesConstrained(subCollapsedP, constrained = collapsedP.membership)
  refinedP = subCollapsedP
  refinedP.renumber()

  # Phase 3: aggregation
  newCollapsed = collapsed.collapse(refinedP.membership, refinedP.ncomm)
  newCollapsedMembership = for each refined sub-comm xi,
                            inherit collapsedP.membership of any constituent node
                            (this is the two-label trick; see notes below)
  collapsedG = newCollapsed
  collapsedP = Partition(newCollapsed, newCollapsedMembership, qualityFn)

  if newCollapsed.vcount >= prevVcount: break
  if prevVcount <= collapsedP.ncomm: break

return Partition over original graph from fineMembership; renumber
```

The two-label trick: `newCollapsedMembership[xi] = collapsedP.membership[u]`
where `u` is any node in refined sub-community `xi`. So if the refined
partition split community 5 into pieces `{5a, 5b, 5c}`, the next-level
super-graph has three super-nodes all carrying community label 5. The
next `moveNodes` pass starts with those three super-nodes inside one
super-community; if they belong together, no move is taken; if local
geometry says split, the moves separate them. This is what carries the
γ-connectivity guarantee through the level boundary.

## Defaults (libleidenalg 0.12.0)

| Setting | Value | Source |
|---|---|---|
| `consider_comms` | `ALL_NEIGH_COMMS` (=2) | `Optimiser.cpp:18` |
| `optimise_routine` | `MOVE_NODES` (=10) | `Optimiser.cpp:19` |
| `refine_consider_comms` | `ALL_NEIGH_COMMS` | `Optimiser.cpp:20` |
| `refine_routine` | `MERGE_NODES` (=11) | `Optimiser.cpp:21` |
| `refine_partition` | `true` | `Optimiser.cpp:22` |
| `consider_empty_community` | `true` | `Optimiser.cpp:23` |
| `max_comm_size` | 0 (unlimited) | `Optimiser.cpp:24` |

Default seed in canonical Python: `time(NULL)` set in the constructor
(`Optimiser.cpp:27`); overridden by `set_rng_seed(N)` if the user passes
`seed=N` to `find_partition`. The JS port forces an explicit seed (no
`time(NULL)` fallback) and defaults to 42 in the page glue.

## Reproducibility

| Knob | Default | Effect |
|---|---|---|
| `seed` (CPM and Mod pages) | 42 | Initial state of `MT19937`; controls per-pass shuffle + (in randomised refinement, but currently greedy) the candidate pick. |
| `gamma` (CPM page only) | 0.05 | CPM resolution. Slider replays from log scale `[10^-3, 10^0]`. |
| Number of outer iterations | 1 (default) | Could be increased; the paper notes that iterating Leiden with the previous output as input keeps strictly improving the partition until a stable iteration. |

## Output shape

`COMDET.LEIDEN.optimisePartition(graph, qfn, seed, opts)` returns:

```text
{
  partition: Partition,        // final partition over the original graph
  quality: number,             // H (CPM) or Q (Modularity) of the final partition
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

Page walkers stitch `moveTraces` and `refineTraces` into a flat
timeline; the membership snapshot at any point is approximated by the
finePostMove (during move events) or finePostRefine (during refine
events). For a 32-node fixture this approximation is fine; for a more
exact per-event snapshot the kernel would need to record the membership
diff after every single move.

## Behaviour on the comdet 32-node fixture (Leiden CPM at γ=0.05, seed=42)

- Number of outer iterations: 1 (level 0 only).
- Number of move-phase visits at level 0: 40 (with one or more re-pushes).
- Number of refine-phase visits at level 0: 21 (singletons in original communities).
- Final partition: 8 communities.
- Final \(\mathcal{H}\): 36.69.

## Cross-references

- The page walker's per-cluster stats use `COMDET.PAGE.computeClusterStats`
  in [`page_helpers.js`](../../vltanh.github.io/comdet/js/comdet/page_helpers.js).
- The Fig. 2 disconnected-community demo on the Leiden CPM page is
  hand-written in [`page_cpm.js`](../../vltanh.github.io/comdet/js/leiden/page_cpm.js)
  (`mountFig2`); it does not call into the kernel.

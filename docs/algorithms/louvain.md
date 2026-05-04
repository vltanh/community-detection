# Louvain — technical reference

Companion to [`comdet/louvain.html`](https://vltanh.me/comdet/louvain.html).
The page explains what the algorithm does in plain English; this file
holds the implementation detail and source-code pointers.

## Provenance

- Paper: Blondel, Guillaume, Lambiotte, Lefebvre. "Fast unfolding of communities in large networks." J. Stat. Mech. (2008) P10008.
  DOI: <https://doi.org/10.1088/1742-5468/2008/10/P10008>
- Local PDF: `~/Downloads/Research/Blondel_2008_J._Stat._Mech._2008_P10008.pdf`
- Canonical C++ implementation cloned at:
  [`community-detection/louvain-generic/gen-louvain/`](../../louvain-generic/gen-louvain/)
  Campigotto-Conde Céspedes-Guillaume v0.3, July 2013 (multi-criteria
  fork over the original 2008 reference). Source files of interest:
  - `src/louvain.cpp` — driver (Phase 1 + Phase 2 loop)
  - `src/graph_binary.cpp`, `src/graph.cpp` — graph storage + collapse
  - `src/<criterion>.cpp` — quality-function plug-ins (`condora.cpp`,
    `devind.cpp`, `devuni.cpp`, `dp.cpp`, `goldberg.cpp`, `balmod.cpp`)
  - For straight modularity (the Newman 2006 form used by the comdet page),
    the relevant quality is the v0.3 default; the multi-criteria options are
    out of scope.
- JS port: [`vltanh.github.io/comdet/js/louvain/louvain.js`](../../vltanh.github.io/comdet/js/louvain/louvain.js).

## Quality function

Newman 2006 modularity, exactly as the paper writes it (§1, eq. 1):

\[
Q = \frac{1}{2m} \sum_{ij} \big[ A_{ij} - \frac{k_i k_j}{2m} \big] \delta(c_i, c_j)
\]

ΔQ closed form (paper §2, eq. 2), used at every node-visit:

\[
\Delta Q =
  \big[\frac{\Sigma_{in} + 2k_{i,in}}{2m} - (\frac{\Sigma_{tot} + k_i}{2m})^2\big]
  -
  \big[\frac{\Sigma_{in}}{2m} - (\frac{\Sigma_{tot}}{2m})^2 - (\frac{k_i}{2m})^2\big]
\]

JS implementation: `COMDET.LOUVAIN.Modularity()` in
[`louvain.js`](../../vltanh.github.io/comdet/js/louvain/louvain.js)
(`Modularity.diffMove`, `Modularity.quality`). The implementation uses
`P.totalWeightFromComm(c)` for \(\Sigma_{tot}\) (sum of weights incident
to nodes in \(C\)) and `P.weightToComm(v, c)` for \(k_{i,in}\) (sum of
weights from \(i\) to nodes already in \(C\)).

## Algorithm

Two phases per pass, iterated until modularity stops increasing.

### Phase 1 — modularity sweep

Pseudo-code (matches `COMDET.LOUVAIN.sweep` + `COMDET.LOUVAIN.phase1`):

```text
init: every node in its own community
loop:
  shuffle nodes via MT19937
  for each node v in order:
    cands = { membership(u) : u neighbour of v } ∪ { membership(v) }
    compute ΔQ(v → c) for each c in cands
    pick c* = argmax ΔQ; if max ΔQ <= 0, leave v alone
    if c* != membership(v): move v; nbMoves += 1
  if nbMoves == 0: break
```

Differences from Leiden's `moveNodes`:
- No FIFO queue. Every node is revisited every sweep until the whole
  graph quiets down. Leiden's `moveNodes` revisits only neighbours of
  moved nodes, so cost grows with moves performed rather than nodes
  times passes.
- No "consider empty community" option. Louvain only considers
  neighbours' communities + the node's own.
- Strict positive acceptance (`ΔQ > 0`). Same as Leiden's `moveNodes`
  in spirit (Leiden uses `> 10·ε`, equivalent on graphs that are not
  pathologically scaled).

### Phase 2 — aggregation

Pseudo-code (matches `COMDET.LOUVAIN.run`):

```text
super-graph: one node per community in Phase 1 output
super-edges: weight = sum of original edge weights between communities
intra-community edges: become self-loops on the super-node
re-run Phase 1 on super-graph
```

JS implementation: `Graph.collapse(membership, ncomm)` in
[`louvain.js`](../../vltanh.github.io/comdet/js/louvain/louvain.js).
The collapse halves self-loop weights on undirected graphs, matching
the `igraph_create` storage convention used by libleidenalg
(`src/GraphHelper.cpp:739`).

### Outer loop

```text
collapsed = original
loop:
  P = phase1(collapsed)
  newCollapsed = collapsed.collapse(P.membership, P.ncomm)
  if newCollapsed.vcount >= collapsed.vcount: break
  if newCollapsed.vcount <= 1: break
  collapsed = newCollapsed
return P projected back onto original (via aggregateMap)
```

The early exit at `vcount <= 1` catches the modularity-resolution-limit
trap on small graphs where the algorithm wants to keep merging until
everything is one community.

## Reproducibility

| Knob | Default | Effect |
|---|---|---|
| `seed` | 42 | Initial state of `MT19937`; controls the per-sweep shuffle order. |
| Number of sweeps per Phase 1 | unbounded (up to 50 in the JS guard) | Page reports actual sweep count per level. |
| Maximum levels | unbounded (up to 30 in the JS guard) | Page reports actual level count. |

## Output shape

`COMDET.LOUVAIN.run(graph, qfn, seed, opts)` returns:

```text
{
  partition: Partition,        // final partition over original graph
  quality: number,             // Q at the deepest level
  levels: [{
    level: 0,1,2,...,
    sweeps: [
      { nbMoves, totalImprov, traces: [{ v, fromComm, toComm, moved, delta, candidates }] }
    ],
    collapsedVcountBefore: N,
    collapsedNcomm: K,
    finePost: Int32Array,        // membership over original graph at this level
    newCollapsedVcount: K' = collapsedG.collapse(...).vcount()
  }, ...]
}
```

The page walker steps through `levels[0].sweeps[*].traces[*]` linearly;
the level table aggregates by level.

## Behaviour on the comdet 32-node fixture

| Level | vc-before | ncomm found | vc-after | sweeps to quiet | total moves | ΔQ |
|---|---|---|---|---|---|---|
| 0 | 32 | 8 | 8 | 5 | 32 | +0.616 |
| 1 | 8 | 4 | 4 | 2 | 4 | +0.047 |
| 2 | 4 | 3 | 3 | 2 | 1 | +0.006 |
| 3 | 3 | 1 | 1 | 2 | 2 | +0.026 |

Final Q at the deepest level: 0.750. The page reports the deepest-level
output, which collapses to a single community on this fixture (textbook
modularity-resolution-limit behaviour). Level-0 is the more meaningful
community structure but Louvain doesn't expose a "stop at level k" knob.

## Internally disconnected communities

Louvain's connectivity behaviour matches the paper: a community can
become internally disconnected when a bridge node moves out (paper Fig. 2
of Traag et al. 2019, recreated on the [Leiden page](../vltanh.github.io/comdet/leiden-cpm.html)).
The Louvain page calls `disconnectedComms(membership)` on the final
partition to detect any such case; on the 32-node fixture none survive
(the deepest level is one community = trivially connected). On larger
real-world networks the rate runs 1–17% per Traag et al. 2019.

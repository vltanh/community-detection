# Infomap

[← back to index](../algorithms.md)

Map equation on random walks. Rosvall + Bergstrom 2008
([doi:10.1073/pnas.0706851105](https://doi.org/10.1073/pnas.0706851105),
PNAS 105(4):1118-1123). Reference implementation: the
[mapequation/infomap](https://github.com/mapequation/infomap) C++
binary, vendored into this repo at [`infomap/`](../../infomap/) (current
HEAD: see the submodule pointer in `.gitmodules`).

The page at `vltanh.github.io/comdet/infomap.html` walks the algorithm
on the 32-node fixture; this file is the technical companion.

## Entrypoint

The Python wrapper at
[`src/infomap/run_infomap.py`](../../src/infomap/run_infomap.py) is a
thin subprocess shell around the
[`infomap/`](../../infomap/) C++ binary. The interesting code lives in
the binary's source under `infomap/src/`.

For comdet-page purposes the relevant entrypoints are:

- `InfomapBase::run()` at
  [`infomap/src/core/InfomapBase.cpp`](../../infomap/src/core/InfomapBase.cpp).
  Top-level driver; calls flow calculation, optimisation, and
  hierarchical recursion.
- `FlowCalculator::calculateFlow()` at
  [`infomap/src/utils/FlowCalculator.cpp`](../../infomap/src/utils/FlowCalculator.cpp).
  Computes the per-node stationary distribution under the configured
  flow model (random walk, undirected, directed-PageRank, etc.).
- `InfomapOptimizer::run()` at
  [`infomap/src/core/InfomapOptimizer.h`](../../infomap/src/core/InfomapOptimizer.h).
  Per-level partition optimiser; calls
  `optimizeActiveNetwork()` (the iterative single-node-move loop) and
  `consolidateModules()` (the hierarchical aggregation).
- `MapEquation::calcCodelength()` at
  [`infomap/src/core/MapEquation.cpp`](../../infomap/src/core/MapEquation.cpp).
  Eq. 1 evaluator with the per-module flow and exit-flow bookkeeping.

## Map equation (paper Eq. 1)

```
L(M) = q⤸ · H(Q) + Σᵢ p⊙ⁱ · H(Pⁱ)
```

where:

- `q_i⤸` = exit probability of module i = `Σ_{α∈i} p_α · (out_α / d_α)`
- `q⤸` = `Σᵢ q_i⤸`, total inter-module switching probability
- `π_i` = visit fraction of module i = `Σ_{α∈i} p_α`
- `p⊙ⁱ` = `π_i + q_i⤸`, total within-module-codeword frequency for
  module i (paper SI: `Σᵢ p⊙ⁱ = 1 + q⤸`)
- `H(Q)` = entropy of `{q_i⤸ / q⤸}` over modules
- `H(Pⁱ)` = entropy of `{p_α / p⊙ⁱ : α ∈ i}` ∪ `{q_i⤸ / p⊙ⁱ}`

The map equation is a lower bound (paper §"Highlighting Important
Objects") on the average codeword length under any prefix-free code
naming the random walker's positions. The bound is tight when each
module's within-codebook is a Huffman code over its members and the
exit symbol, and the index codebook is a Huffman code over the
per-module exit frequencies.

## Stationary distribution

For undirected unweighted graphs the stationary distribution of a
simple random walk reduces to:

```
p_α = d_α / (2m)
```

Canonical Infomap supports several flow models. The configurable
flow-model dispatch lives in `FlowCalculator.cpp`:

| Flow model | When |
| --- | --- |
| `RandomWalk` | undirected, unweighted (the comdet 32-node fixture) |
| `UndirectedDir` | undirected with PageRank-style smart teleportation |
| `Directed` | directed nets; uses smart-teleportation PageRank |
| `RawDirected` | directed nets without teleportation (rarely correct) |

The fixture is undirected unweighted, so the simple `d/(2m)` form is
exact + matches the canonical binary's `RandomWalk` mode.

## JS-port pipeline

The page kernel lives at
`vltanh.github.io/comdet/js/infomap/infomap.js`. Five stages, all
deterministic:

1. **Stationary distribution.** `stationary(g)` returns
   `d_α / (2m)` per node. Sum-to-one is verified in the page's stage-1
   readout.
2. **Singleton init.** Every node is its own module. Initial
   codelength `L_0 = H(P) + 2`: the index entropy is `H(P)` (every
   step is an exit step, so `q⤸ = 1`); the within-module overhead is
   2 bits across the partition (each module has one node + one exit,
   both with equal weight, contributing `2 p_v` per node).
3. **Greedy pair-joining.** Iteratively find the adjacent module pair
   `(i, j)` whose merge gives the largest negative `ΔL`; merge; repeat
   until no merge improves. Pairs with no inter-module edge are
   skipped, since for a connected graph any such merge only increases
   `L` (the index code gets worse when adding a module the walker
   never visits from the current one).
4. **Single-node tuning.** For each node, evaluate every distinct
   neighbour-module (including its current one), accept the move with
   the most-negative `ΔL`, sweep until idempotent. Greedy variant of
   the paper's heat-bath simulated-annealing refinement.
5. **Sub-level recursion.** For every flat module produced above, run
   the same procedure on the module's induced subgraph. Returns
   nested `.submodules[]` records, depth-bounded at 3.

## Divergence from canonical

| Aspect | Canonical (`infomap/`) | This page's port |
| --- | --- | --- |
| Stationary | Configurable flow model; default `UndirectedDir` w/ PageRank teleportation | `RandomWalk` only (`d_α / (2m)`) |
| Joining | Greedy pair-joining, same as paper §"Mapping Flow" (paper SI) | Same |
| Refinement | Heat-bath SA (paper SI), temperature schedule, P ∝ exp(-ΔL/T) acceptance | Greedy single-node-move only |
| Recursion | Hierarchical greedy + tune at every level until ΔL = 0 | Same, capped at depth 3 |
| RNG | Configurable; deterministic by `--seed` | Deterministic by input order; no randomness |
| Output | `.tree` / `.ftree` / `.clu` files | In-memory `result` object |

The greedy-vs-SA-tuning gap has not been measured on the 32-node
fixture; on the paper's social-sciences map (Fig. 4: 1,431 journals)
SA picks up roughly 1-2% of `L` over greedy. ARI between the two on
the comdet 32-node fixture is pending (Phase 4c §4e verification
matrix).

## Modularity vs map equation (paper Fig. 2 contrast)

Paper §"Mapping Flow Compared with Maximizing Modularity" runs both on
two carefully designed graphs (paper Fig. 2). On the directed-flow
graph, the map equation finds the four flow-trap clusters (`L = 2.67`)
while modularity stuffs everything into two coarse blocks (`Q = 0.50`).
On the no-flow graph, the map equation collapses to one module
(`L = 2.73`) while modularity again picks the topological grouping
(`Q = 0.56`). The disagreement is real: modularity is a
flow-independent statistic (counts edges and degree products); the map
equation is a flow-dependent statistic (counts steps and transitions).

On undirected unweighted graphs the disagreement narrows because the
flow distribution becomes a function of the degree sequence and the
laplacian eigen-gap, which is what modularity already implicitly uses.
The 32-node fixture is mild enough that Leiden-Mod, Leiden-CPM, and
Infomap roughly agree on A's K_5, B's K_4 split, and C's two halves;
they disagree on what to do with A's periphery and the outliers.

## Output (canonical)

The C++ binary writes (selectable via `-o tree,ftree,clu,…`):

- `.tree`: hierarchical, `path flow name id` per row, where `path` is
  a colon-separated module index from root.
- `.ftree`: same as `.tree` plus link-flow records.
- `.clu`: flat, `node_id cluster_id flow` per row.

`run_infomap.py` consumes `.clu` and emits `com.csv` in the standard
`node_id,cluster_id` shape used elsewhere in the pipeline.

## References

- Rosvall M, Bergstrom CT. "Maps of random walks on complex networks
  reveal community structure." PNAS 105(4):1118-1123 (2008).
- Edler D, Bohlin L, Rosvall M. "Mapping Higher-Order Network Flows in
  Memory and Multilayer Networks with Infomap." Algorithms 10(4):112
  (2017). Multi-layer extension; the comdet fixture is single-layer so
  this is informational only.
- Lancichinetti A, Fortunato S. "Community detection algorithms: a
  comparative analysis." PRE 80:056117 (2009). Benchmark comparison
  that places Infomap among the top performers on LFR-generated
  networks.

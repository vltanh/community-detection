# Community detection: technical reference

Algorithm walkthroughs (with interactive viz) live at
[https://vltanh.me/comdet/](https://vltanh.me/comdet/). This page lists
the algorithms in the gallery; per-algorithm technical detail (source
pointers, formulas, departures from canonical, determinism, output
shape, verification status) lives in
[`algorithms/<name>.md`](./algorithms/).

## The algorithms in this gallery

| Algorithm | Family | Stage-2 sampler / optimiser | Source-of-truth |
| --- | --- | --- | --- |
| [`louvain`](./algorithms/louvain.md) | Greedy modularity | Two-phase modularity ascent + super-graph collapse | [`louvain-generic/gen-louvain/`](../louvain-generic/gen-louvain/) (Campigotto-Conde Céspedes-Guillaume v0.3, July 2013) |
| [`leiden-cpm`](./algorithms/leiden.md) | Refined modularity ascent + CPM quality | Move + refine + aggregate, CPM resolution sweep | [`leidenalg/`](../leidenalg/) (Python wrapper) + [`libleidenalg/`](../libleidenalg/) (algorithm body) |
| [`leiden-mod`](./algorithms/leiden.md) | Same body, modularity quality | same | same |
| [`sbm-flat-dc`](./algorithms/sbm.md) | Bayesian SBM, flat, degree-corrected | Single-vertex Metropolis-Hastings on description length Σ | [`graph-tool/`](../graph-tool/) (release-2.98) `BlockState` |
| `sbm-{flat-ndc, flat-pp, nested-dc, nested-ndc, flat-best, nested-best}` | SBM variants over the same kernel | same Σ minimisation, variant-specific entropy | same |
| [`ikc`](./algorithms/ikc.md) | k-core peel | Iterated k-core extraction + modularity gate | NetworKit core decomposition |
| [`cc`](./algorithms/cc.md) | Post-proc: connected-component split | Per-cluster BFS, no threshold, no recursion | [`constrained-clustering/`](../constrained-clustering/) `MincutOnly --connectedness-criterion 0` |
| [`wcc`](./algorithms/wcc.md) | Post-proc: well-connected-components split | Per-cluster mincut + threshold check, recurse on weak cuts | [`constrained-clustering/`](../constrained-clustering/) `MincutOnly` over VieCut |
| [`cm`](./algorithms/cm.md) | Post-proc: cut + re-cluster the halves | WCC loop + base-algo re-cluster on each split half until well-connected | [`constrained-clustering/`](../constrained-clustering/) `CM` + Leiden via libleidenalg |

Other algorithms (Infomap, MCL) are planned but unowned in this
session. See the active plan
([`memory/plan_state_system_extension.md`](../../netsci-research/memory/plan_state_system_extension.md))
for the full Phase 4 build order.

All gallery pages run on the same shared substrate at
[`vltanh.github.io/comdet/`](../vltanh.github.io/comdet/): one 32-node
fixture (`js/fixture.js`), one shared-style/runtime layer
(`shared.js`, `shared.css`), one page-helper layer
(`js/comdet/page_helpers.js`). Every algorithm imports the
graph-partition primitives from `js/louvain/louvain.js` (MT19937,
shuffle, `Graph`, `Partition`); per-algorithm modules live under
`js/<algo>/`.

## Summary: who solves what

| Property | louvain | leiden-cpm | leiden-mod | sbm-flat-dc | ikc |
| --- | --- | --- | --- | --- | --- |
| *Quality function*                  |     |     |     |     |     |
| Modularity Q                         | ✓   | —   | ✓   | —   | gate |
| CPM (constant Potts)                 | —   | ✓   | —   | —   | —   |
| Description length Σ (Bayesian MDL)  | —   | —   | —   | ✓   | —   |
| *Output guarantees*                 |     |     |     |     |     |
| Internally connected communities     | —   | ✓   | ✓   | —   | ✓   |
| Resolution-tunable                   | —   | ✓   | —   | auto | —   |
| Built-in B selection                 | —   | —   | —   | ✓   | ✓ (k) |
| *Determinism*                       |     |     |     |     |     |
| Same seed → same partition           | ✓   | ✓   | ✓   | ✓   | ✓ (no RNG) |
| Bit-equivalent to canonical          | pending | pending | pending | pending | pending |

✓ = the algorithm has the property by construction. — = not provided.
"pending" entries refer to the Phase 4c §4e verification matrix; per-algo
detail in each `algorithms/<name>.md`.

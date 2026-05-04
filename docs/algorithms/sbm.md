# SBM

[← back to index](../algorithms.md)

Bayesian inference of a flat or nested stochastic block model. Seven
gallery pages share one JS kernel; this doc covers the kernel and
reads it through the lens of `sbm-flat-dc` (the only variant fully
wired today).

## Pages and flags

| Page | `degCorr` | Entropy form | Status |
| --- | --- | --- | --- |
| `sbm-flat-dc` | `true` | sparse, DC | wired |
| `sbm-flat-ndc` | `false` | sparse, NDC | UC stub |
| `sbm-flat-pp` | n/a | planted-partition | UC stub |
| `sbm-nested-dc` | `true` | sparse, DC, nested levels | UC stub |
| `sbm-nested-ndc` | `false` | sparse, NDC, nested levels | UC stub |
| `sbm-flat-best` | meta | runs flat-{dc, ndc, pp}, picks min Σ | UC stub |
| `sbm-nested-best` | meta | runs nested-{dc, ndc}, picks min Σ | UC stub |

## Kernel

Three JS modules under
[`vltanh.github.io/comdet/js/sbm/`](../../vltanh.github.io/comdet/js/sbm/),
loaded in dependency order:

| Module | Substance | Source |
| --- | --- | --- |
| `util.js` | `lgamma`, `lbinom`, `xlogx`, `safelog`, `logChooseRep` | port of [`graph-tool/src/graph/inference/support/util.hh`](../../graph-tool/src/graph/inference/support/util.hh) |
| `block_state.js` | `BlockState`: admin tables (`e_rs`, `e_r`, `n_r`, `k_v`) + `entropy`, `virtualMove`, `moveVertex` | port of [`graph_blockmodel.hh:111`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel.hh#L111) (template) + [`graph_blockmodel_entropy.hh`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel_entropy.hh) (eterm/vterm/get_edges_dl) |
| `mcmc.js` | `mcmcSweep`, `equilibrate`, `candidatePool` | port of [`mcmc_loop.hh:104-200`](../../graph-tool/src/graph/inference/loops/mcmc_loop.hh#L104) |

Substrate: the SBM kernel reuses `COMDET.LOUVAIN.Graph`, `MT19937`,
`shuffle` from
[`vltanh.github.io/comdet/js/louvain/louvain.js`](../../vltanh.github.io/comdet/js/louvain/louvain.js).
No separate Partition primitive: the SBM tracks block-block edge
counts $e_{rs}$, not Leiden's per-community weight caches.

## Description length

The total Σ the kernel minimises decomposes into four terms (Peixoto
2017 ch. 11 §V + §VI). All formulas verbatim from
[`graph_blockmodel_entropy.hh`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel_entropy.hh)
unless noted; JS implementations all in
[`block_state.js`](../../vltanh.github.io/comdet/js/sbm/block_state.js).

| Term | Formula | Code path |
| --- | --- | --- |
| Sparse entropy (DC) | $\sum_r x\log x(e_r) - \sum_{r<s} x\log x(e_{rs}) - \tfrac{1}{2}\sum_r x\log x(e_{rr})$ | [`block_state.js`](../../vltanh.github.io/comdet/js/sbm/block_state.js) `sparseEntropy()`; verbatim eterm/vterm at [`graph_blockmodel_entropy.hh:102, :118`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel_entropy.hh#L102) |
| Sparse entropy (NDC) | replaces $x\log x(e_r)$ with $e_r \cdot \log n_r$ in the vertex term | same; vterm branches on `deg_corr` |
| Edges DL | $\log\binom{B(B+1)/2 + E - 1}{E}$ | [`block_state.js`](../../vltanh.github.io/comdet/js/sbm/block_state.js) `edgesDl()`; verbatim of [`graph_blockmodel_entropy.hh:172`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel_entropy.hh#L172) (`get_edges_dl`) |
| Partition DL | $\log N! - \sum_r \log n_r! + \log\binom{N-1}{B-1} + \log N$ | [`block_state.js`](../../vltanh.github.io/comdet/js/sbm/block_state.js) `partitionDl()`; Peixoto 2017 Eq 17 |
| Degree DL (DC only) | $\sum_r \log\binom{n_r + e_r - 1}{e_r}$ | [`block_state.js`](../../vltanh.github.io/comdet/js/sbm/block_state.js) `degreeDlUniform()`; Peixoto 2017 Eq 44 (uniform variant) |

`BlockState` uses graph-tool's diagonal convention: $e_{rr}$ counts each
internal edge twice (every endpoint is a stub in $r$). A self-loop on
$v$ adds 2 to $e_{b_v, b_v}$.

`exact = false` is the default: sparse-Stirling approximation, what
canonical `mcmc_sweep` uses on every move. The `exact` form
([`graph_blockmodel_entropy.hh:60-97`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel_entropy.hh#L60))
is not ported.

## MCMC sweep

Entrypoint: `COMDET.SBM.mcmcSweep(state, rng, opts)` at
[`mcmc.js`](../../vltanh.github.io/comdet/js/sbm/mcmc.js). One pass:

```text
shuffle nodes via MT19937
for each vertex v in shuffled order:
  cands = nonempty blocks ∪ {fresh empty slot if n[b[v]] > 1}
  s_new = uniform-pick from cands
  ΔΣ = state.virtualMove(v, s_new)
  accept = ΔΣ ≤ 0  ||  rng() < exp(-β · ΔΣ)
  if accept and s_new != b[v]:
    state.moveVertex(v, s_new)
  record (v, b[v]_old, s_new, ΔΣ, accepted, candidates)
```

`equilibrate(state, rng, { sweeps: K })` flattens `K` sweeps into one
trace + a per-sweep `series` of `{ sweep, S, B, accepted }` for the
equilibration plot.

### Departures from canonical

| Aspect | Canonical (graph-tool) | JS port |
| --- | --- | --- |
| Proposal | `sample_block(v, c, d)` weights candidates by neighbour-block frequencies; $c \to \infty$ falls back to uniform ([`graph_blockmodel.hh:1563`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel.hh#L1563)) | uniform over current candidates (the $c \to \infty$ limit) |
| MH correction | `get_move_prob(v, r, s, c, d, reverse)` accounts for proposal-distribution shift | symmetric uniform proposal → ratio reduces to $e^{-\beta\Delta\Sigma}$. The O(1/B) bias when the move opens or closes a block is dropped. |
| RNG | `pcg64_k1024` ([`graph-tool/src/graph/random.hh:24`](../../graph-tool/src/graph/random.hh#L24)), seeded via `gt.seed_rng(int)` | igraph MT19937, the substrate shared with Louvain + Leiden |
| Multilevel merges | `multilevel_mcmc_sweep` runs MH alongside agglomerative merges ([`multilevel.hh:609`](../../graph-tool/src/graph/inference/loops/multilevel.hh#L609) `merge_sweep`) | not ported. Single-vertex moves only; chain still converges on small graphs but slower at scale. |
| Initial partition | `minimize_blockmodel_dl` starts at $B = 1$ + bisection-merges to find optimal $B$ ([`minimize.py:26`](../../graph-tool/src/graph_tool/inference/minimize.py#L26)) | random partition with $B_0 = 8$ |

## Walker stages (`sbm-flat-dc.html`)

Page glue at
[`page_flat_dc.js`](../../vltanh.github.io/comdet/js/sbm/page_flat_dc.js):

| Stage | What it shows | Backed by |
| --- | --- | --- |
| 0 input | 32-node fixture coloured by planted ground-truth | `COMDET.PAGE.renderFixture` |
| 1 init | Random partition over $B_0 = 8$ blocks; $\Sigma_0$ | `BlockState({ init: random8 })` |
| 2 walk | Per-vertex MH walker with full candidate panel + ΔΣ per candidate | `mcmcSweep({ recordCandidates: true })` + `mountStepWalker` |
| 3 equilibrate | $\Sigma$ and $B$ per sweep over the full chain (line chart) | `equilibrate({ sweeps: 20 })` `series` |
| 4 final | Mode partition vs. ground-truth + per-block stats | `mountFinalCompare` + `renderStatsTable` |

The init seed is exposed via the `g-seed` input + `g-reroll` button.
`reroll` re-builds the chain from scratch and rebuilds every stage
downstream of init.

## Determinism

Three sources of randomness, all seeded:

```text
const rng = COMDET.LOUVAIN.MT19937(seed >>> 0);
// shuffle the init permutation
// shuffle the per-sweep visit order
// pick uniform-random target block
```

The kernel never reads `Math.random` or `Date.now`. Same seed →
identical chain → identical `traces` array. Different seed → different
shuffle / different proposal sequence → different (typically
similar-quality) partition.

`BlockState.blockMembership()` returns raw block ids, not renumbered
by size. Block colour comes from `partitionColor(c)` in
[`page_helpers.js`](../../vltanh.github.io/comdet/js/comdet/page_helpers.js)
which keys off the raw id, so two runs with the same seed paint the
canvas identically.

## Output shape

`mcmcSweep` returns:

```text
{
  traces: [
    { v, fromR, toS, dS, accepted, candidates?, sweep? },
    ...
  ],
  accepted: number,    // count of accepted moves this sweep
  dStotal: number      // sum of accepted ΔΣ this sweep
}
```

`equilibrate` flattens many sweeps into one trace + adds
`series: [{ sweep, S, B, accepted }, ...]`.

## Verification

Phase 4c §4e matrix
([plan](../../../netsci-research/memory/plan_state_system_extension.md)):

| Artifact | Method | Acceptance |
| --- | --- | --- |
| Σ at fixed partition | JS `entropy()` vs. graph-tool's `state.entropy()` on identical $\boldsymbol{b}$ | abs diff < 1e-9 |
| Mode partition | JS chain vs. fixture ground-truth, ensemble across 10 seeds | mean ARI ≥ 0.85 |
| Σ at JS mode vs. canonical mode | relative gap | ≤ 5% |
| Page render | puppeteer + `localhost:8080`, scrub the walker, console clean | no errors, walker advances 1..640 |

ARI is relaxed below 1.0 because the JS kernel uses MT19937 (not
graph-tool's `pcg64_k1024`), so proposal sequences diverge on shared
seed. Porting `pcg64_k1024` to JS is on the follow-up;
[`plan §16 D16-revised`](../../../netsci-research/memory/plan_state_system_extension.md)
covers the trade-off.

The pass itself is **pending**. Page caveat in
[`sbm-flat-dc.html`](../../vltanh.github.io/comdet/sbm-flat-dc.html)
flags this.

## Where to look next

- [`block_state.js`](../../vltanh.github.io/comdet/js/sbm/block_state.js): admin tables + entropy
- [`mcmc.js`](../../vltanh.github.io/comdet/js/sbm/mcmc.js): MH sweep + equilibrate
- [`page_flat_dc.js`](../../vltanh.github.io/comdet/js/sbm/page_flat_dc.js): page glue + walker wiring
- [`graph_blockmodel.hh`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel.hh): canonical `BlockState` template
- [`mcmc_loop.hh`](../../graph-tool/src/graph/inference/loops/mcmc_loop.hh): canonical sweep body

## Other variants (UC)

Each variant slots into the same `BlockState` + `mcmcSweep` substrate:

- `sbm-flat-ndc`: `degCorr: false`. Drops the degree DL; vterm switches to $e_r \cdot \log n_r$.
- `sbm-flat-pp`: replaces `BlockState` with a `PPBlockState`-style entropy (one within-block rate, one between-block rate). Source: [`planted_partition.py:34`](../../graph-tool/src/graph_tool/inference/planted_partition.py#L34).
- `sbm-nested-{dc,ndc}`: composes flat `BlockState` per level (level 0 the actual graph, level $l$ the level-$l-1$ block-graph). Source: [`nested_blockmodel.py:33`](../../graph-tool/src/graph_tool/inference/nested_blockmodel.py#L33).
- `sbm-{flat,nested}-best`: meta-page running all variants in parallel, rendering lowest-Σ winner. Mirrors `run_cd.sh` `*-best` cascade and `choose_best_sbm.py`.

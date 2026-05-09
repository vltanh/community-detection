# SBM

[← back to index](../algorithms.md)

Companion to the seven gallery pages, entry point
[`comdet/sbm-flat-best.html`](https://vltanh.me/comdet/sbm-flat-best.html)
(plus `sbm-flat-{dc,ndc,pp}` and `sbm-nested-{best,dc,ndc}`).

Bayesian inference of a flat or nested stochastic block model. Seven
gallery pages share one JS kernel: five per-variant walkers
(`sbm-flat-{dc,ndc,pp}`, `sbm-nested-{dc,ndc}`) plus two meta
comparison pages (`sbm-flat-best`, `sbm-nested-best`). The walker
pages all run through `mountWalkerPage`; the meta pages through
`mountComparePage`. Per-variant detail is in the variant table
below; per-page glue files are one-config wrappers around those two
factories.

## Pages and flags

| Page | `mode` | Entropy form | Status |
| --- | --- | --- | --- |
| `sbm-flat-dc` | `dc` | sparse, DC + degree DL | wired (walker) |
| `sbm-flat-ndc` | `ndc` | sparse, NDC | wired (walker) |
| `sbm-flat-pp` | `pp` | Bernoulli, two-rate (Zhang + Peixoto 2020) | wired (walker) |
| `sbm-nested-dc` | `dc` | level-0 walker only; level-1+ MCMC pending | wired (walker), nested prose stub |
| `sbm-nested-ndc` | `ndc` | level-0 walker only; level-1+ MCMC pending | wired (walker), nested prose stub |
| `sbm-flat-best` | meta | runs flat-{dc, ndc, pp}, compares Σ, flags winner | wired (no walker; comparison panel) |
| `sbm-nested-best` | meta | runs nested-{dc, ndc}, compares Σ | wired (no walker; comparison panel) |

The seven pages share `block_state.js` + `mcmc.js`. Per-page glue
files live alongside (`page_flat_{dc,ndc,pp}.js`,
`page_nested_{dc,ndc}.js`, `page_flat_best.js`,
`page_nested_best.js`).

## Kernel

JS modules under
[`vltanh.github.io/comdet/js/sbm/`](../../vltanh.github.io/comdet/js/sbm/),
loaded in dependency order:

| Module | Substance | Source |
| --- | --- | --- |
| `util.js` | `lgamma`, `lbinom`, `xlogx`, `safelog`, `logChooseRep` | port of [`graph-tool/src/graph/inference/support/util.hh`](../../graph-tool/src/graph/inference/support/util.hh) |
| `block_state.js` | `BlockState`: admin tables (`e_rs`, `e_r`, `n_r`, `k_v`, `Bne` non-empty count) + `entropy`, `virtualMove`, `moveVertex`, `nonEmptyBlocks`, `blockOf`, `blockSize`, `blockMembership` | port of [`graph_blockmodel.hh:111`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel.hh#L111) (template) + [`graph_blockmodel_entropy.hh`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel_entropy.hh) (eterm/vterm/get_edges_dl) |
| `mcmc.js` | `mcmcSweep`, `equilibrate`, `candidatePool` | port of [`mcmc_loop.hh:104-200`](../../graph-tool/src/graph/inference/loops/mcmc_loop.hh#L104) |
| `trace_plot.js` | `tracePlot({hostId, traces, xMax})` — shared single-/multi-trace line plot for the equilibration panels | own |
| `walker_page.js` | `mountWalkerPage({gen, blockOpts, sweeps, initB})` — full page glue for the five per-variant walker pages | own |
| `compare_page.js` | `mountComparePage({gen, variants})` + `VARIANTS = {dc, ndc, pp}` registry — full page glue for the two `*-best` comparison pages | own |

Substrate: the SBM kernel reuses `COMDET.LOUVAIN.Graph`, `MT19937`,
`shuffle` from
[`vltanh.github.io/comdet/js/louvain/louvain.js`](../../vltanh.github.io/comdet/js/louvain/louvain.js).
No separate Partition primitive: the SBM tracks block-block edge
counts $e_{rs}$, not Leiden's per-community weight caches.

## Description length

The total Σ the kernel minimises decomposes into four terms (Peixoto
2017 ch. 11 §V + §VI). The kernel uses **exact microcanonical**
forms (lgamma + lbinom) for all three modes so absolute Σ values are
commensurable in MDL across DC/NDC/PP, allowing the `*-best` pages to
compare them under a single Bayes-factor reading.

| Term | Formula | Code path |
| --- | --- | --- |
| Exact entropy (DC) | $\sum_r \log(e_r!) - \sum_{r<s} \log(e_{rs}!) - \sum_r \log(e_{rr}!!) - \sum_i \log(k_i!)$ (last term is partition-independent but model-class-specific; included to commensurate with NDC) | [`block_state.js`](../../vltanh.github.io/comdet/js/sbm/block_state.js) `exactEntropy()` (vertex + edge terms) + `dcDegreeConst` (per-vertex term); per Peixoto Eq 43, simple-graph form |
| Exact entropy (NDC) | $\sum_r e_r \log n_r - \sum_{r<s} \log(e_{rs}!) - \sum_r \log(e_{rr}!!)$ | same; vterm branches on `degCorr`; per Peixoto Eq 22, simple-graph form |
| Exact PP likelihood | $\log\binom{M_{\text{in}}}{E_{\text{in}}} + \log\binom{M_{\text{out}}}{E_{\text{out}}}$ (uniform draw from simple graphs with exactly $E_{\text{in}}$ internal + $E_{\text{out}}$ external edges) | `ppLikelihood()`; microcanonical PP per [Zhang + Peixoto 2020](https://doi.org/10.1103/PhysRevResearch.2.043271). Source map: [`planted_partition.py:34`](../../graph-tool/src/graph_tool/inference/planted_partition.py#L34) |
| Edges DL (DC/NDC) | $\log\binom{B(B+1)/2 + E - 1}{E}$ | `edgesDl()`; verbatim of [`graph_blockmodel_entropy.hh:172`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel_entropy.hh#L172) (`get_edges_dl`) |
| Edges DL (PP) | $\log(\min(E, M_{\text{in}}) + 1)$ (uniform prior on the integer $E_{\text{in}}$; $E_{\text{out}}$ follows from $E$) | `ppEdgesDl()` |
| Partition DL | $\log N! - \sum_r \log n_r! + \log\binom{N-1}{B-1} + \log N$ | `partitionDl()`; Peixoto 2017 Eq 17 |
| Degree DL (DC only) | $\sum_r \log\binom{n_r + e_r - 1}{e_r}$ | `degreeDlUniform()`; Peixoto 2017 Eq 44 (uniform variant) |

`BlockState` uses the doubled-diagonal convention: $e_{rr}$ counts each
internal edge twice (every endpoint is a stub in $r$). A self-loop on
$v$ adds 2 to $e_{b_v, b_v}$. The `e_{rr}!!` term in the entropy unfolds
to $(e_{rr}/2) \cdot \log 2 + \log((e_{rr}/2)!)$ for the doubled count.

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

## Variant glue (per page)

Each per-page glue file is a one-config wrapper over either
`mountWalkerPage` or `mountComparePage`. The defaults `mode` defines
which entropy + DL chain runs:

| Variant | `mountX` call | Glue file |
| --- | --- | --- |
| `flat-dc` | `mountWalkerPage({ gen: "sbm-flat-dc",  blockOpts: { mode: "dc"  } })` | [`page_flat_dc.js`](../../vltanh.github.io/comdet/js/sbm/page_flat_dc.js) |
| `flat-ndc` | `mountWalkerPage({ gen: "sbm-flat-ndc", blockOpts: { mode: "ndc" } })` | [`page_flat_ndc.js`](../../vltanh.github.io/comdet/js/sbm/page_flat_ndc.js) |
| `flat-pp` | `mountWalkerPage({ gen: "sbm-flat-pp",  blockOpts: { mode: "pp"  } })` | [`page_flat_pp.js`](../../vltanh.github.io/comdet/js/sbm/page_flat_pp.js) |
| `nested-dc` | `mountWalkerPage({ gen: "sbm-nested-dc",  blockOpts: { mode: "dc"  } })` | [`page_nested_dc.js`](../../vltanh.github.io/comdet/js/sbm/page_nested_dc.js) |
| `nested-ndc` | `mountWalkerPage({ gen: "sbm-nested-ndc", blockOpts: { mode: "ndc" } })` | [`page_nested_ndc.js`](../../vltanh.github.io/comdet/js/sbm/page_nested_ndc.js) |
| `flat-best` | `mountComparePage({ gen: "sbm-flat-best",  variants: [V.dc, V.ndc, V.pp] })` | [`page_flat_best.js`](../../vltanh.github.io/comdet/js/sbm/page_flat_best.js) |
| `nested-best` | `mountComparePage({ gen: "sbm-nested-best", variants: [V.dc, V.ndc] })` | [`page_nested_best.js`](../../vltanh.github.io/comdet/js/sbm/page_nested_best.js) |

The `dc` mode auto-enables degree DL; `ndc` skips it; `pp` swaps the
sparse-entropy + flat-edges-DL chain for `ppLikelihood + ppEdgesDl`.
`partitionDl` is on by default for every mode. Defaults match
graph-tool's `BlockState(...)` / `PPBlockState(...)` constructor
defaults; explicit per-variant DL flags are intentionally omitted from
the glue files.

## Future work

- **Nested level-1+ MCMC**. The current nested pages run only the level-0 walker; the canonical [`nested_blockmodel.py:33`](../../graph-tool/src/graph_tool/inference/nested_blockmodel.py#L33) composes per-level `BlockState` instances and runs MCMC on each level alongside joint moves. Implementation: a `NestedBlockState` wrapper that owns an array of `BlockState`s, rebuilds level $l$'s graph as the collapsed block-graph from level $l-1$ on every move, and exposes a multi-level `mcmcSweep` that visits each level in turn. Total Σ adds the per-level partition DL and per-level edges DL contributions ([Peixoto 2017 Eq 28-30](https://doi.org/10.1002/9781119483298.ch11)).
- **PP-aware proposal distribution**. The PP variant currently uses the same uniform candidate pool as DC/NDC. Canonical PP biases proposals towards the local neighbourhood ([`planted_partition.py`](../../graph-tool/src/graph_tool/inference/planted_partition.py) `sample_block`), which converges much faster on assortative networks.
- **Multilevel merge moves**. Single-vertex MH only finds the right partition slowly on small graphs and not at all reliably on large ones. The canonical [`multilevel_mcmc_sweep`](../../graph-tool/src/graph/inference/loops/multilevel.hh#L77) interleaves agglomerative merges with single-vertex moves; porting `merge_sweep` to JS is the next big step.
- **PCG64 RNG**. `pcg64_k1024` ([`graph-tool/src/graph/random.hh:24`](../../graph-tool/src/graph/random.hh#L24)) replaces MT19937 in the canonical sampler. Bit-equivalence with canonical seeded runs requires this.
- **Verification**. Phase 4c §4e matrix has not been run yet for any variant. The pages all carry a draft caveat.

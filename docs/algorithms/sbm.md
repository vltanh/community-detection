# SBM

[← back to index](../algorithms.md)

Companion to the seven gallery pages. Entry point:
[`comdet/sbm-flat-best.html`](https://vltanh.me/comdet/sbm-flat-best.html)
(plus `sbm-flat-{dc,ndc,pp}` and `sbm-nested-{best,dc,ndc}`).

Bayesian inference of a flat or nested stochastic block model under the
microcanonical posterior. Seven gallery pages share one JS kernel: five
per-variant walkers (`sbm-flat-{dc,ndc,pp}`, `sbm-nested-{dc,ndc}`) and
two meta comparison pages (`sbm-flat-best`, `sbm-nested-best`). Anchor
pages live at `sbm-flat-best.html` (flat family) and
`sbm-nested-best.html` (nested family); the variant walker pages defer
to the anchors for the family-level Bayesian framing.

## Variants and CLI flags

The pages mirror the seven `run_cd.sh` variants. Each maps to a
`mountWalkerPage` or `mountComparePage` call with one config arg, and
each runs under the corresponding `graph-tool` API.

| Page | `mode` | Entropy form | Walker | graph-tool API |
| --- | --- | --- | --- | --- |
| `sbm-flat-dc` | `dc` | sparse DC + degree DL | per-vertex MH | `BlockState(g, deg_corr=True)` |
| `sbm-flat-ndc` | `ndc` | sparse NDC | per-vertex MH | `BlockState(g, deg_corr=False)` |
| `sbm-flat-pp` | `pp` | Bernoulli two-rate (Zhang and Peixoto 2020) | per-vertex MH | `PPBlockState(g)` (page diverges; see below) |
| `sbm-nested-dc` | `dc` (level 0) | DC at L0, NDC at L>=1 | per-vertex MH at L0 only | `NestedBlockState(g, state_args=dict(deg_corr=True))` |
| `sbm-nested-ndc` | `ndc` (every level) | NDC at every level | per-vertex MH at L0 only | `NestedBlockState(g, state_args=dict(deg_corr=False))` |
| `sbm-flat-best` | meta | runs flat-{dc,ndc,pp}, compares Σ, flags winner | no walker; comparison panel | n/a (meta page) |
| `sbm-nested-best` | meta | runs nested-{dc,ndc}, compares Σ | no walker; comparison panel | n/a (meta page) |

CLI defaults (graph-tool `minimize_blockmodel_dl` and
`minimize_nested_blockmodel_dl`):

| Flag | Default | Effect |
| --- | --- | --- |
| `state_args=dict(deg_corr=True)` | `True` | DC entropy form. Set `False` for NDC. |
| `degree_dl_kind` | `"distributed"` | DC degree DL form. JS port uses `"uniform"` only (Peixoto Eq 44). |
| `nested` | inferred | Flat vs. nested. JS port has separate `NestedBlockState` wrapper. |
| `beta` | `numpy.inf` (cooled to 1) in `minimize`; `1.0` in `mcmc_sweep` | MH inverse temperature. JS pages run at `β = 1` (posterior sampling). |
| `multilevel_mcmc_sweep` | enabled | Agglomerative merge moves alongside single-vertex MH. Not ported to JS. |

The JS kernel uses single-vertex moves only; multilevel agglomerative
merges are absent. See "Departures from canonical" below.

## Kernel modules

JS modules under
[`vltanh.github.io/comdet/js/sbm/`](../../vltanh.github.io/comdet/js/sbm/),
loaded in dependency order:

| Module | Substance | Source |
| --- | --- | --- |
| `util.js` | `lgamma`, `lbinom`, `xlogx`, `safelog`, `logChooseRep`, `shuffle` (backward Fisher-Yates) | port of [`graph-tool/src/graph/inference/support/util.hh`](../../graph-tool/src/graph/inference/support/util.hh) |
| `rng.js` | `MT19937(seed)` with `raw()`, `int(lo, hi)`, `drawCount()` | own; mirrors `std::mt19937` in the cpp tracer |
| `graph.js` | `Graph(n, edges, opts)` with self-twice strength convention | own; mirrors the cpp tracer's Graph struct |
| `block_state.js` | `BlockState`: admin tables (`b`, `nr`, `er`, `ers`, `Bne`, `neList`, `neIdx`) + `entropy`, `virtualMove`, `moveVertex`, `addBlock`, `nonEmptyBlocks`, `blockOf`, `blockSize`, `blockMembership` | port of [`graph_blockmodel.hh:111`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel.hh#L111) (template) + [`graph_blockmodel_entropy.hh`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel_entropy.hh) (eterm, vterm, get_edges_dl) |
| `mcmc.js` | `mcmcSweep`, `equilibrate`, `candidatePool` | port of [`mcmc_loop.hh:104-200`](../../graph-tool/src/graph/inference/loops/mcmc_loop.hh#L104) |
| `nested_state.js` | `NestedBlockState`, `buildLevelGraph` | own; mirrors [`nested_blockmodel.py:33`](../../graph-tool/src/graph_tool/inference/nested_blockmodel.py#L33) structurally; entropy form differs (see below) |
| `trace_plot.js` | `tracePlot({hostId, traces, xMax})` | own; shared single- and multi-trace line plot |
| `walker_page.js` | `mountWalkerPage` | own; full page glue for the five per-variant walkers |
| `compare_page.js` | `mountComparePage` + `VARIANTS = {dc, ndc, pp}` registry | own; full page glue for the two meta pages |
| `page_*.js` | per-variant one-config wrappers around `mountWalkerPage` or `mountComparePage` | own |

The SBM kernel is self-contained: it ships its own `Graph`, `MT19937`,
and `shuffle` rather than reusing the Louvain substrate. The
`SBM.Graph.strength(v)` doubles self-loops (Peixoto Eq 43 doubled-stub
convention), where the Louvain substrate single-counts. The
`SBM.UTIL.shuffle` runs backward Fisher-Yates to match
`std::shuffle` in the cpp tracer; the Louvain substrate's shuffle runs
forward (mirroring `louvain.cpp:222-229`).

## Description length

Total Σ per Peixoto 2017 ch. 11 §V + §VI decomposes into three or four
terms. The kernel uses exact microcanonical forms (lgamma + lbinom) for
all modes, so absolute Σ values are commensurable in MDL units across
DC/NDC/PP at the same partition. The `*-best` pages exploit this to
compare them under one Bayes-factor reading.

| Term | Formula | Code path | Source |
| --- | --- | --- | --- |
| Sparse entropy (DC) | \(\sum_r \log(e_r!) - \sum_{r<s} \log(e_{rs}!) - \sum_r [\log((e_{rr}/2)!) + (e_{rr}/2)\log 2]\) | [`block_state.js`](../../vltanh.github.io/comdet/js/sbm/block_state.js) `exactEntropy()` + `dcDegreeConst` (per-vertex term) | Peixoto Eq 43 simple-graph form |
| Sparse entropy (NDC) | \(\sum_r e_r \log n_r - \sum_{r<s} \log(e_{rs}!) - \sum_r [\log((e_{rr}/2)!) + (e_{rr}/2)\log 2]\) | same; `vterm` branches on `degCorr` | Peixoto Eq 22 |
| PP likelihood | \(\log\binom{M_{\text{in}}}{E_{\text{in}}} + \log\binom{M_{\text{out}}}{E_{\text{out}}}\) | `ppLikelihood()` | [Zhang and Peixoto 2020](https://doi.org/10.1103/PhysRevResearch.2.043271) |
| Edges DL (DC, NDC) | \(\log\binom{B(B+1)/2 + E - 1}{E}\) | `edgesDl()` | verbatim of [`graph_blockmodel_entropy.hh:172`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel_entropy.hh#L172) (`get_edges_dl`) |
| Edges DL (PP) | \(\log(\min(E, M_{\text{in}}) + 1)\) | `ppEdgesDl()` | own; uniform prior on \(E_{\text{in}}\) |
| Partition DL | \(\log N! - \sum_r \log n_r! + \log\binom{N-1}{B-1} + \log N\) | `partitionDl()` | Peixoto 2017 Eq 17 |
| Degree DL (uniform, DC only) | \(\sum_r \log\binom{n_r + e_r - 1}{e_r}\) | `degreeDlUniform()` | Peixoto 2017 Eq 44 (uniform variant) |

The doubled-diagonal convention: \(e_{rr}\) counts each internal edge
twice (each endpoint is a stub in \(r\)). A self-loop on \(v\) adds 2
to \(e_{b_v, b_v}\). The \(e_{rr}!!\) term in the canonical entropy
unfolds to \((e_{rr}/2) \log 2 + \log((e_{rr}/2)!)\) under this
convention.

`exact = false` is graph-tool's default for `mcmc_sweep` (sparse
Stirling approximation). The JS kernel implements the exact form only
(lgamma); the sparse form is not ported. Numerical drift between the
two forms is sub-ulp at the partitions visited by the chain, so the
divergence is structural rather than observed.

## MCMC sweep

Entrypoint: `COMDET.SBM.mcmcSweep(state, rng, opts)` at
[`mcmc.js`](../../vltanh.github.io/comdet/js/sbm/mcmc.js). One pass:

```text
shuffle nodes via MT19937 (backward Fisher-Yates)
for each vertex v in shuffled order:
  cands = nonempty blocks ∪ {fresh empty slot if blockSize(b[v]) > 1}
  s_new = uniform-pick from cands
  ΔΣ = state.virtualMove(v, s_new)
  accept = ΔΣ ≤ 0  ||  rng() / 2^32 < exp(-β · ΔΣ)
  if accept and s_new != b[v]:
    state.moveVertex(v, s_new)
  record (v, b[v]_old, s_new, ΔΣ, accepted, candidates)
```

`equilibrate(state, rng, { sweeps: K })` flattens `K` sweeps into one
trace + a per-sweep `series` of `{ sweep, S, B, accepted }` for the
equilibration plot.

The `candidatePool(state, v)` function returns the non-empty blocks
plus, when `blockSize(blockOf(v)) > 1`, a fresh-block id one greater
than the current `maxId`. When the fresh id is at or above the current
`B` capacity, the function calls `state.addBlock(...)` first to grow
the per-block scalars and the \(B \times B\) `ers` matrix. This
mirrors graph-tool's `get_empty_block` calling
`BlockState::add_block(size_t n=1)` at
[`graph_blockmodel.hh:1513-1528`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel.hh#L1513).

### Departures from canonical

| Aspect | Canonical (graph-tool release-2.98) | JS port |
| --- | --- | --- |
| RNG | `pcg64_k1024` ([`graph-tool/src/graph/random.hh:24`](../../graph-tool/src/graph/random.hh#L24)) | `std::mt19937`-equivalent `SBM.MT19937` |
| Proposal | `sample_block(v, c, d)` weights candidates by neighbour-block frequencies; `c→∞` falls back to uniform ([`graph_blockmodel.hh:1563`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel.hh#L1563)) | uniform over current candidates (`c→∞` limit) |
| MH correction | `get_move_prob(v, r, s, c, d, reverse)` accounts for proposal-distribution shift | symmetric uniform proposal; ratio reduces to \(e^{-\beta \Delta\Sigma}\) |
| Entropy form | sparse Stirling default (`exact=false`); dense and multigraph forms available | exact lgamma form only |
| Multilevel merges | `multilevel_mcmc_sweep` interleaves single-vertex MH with agglomerative merges ([`multilevel.hh:609`](../../graph-tool/src/graph/inference/loops/multilevel.hh#L609) `merge_sweep`) | not ported. Single-vertex moves only. |
| Initial partition | `minimize_blockmodel_dl` starts at `B = 1` and bisection-merges to find optimal `B` ([`minimize.py:26`](../../graph-tool/src/graph_tool/inference/minimize.py#L26)) | random partition with `B_0 = 8` |
| Nested entropy | `NestedBlockState.entropy` uses dense + multigraph `eterm` + Lrecdx coupling ([`nested_blockmodel.py:359`](../../graph-tool/src/graph_tool/inference/nested_blockmodel.py#L359)) | self-consistent sum `Σ_l states[l].entropy()` (no inter-level coupling term) |
| PP entropy | `PPBlockState` is the Peixoto 2018 degree-corrected PP variant ([`planted_partition.py:34`](../../graph-tool/src/graph_tool/inference/planted_partition.py#L34)) | Zhang and Peixoto 2020 microcanonical two-rate form |
| Shuffle direction | `std::shuffle` (backward) | `SBM.UTIL.shuffle` (backward); SBM-local, not Louvain's forward shuffle |

The page kernel is intentionally a simplified faithful pedagogical port
of the canonical algorithm. The simplifications (uniform proposal, no
Hastings correction, no multilevel merges, exact entropy, random init,
nested self-consistent entropy, two-rate PP) are sanctioned divergences
documented per page in the warn callouts and verified bit-equal against
a cpp tracer that mirrors the JS form exactly. See "Verification" below.

## Walker stages (`sbm-flat-dc.html`)

Page glue at
[`page_flat_dc.js`](../../vltanh.github.io/comdet/js/sbm/page_flat_dc.js):

| Stage | What it shows | Backed by |
| --- | --- | --- |
| 0 input | 32-node fixture coloured by planted ground-truth | `COMDET.PAGE.renderFixture` |
| 1 init | Random partition over `B_0 = 8` blocks; \(\Sigma_0\) | `BlockState({ init: random8 })` |
| 2 walk | Per-vertex MH walker with full candidate panel + \(\Delta\Sigma\) per candidate | `mcmcSweep({ recordCandidates: true })` + `mountStepWalker` |
| 3 equilibrate | \(\Sigma\) and \(B\) per sweep over the full chain | `equilibrate({ sweeps: 20 })` `series` |
| 4 final | Mode partition vs. ground-truth + per-block stats | `mountFinalCompare` + `renderStatsTable` |

The init seed is exposed via the `g-seed` input plus `g-reroll` button.
Reroll re-builds the chain from scratch and rebuilds every downstream
stage.

## Determinism

Three sources of randomness, all seeded by the same MT19937 instance:

```text
const rng = COMDET.SBM.MT19937(seed >>> 0);
// shuffle the init permutation (init_makeBlockInit)
// shuffle the per-sweep visit order
// pick uniform-random target block from cands
// draw uniform sample for accept/reject when ΔΣ > 0
```

The kernel never reads `Math.random`, `Date.now`, or any other entropy
source. Same seed → identical chain → identical `traces` array.
Different seed → different shuffle and proposal sequence → different
(typically similar-quality) partition.

`BlockState.blockMembership()` returns raw block ids, not renumbered by
size. Block colour comes from `partitionColor(c)` in
[`page_helpers.js`](../../vltanh.github.io/comdet/js/comdet/page_helpers.js)
keyed off the raw id, so two runs with the same seed paint the canvas
identically.

## Output shape

`mcmcSweep` returns:

```text
{
  traces: [
    { v, fromR, toS, dS, accept, accepted, cands, candidates?, sweep? },
    ...
  ],
  accepted: number    // count of accepted moves this sweep
}
```

`equilibrate` flattens many sweeps into one trace + adds
`series: [{ sweep, S, B, accepted }, ...]`.

`NestedBlockState` returns the same shape per-level on demand via
`level(l)` and `levelMembership(l)`; `entropy()` returns the summed
multi-level \(\Sigma\).

## Verification

Three-leg byte-equal harness (cpp tracer + JS replay + JS self-RNG)
under
[`tools/viz_check/sbm/`](../../tools/viz_check/sbm/). All three legs
operate under matched `MT19937` and ported fdlibm primitives
(`jsLog`, `jsExp`, `jsLgamma`); the harness target is JS-vs-cpp tracer
bit-equality across the per-visit `(v, fromR, toS, dS, accept,
cands[i])` and per-sweep \(\Sigma\) probes. The cpp tracer is a
sanctioned fork that uses `std::mt19937` + sparse-only entropy +
uniform-proposal MH (matching the JS port); bit-equality vs. unmodified
graph-tool is not the target (graph-tool uses `pcg64_k1024` + dense
entropy + multilevel agglomerative merges, all sanctioned divergences).

| Layer | Status | Source |
| --- | --- | --- |
| L0 RNG byte stream | PASS | `diagnostic/l0_rng_raw.{cpp,mjs}` 0/100 mismatches seed=7 |
| L2 FP primitives | PASS | `diagnostic/l2_fp_primitives.{cpp,mjs}`: jsLog 0/80113, jsLgamma 0/19999; jsExp 0/200000 per prior bench |
| L3 oracle replay | PASS | `kernel_check.mjs` injects cpp's RNG outputs; JS deterministic functions match cpp deterministic functions |
| L4 self-RNG | PASS | `self_rng_check.mjs` end-to-end JS-vs-cpp bit-equal; 17-fixture 3-tier stress panel × 9 seeds × 3 flat modes = 459 cells, 8,489,232 visits, 0 mismatches |
| L4 nested | PASS | same panel × 9 seeds × {nested-dc, nested-ndc} = 306 cells, 3,475,266 visits, 0 mismatches |

Audit rows A through M and R all CLOSED:

| Row | Category | Status |
| --- | --- | --- |
| A | RNG byte stream | CLOSED (MT19937 Knuth init + Matsumoto tempering byte-equal both sides) |
| B | RNG draw count + order | CLOSED (covered transitively by self_rng L4) |
| C | RNG distribution mapping | CLOSED (rejection-mod intRange + 2^32 uniform; identical algorithm) |
| D | FP primitives | CLOSED (jsLog/jsExp/jsLgamma ported, bit-equal) |
| E | FP composition | CLOSED (sortedNonEmpty iterator order deterministic; no FMA contraction issues) |
| F | Short-circuit boundaries | CLOSED (`dS<=0` short-circuit mirrored exactly; lbinom `k==0\|\|k==n` early-return identical) |
| G | Implicit conversions | CLOSED (`Int32Array` OOB coerce to 0 mirrored; no NaN drift) |
| H | Container iteration order | CLOSED (`neList` sorted ascending before entropy/DL loops; `Bne` prefix iteration matches) |
| I | Edge cases | CLOSED (self-loop doubled; `u<=v` rule; empty/new block consistency) |
| J | Tie-break rules | CLOSED (`dS<=0` strict inclusive; no secondary tie-break tolerance) |
| K | State-snapshot inheritance | CLOSED (`e_rs` passed as snapshot at level transitions; no fresh recompute) |
| L | Per-level encounter-order | CLOSED (`neList` iteration order at `buildLevelGraph` matched) |
| M | Accumulator update order | CLOSED (`subsetSE(r,s)` operand order matched; no per-step drift via sum) |
| R | Formula composition (E convention) | CLOSED (JS uses `graph.totalEdgeWeight()` = sum of edge weights, matches cpp `g.E += w` and Peixoto Eq 23 / `get_edges_dl(B, E, g)`) |

Stress matrix (post-2026-05-12 closures): 459 flat cells PASS at
8,489,232 visits (T1 297/297 = 2,199,690; T2 108/108 = 3,800,358; T3
54/54 = 2,489,184) plus 306 nested cells PASS at 3,475,266 visits, plus
the 1,500-cell post-`addBlock` re-verification panel (1,400 nested
heap-corruption probe cells + 100 flat-{dc,pp} regression cells), all
under MT19937 with no mismatches. The cpp tracer's earlier
heap-corruption failures on small fixtures (florentine_families,
kangaroo, etc.) were closed by porting graph-tool's
`BlockState::add_block` (`graph_blockmodel.hh:912-933`) into both the
cpp tracer and the JS kernel; the per-block scalars and the \(B \times
B\) `ers` matrix now grow dynamically on every fresh-block proposal,
mirroring the canonical `get_empty_block` discipline.

### Cross-check vs. graph-tool

The three claims under the "layered-canonical" framing:

1. **JS visualizer == cpp tracer-swapped (TRACER_MODE)**: TRUE
   bit-for-bit on the stress matrix above. Trajectory + per-step
   \(\Delta\Sigma\) + final partitions byte-equal under matching seed.
2. **cpp tracer-canonical (CANONICAL_MODE) == unmodified graph-tool
   pipeline**: FALSE bit-for-bit. Sanctioned divergences listed under
   "Departures from canonical" above; the cpp tracer is a fork, not a
   binary equivalent of `minimize_blockmodel_dl`.
3. **JS visualizer == unmodified graph-tool**: FALSE bit-for-bit, same
   reasons as (2). Achievable bar is final-\(B\) and ARI similarity at
   matching seed; not bit-equality.

Σ at fixed partition cross-run determinism, JS-vs-graph-tool Σ at
fixed partition, and mode-ARI vs. ground-truth verification are listed
on the outstanding tasks below.

## Hand-check anchors

For unit-test regression on any future kernel refactor:

| Anchor | Expected |
| --- | --- |
| `K_3` (3 nodes, all in one block), DC, data-fit only (no DLs) | \(\Sigma = 0.6286 = -\log(8/15)\) per Peixoto Eq 43 closed form |
| 32-node fixture seed 7, 20 sweeps, DC | \(\Sigma \approx 220.12\), \(B = 5\) |
| 32-node fixture seed 7, 20 sweeps, NDC | \(\Sigma \approx 209.15\), \(B = 5\) |
| 32-node fixture seed 7, 20 sweeps, PP | \(\Sigma \approx 183.08\), \(B = 4\) |

PP wins on seed 7 by a decisive margin (\(\Delta\Sigma_{\text{PP - NDC}}
\approx -26\), \(\Delta\Sigma_{\text{PP - DC}} \approx -37\)). The
ranking flips under some seeds; the `sbm-flat-best` page shows the
seed-dependence directly.

## Outstanding work

- **Nested level-1+ MCMC animation**. The kernel composes the full
  multi-level chain end-to-end; the cpp tracer and JS replay leg
  exercise it bit-equal. The page UI animates level 0 only. Surfacing
  upper-level walkers in the page UI is on the follow-up.
- **PP-aware proposal distribution**. The PP variant currently uses the
  same uniform candidate pool as DC and NDC. Canonical PP biases
  proposals toward the local neighbourhood
  ([`planted_partition.py`](../../graph-tool/src/graph_tool/inference/planted_partition.py)
  `sample_block`), which converges faster on assortative networks.
- **Multilevel merge moves**. Single-vertex MH only finds the right
  partition slowly on small graphs and not at all reliably on large
  ones. The canonical
  [`multilevel_mcmc_sweep`](../../graph-tool/src/graph/inference/loops/multilevel.hh#L77)
  interleaves agglomerative merges with single-vertex moves; porting
  `merge_sweep` to JS is the next algorithmic step.
- **PCG64 RNG**. `pcg64_k1024`
  ([`graph-tool/src/graph/random.hh:24`](../../graph-tool/src/graph/random.hh#L24))
  replaces MT19937 in the canonical sampler. Bit-equivalence with
  graph-tool seeded runs requires the port.
- **Σ at fixed partition cross-check vs. graph-tool**. Requires
  graph-tool installed (`nwbench` conda env). Build identical
  `BlockState(g, b=...)` in Python plus JS and assert
  \(|\Delta\Sigma| < 10^{-9}\). Blocked on env setup.
- **Mode-partition ARI vs. ground-truth ensemble**. 10 seeds × 3
  variants × 20 sweeps. Acceptance ≥ 0.85 mean ARI.

## Paper reference

Peixoto, "Bayesian Stochastic Blockmodeling" (2017). DOI:
<https://doi.org/10.1002/9781119483298.ch11>. arXiv:
<https://arxiv.org/abs/1705.10225> (v9). Key sections: pp 8-12 (priors),
pp 14-17 (nested), pp 18-20 (degree correction), §VII.A (Bayes-factor
reading of \(\Delta\Sigma\)). The reference for every formula in
graph-tool's `BlockState`.

Karrer and Newman, "Stochastic blockmodels and community structure in
networks" (2011). DOI:
<https://doi.org/10.1103/PhysRevE.83.016107>. The DC vs. NDC
distinction with the hub-leaf failure mode on heavy-tailed graphs.

Zhang and Peixoto, "Statistical inference of assortative community
structures" (2020). DOI:
<https://doi.org/10.1103/PhysRevResearch.2.043271>. The microcanonical
two-rate PP form ported by `ppLikelihood` + `ppEdgesDl`.

Peixoto, "Hierarchical block structures and high-resolution model
selection in large networks" (2014). DOI:
<https://doi.org/10.1103/PhysRevX.4.011047>. The hierarchical
construction underlying `NestedBlockState`.

## Where to look next

- [`block_state.js`](../../vltanh.github.io/comdet/js/sbm/block_state.js):
  admin tables, entropy, virtualMove, addBlock
- [`mcmc.js`](../../vltanh.github.io/comdet/js/sbm/mcmc.js): MH sweep,
  candidatePool, equilibrate
- [`nested_state.js`](../../vltanh.github.io/comdet/js/sbm/nested_state.js):
  NestedBlockState, buildLevelGraph
- [`graph_blockmodel.hh`](../../graph-tool/src/graph/inference/blockmodel/graph_blockmodel.hh):
  canonical `BlockState` template
- [`mcmc_loop.hh`](../../graph-tool/src/graph/inference/loops/mcmc_loop.hh):
  canonical sweep body
- [`tools/viz_check/sbm/`](../../tools/viz_check/sbm/): cpp tracer + JS
  replay + diagnostic ladder

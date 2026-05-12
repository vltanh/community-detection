# Infomap

[← back to index](../algorithms.md)

Companion to [`comdet/infomap.html`](https://vltanh.me/comdet/infomap.html).

Map equation on random walks. Rosvall + Bergstrom 2008
([doi:10.1073/pnas.0706851105](https://doi.org/10.1073/pnas.0706851105),
PNAS 105(4):1118-1123). Reference implementation: the
[mapequation/infomap](https://github.com/mapequation/infomap) C++
binary, vendored into this repo at [`infomap/`](../../infomap/) at tag
`v2.9.2` / commit `cc3bf2c`.

The page at `vltanh.github.io/comdet/infomap.html` walks the 2008 paper
algorithm on the 32-node fixture; this file is the technical companion
covering both the paper algorithm and the maintained binary, with
source pointers, flag tables, and the verification grid.

## Two algorithms

Two distinct algorithms travel under the name "Infomap". They optimise
the same objective and share the same family of map-equation
quantities, but their trajectories through partition space are
structurally different.

| Aspect | 2008 paper | v2.9.2 binary |
|---|---|---|
| Initial pass | Greedy pair-join over adjacent module pairs | Multi-level Louvain-style per-vertex sweep |
| Refinement | Heat-bath simulated annealing | Alternating fine-tune + coarse-tune outer loop |
| Per-pass move | Module-pair merge | Single-node move between modules |
| RNG use | Move proposal probabilities | Per-sweep visit order, per-visit candidate order |
| Convergence | Anneal schedule cools to zero | Improvement gate (absolute and relative thresholds) |
| Hierarchy | Greedy recursive split per module | Multi-level aggregation, optional sub-module recursion |
| Bail-out | None | Dump to one module if optimised L exceeds one-module L |

The page walks the 2008 algorithm because it has natural pedagogical
staging (one merge per step, one tune visit per step). The maintained
binary is sketched in the page's stage 7 callout and verified
separately; the page does not step through it.

## Source layout (v2.9.2)

Pinned reference: vendored upstream at
[`community-detection/infomap/`](../../infomap/), tag `v2.9.2`, commit
`cc3bf2c`. Do not chase upstream `main`.

The binary lives at `community-detection/infomap/Infomap` after a
CMake build. The Python wrapper at
[`src/infomap/run_infomap.py`](../../src/infomap/run_infomap.py) is a
subprocess shell around it.

Top-level entrypoints:

| Function | File:line | Role |
|---|---|---|
| `main(argc, argv)` | `infomap/src/main.cpp:38` | CLI shell |
| `infomap::run(flags)` | `infomap/src/main.cpp:20` | Constructs `InfomapWrapper(Config(flags, true))`, calls `.run()` |
| `InfomapBase::run()` | `infomap/src/core/InfomapBase.cpp:102` | Top-level driver: read input, compute flow, loop over trials |
| `InfomapBase::run(Network&)` | `infomap/src/core/InfomapBase.cpp:161` | Per-trial driver: `removeModules`, optional `initPartition`, `runPartition`, `writeResult` on best trial |
| `InfomapBase::partition()` | `infomap/src/core/InfomapBase.cpp:1043` | Two-level outer loop: alternates `fineTune` and `coarseTune` |
| `InfomapBase::hierarchicalPartition()` | `infomap/src/core/InfomapBase.cpp:927` | Multi-level outer loop; used when `--two-level` is off |
| `InfomapBase::findTopModulesRepeatedly()` | `infomap/src/core/InfomapBase.cpp:1341` | Multi-level Louvain core: sweep, consolidate, repeat |
| `InfomapBase::fineTune()` | `infomap/src/core/InfomapBase.cpp:1390` | Project leaves to top-module labels and re-sweep |
| `InfomapBase::coarseTune()` | `infomap/src/core/InfomapBase.cpp:1428` | Run sub-Infomap per top-module to find sub-modules, then sweep at sub-module level |

Per-level optimisation:

| Function | File:line | Role |
|---|---|---|
| `InfomapOptimizer<Objective>::tryMoveEachNodeIntoBestModule` | `infomap/src/core/InfomapOptimizer.h:290` | Per-vertex sweep: visit-order shuffle, per-visit candidate enumeration, ΔL evaluation, move commit |
| `InfomapOptimizer<Objective>::consolidateModules` | `infomap/src/core/InfomapOptimizer.h:661` | Collapse modules into super-vertices, build super-edges |
| `InfomapOptimizer<Objective>::moveNodeToPredefinedModule` | `infomap/src/core/InfomapOptimizer.h:241` | Move-commit primitive used by fineTune / coarseTune projection |

Objective evaluator:

| Function | File:line | Role |
|---|---|---|
| `MapEquation<...>::getDeltaCodelengthOnMovingNode` | `infomap/src/core/MapEquation.h:197` | Closed-form ΔL for a single per-vertex move; 10 plogp evaluations |
| `MapEquation<...>::updateCodelengthOnMovingNode` | `infomap/src/core/MapEquation.h:229` | Incremental update to the four running accumulators after a committed move |
| `MapEquation<...>::calcCodelengthOnModuleOfLeafNodes` | `infomap/src/core/MapEquation.h` | Per-module within-codebook entropy contribution |
| `MapEquation<...>::calcCodelengthOnModuleOfModules` | `infomap/src/core/MapEquation.h` | Per-super-module index-codebook entropy contribution |

Flow computation:

| Function | File:line | Role |
|---|---|---|
| `calculateFlow(StateNetwork, Config)` | `infomap/src/utils/FlowCalculator.h:68` | Dispatch on `config.flowModel`; populates `node.flow`, `node.enterFlow`, `node.exitFlow` |
| `FlowCalculator::calcFlow` (undirected branch) | `infomap/src/utils/FlowCalculator.cpp:154` | `flow = degree / (2m)` for undirected unweighted graphs |
| `FlowCalculator::calcFlow` (directed branch) | `infomap/src/utils/FlowCalculator.cpp:161` | Smart-teleportation PageRank for directed graphs |

Random number generation:

| Symbol | File:line | Role |
|---|---|---|
| `using RandGen = std::mt19937` | `infomap/src/utils/Random.h:18` | 32-bit Mersenne Twister, not MT19937-64 |
| `Random::randInt(min, max)` | `infomap/src/utils/Random.h:34` | `std::uniform_int_distribution<unsigned int>` (libstdc++ Lemire's debiased multiplication for `std::mt19937`) |
| `Random::getRandomizedIndexVector` | `infomap/src/utils/Random.h:42` | Forward Fisher-Yates: at step `i`, draw `randInt(0, size-i-1)` and swap `order[i]` with `order[i + draw]` |
| Seed routing: `Config.seedToRandomNumberGenerator` → `m_rand(seed)` | `infomap/src/core/InfomapConfig.h:27` | Construct-time seeding; `--seed N` sets the field |

Math primitives:

| Symbol | File:line | Role |
|---|---|---|
| `infomath::plogp(p)` | `infomap/src/utils/infomath.h:21` | `p > 0 ? p * std::log2(p) : 0` (strict `>` short-circuit) |
| `std::log2` | libm | Only `log2` call on the kernel hot path; `log`, `exp`, `lgamma`, `pow`, `sqrt` are not used |

## CLI flags (kernel-relevant)

Registered in [`infomap/src/io/Config.cpp`](../../infomap/src/io/Config.cpp).

| Flag | Default | Effect | Site |
|---|---|---|---|
| `--seed N` | 123 (field default in `Config.h:121`; `1ul` in the CLI signature is the integer lower-bound, not a default override) | Seed for `std::mt19937` | `Config.cpp:138` |
| `--num-trials N`, `-N` | 1 | Run N trials of the whole algorithm; keep best-L; trials share one continuous RNG stream | `Config.cpp:140` |
| `--two-level`, `-2` | off | Optimise a two-level partition (no super-modules); routes through `partition()` only | `Config.cpp:91` |
| `--flow-model X`, `-f` | `undirected` | One of: `undirected`, `directed`, `undirdir`, `outdirdir`, `rawdir`, `precomputed` | `Config.cpp:95` |
| `--directed`, `-d` | off | Shorthand for `--flow-model directed` | `Config.cpp:97` |
| `--recorded-teleportation`, `-e` | off | Include teleportation in the flow used for codelength | `Config.cpp:99` |
| `--teleportation-probability X`, `-p` | 0.15 | Probability of teleport per step (directed flow models only) | `Config.cpp:105` |
| `--core-loop-limit M`, `-M` | 10 | Max sweeps per `optimizeActiveNetwork` invocation | `Config.cpp:142` |
| `--core-level-limit L`, `-L` | 0 (no limit) | Max aggregation levels in `findTopModulesRepeatedly` | `Config.cpp:144` |
| `--tune-iteration-limit T`, `-T` | 0 (no limit) | Max alternations of fineTune / coarseTune in `partition()` | `Config.cpp:146` |
| `--core-loop-codelength-threshold X` | 1e-10 | Absolute codelength threshold for `partition()`'s improvement gate | `Config.cpp:148` |
| `--tune-iteration-relative-threshold X` | 1e-5 | Relative codelength threshold (× initial L) for `partition()`'s improvement gate | `Config.cpp:150` |
| `--prefer-modular-solution` | off | Skip the final bail-out to one-module if optimised L > one-module L | `Config.cpp:154` |
| `--preferred-number-of-modules K` | 0 (off) | Bias toward K top-modules; affects `hierarchicalPartition` mainly | `Config.cpp:125` |
| `--no-self-links` | off | Drop self-loops from the input network | `Config.cpp:52` |
| `--markov-time X` | 1.0 | Scale link flow to control resolution; higher = fewer modules | `Config.cpp:115` |
| `--silent` | off | Suppress console output | `Config.cpp:160` |

Output flags:

| Flag | Default | Effect |
|---|---|---|
| `--tree` | on (auto if no other) | Write `.tree` hierarchical file |
| `--ftree` | off | Write `.ftree` (tree + aggregated inter-module flow links) |
| `--clu` | off | Write `.clu` flat-partition file |
| `--clu-level N` | -1 (bottom) | Write modules at depth N from root |
| `-o list` | (empty) | Comma-separated output formats: `clu,tree,ftree,newick,json,csv,network,states,flow` |
| `--no-file-output`, `-0` | off | Suppress all file output |

Not exposed at the CLI in v2.9.2 (API-only setters in
[`InfomapConfig.h`](../../infomap/src/core/InfomapConfig.h)):
`noCoarseTune`, `innerParallelization`, `randomizeCoreLoopLimit`,
`isCLI`, plus the inner-most accuracy thresholds
`minimumSingleNodeCodelengthImprovement` (1e-16 default).

The comdet pipeline drives the binary via
[`tools/viz_check/infomap/canonical_run.py`](../../tools/viz_check/infomap/canonical_run.py)
with the canonical command
`Infomap edge.csv outdir --seed N --silent --two-level --clu`. The
two-level mode forces the algorithm through `partition()` only, no
super-modules.

## RNG details

`std::mt19937` (32-bit Mersenne Twister), seeded once at
`InfomapConfig` construction from `Config.seedToRandomNumberGenerator`.
Between trials the binary calls `removeModules()` but not
`m_rand.seed(...)`, so the trials share one continuous RNG stream
rather than each starting from a fresh seed.

The single-threaded path is fully deterministic: same seed, same
input, same compiled binary, byte-identical output. The parallel path
(`--inner-parallelization`) is off by default and not exercised by the
comdet pipeline.

Two RNG draws per per-vertex sweep visit:

1. Visit-order shuffle at sweep start: one `getRandomizedIndexVector`
   over the active network (size N), producing N `randInt(0, k-1)`
   calls for k = N..1.
2. Per-visit candidate-module shuffle: one `getRandomizedIndexVector`
   over the candidate set (size = neighbour-module count + self +
   optionally one empty module), producing those many `randInt`
   calls. Skipped when the visit is gated out by the dirty bit or the
   first-loop guard.

The candidate-shuffle skip is critical for byte-equal verification: a
JavaScript port that draws the shuffle unconditionally diverges from
the binary's RNG stream at the first dirty-bit miss.

`std::uniform_int_distribution<unsigned int>` for `std::mt19937` (full
32-bit urngrange) uses libstdc++'s Lemire debiased multiplication
(`bits/uniform_int_dist.h:244-270`):

```
range    = max - min + 1
product  = uint64(mt()) * uint64(range)
low      = uint32(product)
if low < range:
    threshold = (-range) % range          // == 2^32 % range
    while low < threshold: redraw
ret      = uint32(product >> 32)
return min + ret
```

A faithful port must mirror Lemire bit-for-bit. The JS port at
`vltanh.github.io/comdet/js/infomap/infomap_canon.js` does this with
BigInt for the 64-bit multiply.

## Output format

`.clu`: header `# stateId moduleId flow`, one row per leaf node. The
`run_infomap.py` wrapper post-processes the `.clu` into the standard
`com.csv` format:

```
node_id,cluster_id
```

Post-processing rules:

1. Drop trivial singleton modules.
2. ASC-renumber the surviving module IDs starting from 0.
3. Sort by node id.

`.tree`: hierarchical, `path flow name id` per row, where `path` is a
colon-separated module index from root.

`.ftree`: `.tree` plus aggregated inter-module flow link records.

The comdet pipeline reads `com.csv` only; `.tree` and `.ftree` are
diagnostic outputs.

## JS-port pipelines

Two JS ports live side by side in the comdet gallery, with distinct
roles.

### Paper port: `infomap.js`

Located at
[`vltanh.github.io/comdet/js/infomap/infomap.js`](https://github.com/vltanh/vltanh.github.io/blob/main/comdet/js/infomap/infomap.js).
Loaded by the page step-walker.

Implements the 2008 paper algorithm in five deterministic stages:

1. Stationary distribution `p = d / (2m)` for undirected unweighted.
2. Singleton init: each node a module; initial codelength `L_0 = H(P) + 2`.
3. Greedy pair-joining: iteratively merge the most-negative-ΔL adjacent
   module pair; halt when no merge improves.
4. Single-node tuning: for each node, evaluate every distinct
   neighbour-module's ΔL; commit the best; sweep until idempotent.
   Greedy variant of the paper's heat-bath SA (the SA variant lives
   alongside under `saRefine` and is opt-in via `opts.refine = "sa"`).
5. Sub-level recursion: per module, recurse on the induced subgraph;
   depth-bounded at 3.

The port is order-deterministic on input order, not seeded. The SA
variant uses a seedable MT19937 for proposal weighting; the greedy
variant does not need RNG. The kernel computes the full map equation
on every candidate (O(N) per ΔL), not the closed form, because the
five-stage trajectory does not lend itself to running closed-form
accumulators across the merge phase.

### Canonical port: `infomap_canon.js`

Located at
[`vltanh.github.io/comdet/js/infomap/infomap_canon.js`](https://github.com/vltanh/vltanh.github.io/blob/main/comdet/js/infomap/infomap_canon.js).
Loaded only by the verification harnesses; not loaded by the page.

Mirrors the v2.9.2 binary's trajectory at byte-equal granularity:

- `InfomapBase::partition()` outer loop with alternating
  `fineTune` / `coarseTune` until improvement gate fails after a
  coarse-tune pass.
- `findTopModulesRepeatedly`: multi-level Louvain core with sweep,
  consolidate, repeat.
- `tryMoveEachNodeIntoBestModule`: per-vertex sweep with visit-order
  Fisher-Yates, per-visit candidate Fisher-Yates, closed-form ΔL,
  strongest-connected tie-break, singleton-companion move, dirty bit,
  first-loop guard, empty-module candidate.
- `MapEquation::getDeltaCodelengthOnMovingNode` /
  `updateCodelengthOnMovingNode` closed forms.

RNG primitives: `COMDET.COMMON.MT19937` plus a libstdc++-style
`uniformIntDist` rejection sampler matching Lemire's debiased
multiplication. `plogp` routes through `jsLog2(x) = x === 1 ? 0 :
Math.log(x) * Math.LOG2E` to match the binary's `std::log2` bit-for-bit
on the kernel hot path (V8 `Math.log2` and glibc `std::log2` drift by
one ulp on roughly one in a hundred thousand inputs, verified at
`tools/viz_check/infomap/L2_log2/`).

### Divergences (paper algorithm vs v2.9.2 binary)

The two algorithms target the same objective but trace different
paths through partition space. Enforced invariants the canonical port
must match:

| # | Divergence | Site |
|---|---|---|
| D1 | Multi-level Louvain core in place of pair-join | `InfomapBase::findTopModulesRepeatedly` at line 1341, `InfomapOptimizer::consolidateModules` at line 661 |
| D2 | Alternating fineTune / coarseTune outer loop | `InfomapBase::partition` at line 1043 |
| D3 | Recursive sub-Infomap per top-module inside coarseTune | `InfomapBase::coarseTune` at line 1428 |
| D4 | Strongest-connected tie-break overrides pure ΔL argmin within 1e-16 tolerance | `InfomapOptimizer.h:401` |
| D5 | Singleton-companion move: drag along the lone remaining member of an emptied old module | `InfomapOptimizer.h:445` |
| D6 | First-loop guard: refuse to move from a multi-member module on sweep 0 of iteration 0 | `InfomapOptimizer.h:310` |
| D7 | Empty-module candidate added once per visit when applicable | `InfomapOptimizer.h:338` and `:248` |
| D8 | Two RNG-driven shuffles per visit: visit-order plus per-visit candidate-order | `InfomapOptimizer.h:295` and `:369` |
| D9 | Dirty bit skips nodes whose neighbourhood has not changed since last sweep | `InfomapOptimizer.h:306` |
| D10 | One-module bail-out at end of `partition()` if optimised L > one-module L | `InfomapBase.cpp:1109` |
| D11 | `restoreConsolidatedOptimizationPointIfNoImprovement` reverts optimiser state when a sweep did not improve | `InfomapOptimizer.h:750` |
| D12 | `m_isCoarseTune` sets `loopLimit = 20` instead of the default 10 | `InfomapOptimizer.h:271` |
| D13 | Improvement gate uses both absolute and relative thresholds combined with `&&` | `InfomapBase.cpp:1091` |
| D14 | Tie-tolerance in argmin uses `minimumSingleNodeCodelengthImprovement = 1e-16` | `InfomapOptimizer.h:387` |
| D15 | Outer loop terminates only after a coarse-tune pass without improvement, not on any phase | `InfomapBase.cpp:1094` |

A paper-faithful JS port reaches ARI roughly 0.29 vs the binary on
the dnc network; reproducing all fifteen divergences in the same
arithmetic order is what closes the gap to byte-equal.

## fineTune / coarseTune contract

`fineTune()` at `InfomapBase.cpp:1390`:

```
assert numLevels() == 2
setActiveNetworkFromLeafs()
initPartition()                                       # one module per leaf
moduleIndex = 0
for module in m_root: module.index = moduleIndex++
modules = [leaf.parent.index for leaf in m_leafNodes]
moveActiveNodesToPredefinedModules(modules)           # collapse leaves to top-module labels
numEffectiveLoops = optimizeActiveNetwork()           # re-sweep at leaf level
if numEffectiveLoops == 0:
    restoreConsolidatedOptimizationPointIfNoImprovement()
else:
    root().replaceChildrenWithGrandChildren()
    consolidateModules(false)
return numEffectiveLoops
```

Effect: project every leaf back to its top-module label, re-run the
per-vertex sweep on the leaf graph. If any leaf moved, consolidate the
new partition unconditionally; if no leaf moved, restore the previous
partition state.

`coarseTune()` at `InfomapBase.cpp:1428`:

```
assert numLevels() == 2
moduleIndexOffset = 0
for node in m_root:
    if node.childDegree() < 2:
        for child in node: child.index = moduleIndexOffset
        ++moduleIndexOffset
        continue
    subInfomap = getSubInfomap(node).setTwoLevel(true).setTuneIterationLimit(1)
    subInfomap.initNetwork(node).run()
    originalLeafIt = node.begin_child()
    for subLeaf in subInfomap.leafNodes():
        originalLeafIt->index = subLeaf.index + moduleIndexOffset
        ++originalLeafIt
    moduleIndexOffset += subInfomap.numTopModules()
    node.disposeInfomap()

subModules = [leaf.index for leaf in m_leafNodes]
setActiveNetworkFromLeafs(); initPartition()
moveActiveNodesToPredefinedModules(subModules)
consolidateModules(true)

modules = [subModule.index for subModule in m_root]
setActiveNetworkFromChildrenOfRoot(); initPartition()
moveActiveNodesToPredefinedModules(modules)
numEffectiveLoops = optimizeActiveNetwork()
consolidateModules(true)
return numEffectiveLoops
```

Effect: for each top-module of size ≥ 2, run a fresh sub-Infomap
restricted to the module's induced sub-graph with `tuneIterationLimit
= 1` (so the sub-Infomap does only one outer iteration). Re-label
every leaf with its sub-module index. Consolidate to a sub-module
tree, then sweep at the sub-module level to let sub-modules find
better top-module labels. Returns the number of effective sub-module
moves.

`partition()` outer loop at `InfomapBase.cpp:1043`:

```
m_tuneIterationIndex = 0
findTopModulesRepeatedly(levelAggregationLimit)
oldCodelength = newCodelength = getCodelength()
doFineTune = true
coarseTuned = false
while numTopModules() > 1 and (m_tuneIterationIndex+1) != tuneIterationLimit:
    ++m_tuneIterationIndex
    if doFineTune:
        numEffectiveLoops = fineTune()
        if numEffectiveLoops > 0:
            findTopModulesRepeatedly(levelAggregationLimit)
    else:
        coarseTuned = true
        if not noCoarseTune:
            numEffectiveLoops = coarseTune()
            if numEffectiveLoops > 0:
                findTopModulesRepeatedly(levelAggregationLimit)
    newCodelength = getCodelength()
    isImprovement = (newCodelength <= oldCodelength - minimumCodelengthImprovement)
                 && (newCodelength <  oldCodelength - initialCodelength * minimumRelativeTuneIterationImprovement)
    if not isImprovement:
        if coarseTuned: break
    else:
        oldCodelength = newCodelength
    doFineTune = !doFineTune

if (!preferModularSolution and preferredNumberOfModules == 0
    and haveNonTrivialModules() and getCodelength() > getOneLevelCodelength()):
    root().replaceChildrenWithOneNode()
    m_hierarchicalCodelength = getOneLevelCodelength()
```

The outer loop alternates `fineTune` and `coarseTune`. It terminates
when neither pass strictly improves L (both absolute and relative
gates fail) AND at least one coarse-tune has been tried (`coarseTuned
== true`). The break on no-improvement happens only after a
coarse-tune pass, on the reasoning that coarse-tune is the deeper of
the two refinements.

`levelAggregationLimit = 0` and `tuneIterationLimit = 0` (the defaults)
behave as "no limit" because the comparison `(idx + 1) != 0` is always
true for unsigned arithmetic.

## Verification matrix

| Layer | Status |
|---|---|
| L0 RNG raw stream | PASS, 9 seeds × 5000 outputs bit-equal. `tools/viz_check/infomap/L0_rng_raw/`. cpp `std::mt19937` matches `COMDET.COMMON.MT19937`. |
| L1 uniform_int_distribution | PASS, 9 seeds × 100k draws bit-equal. `tools/viz_check/infomap/L1_uniform_int/`. libstdc++ for `std::mt19937` (UINT32_MAX urngrange) routes through Lemire's debiased multiplication; `infomap_canon.js` mirrors. |
| L2 jsLog2 sweep | PASS, 9 seeds × 100k inputs, 0 mismatches. `tools/viz_check/infomap/L2_log2/`. cpp tracer routes plogp through `jsLog2(x) = x == 1 ? 0 : jsLog(x) * 1.4426950408889634`, not `std::log2`. |
| L3 oracle replay | PASS. `kernel_check.{py,mjs}` per-call `tracer == js_replay`. Deterministic side bit-equal. |
| L4 self-RNG 3-tier panel | PASS 153/153 cells, 23.7M cumulative visits, 11.5M decisions, 3.1M moves, 0 trajectory mismatches. 17 fixtures × 9 seeds across T1+T2+T3, n=800..23133. |
| L4 self-RNG bumped 3-tier (T1, post-fix 2026-05-12) | PASS 5850/5850 cells, 33.9M visits, 17.8M decisions, 5.6M moves, 0 mismatches. 117 nets × 50 seeds. |

Strict-fail criteria across L4: per-visit identity (`v`, `moved`,
`newM`), per-move flow updates (`oMf0`, `nMf0`, `oMf1`, `nMf1`, `vnf`),
per-visit decision flags (`bM`, `sPick`, `nLnk`, `ppV`, `ppOM`, `ppT`),
per-call summaries (`nMoved`, `L_post`, `partition_end`), visit and
call count parity. Sub-ulp residuals on transient accumulators are
tracked informationally; they do not propagate into trajectory.

## Determinism notes

The binary's single-threaded path is fully deterministic under fixed
seed and fixed input. Three knobs to mind:

1. `--inner-parallelization` enables OpenMP threads inside the
   per-vertex sweep; off by default. With it on, the visit order
   becomes non-deterministic and byte-equal verification fails.
   Comdet does not enable this.
2. CMake default is `-O2` with `-ffp-contract=fast` on gcc 13+, which
   fuses `a*b + c` into FMA with single rounding. The tracer build
   adds `-ffp-contract=off` to forbid fusion and match V8's
   double-rounding. The shipped binary uses the default; the byte-equal
   port matches the tracer build's arithmetic, not the shipped
   binary's.
3. Multi-trial mode (`--num-trials N`) is one continuous RNG stream
   subdivided across N trials, not N independent seeds. Two-trial runs
   are NOT equivalent to running with `--seed N` and `--seed N+1`
   separately and picking the best.

## Modularity vs map equation (paper Fig. 2 contrast)

Paper §"Mapping Flow Compared with Maximizing Modularity" presents two
designed graphs (paper Fig. 2). The first is a directed-flow graph
where the map equation finds four flow-trap clusters (L ≈ 2.67 bits
per step) while modularity stuffs everything into two coarse blocks
(Q ≈ 0.50). The second is a no-flow graph where the map equation
collapses to one module (L ≈ 2.73) while modularity again picks the
topological grouping (Q ≈ 0.56). The disagreement is real: modularity
is flow-independent (counts edges and degree products); the map
equation is flow-dependent (counts steps and transitions).

On undirected unweighted graphs the disagreement narrows because the
flow distribution becomes a function of the degree sequence. The
32-node comdet fixture is mild enough that Leiden-Mod, Leiden-CPM,
and Infomap roughly agree on A's K_5, B's K_4 split, and C's two
halves; they disagree on what to do with A's periphery and the
outliers.

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
- Kawamoto T, Rosvall M. "Estimating the resolution limit of the map
  equation in community detection." PRE 91:012809 (2015). Resolution
  bound: smallest detectable module size of order \(\sqrt{n / \bar{k}}\).
- Schaub MT, Lambiotte R, Barahona M. "Encoding dynamics for
  multiscale community detection: Markov time sweeping for the map
  equation." PRE 86:026112 (2012). Multi-scale extension via Markov
  time.

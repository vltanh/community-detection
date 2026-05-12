# Louvain — technical reference

Companion to [`comdet/louvain.html`](https://vltanh.me/comdet/louvain.html).
The page explains what the algorithm does in plain English; this file
holds the implementation detail and source-code pointers.

## Provenance

- Paper: Blondel, Guillaume, Lambiotte, Lefebvre. "Fast unfolding of
  communities in large networks." J. Stat. Mech. (2008) P10008.
  DOI: <https://doi.org/10.1088/1742-5468/2008/10/P10008>
- Local PDF: `~/Downloads/Research/Blondel_2008_J._Stat._Mech._2008_P10008.pdf`
- Canonical C++ implementation: gen-louvain v0.3 (Campigotto, Conde Céspedes,
  Guillaume; July 2013), shipped verbatim at
  [`community-detection/externals/louvain/`](../../externals/louvain/).
  The 2013 fork extends the 2008 reference with nine additional quality
  functions; the Newman-Girvan modularity (the only objective the JS port
  exposes) is the v0.3 default.
- JS port: [`community-detection/vltanh.github.io/comdet/js/louvain/louvain.js`](../../vltanh.github.io/comdet/js/louvain/louvain.js)
  (694 LOC; consumes shared primitives from
  [`comdet/js/common/common.js`](../../vltanh.github.io/comdet/js/common/common.js)).

## Three canonical layers

```
run_louvain.py  (Python wrapper)
   └── env LD_PRELOAD=louvain_seed_preload.so + LOUVAIN_SEED=<n>
        ├── convert  binary  (text edge list → binary graph + relabel.txt)
        ├── louvain  binary  (gen-louvain main_louvain.cpp; reads .bin,
        │                     runs Louvain::one_level until no improvement,
        │                     writes hierarchy tree to stdout)
        └── hierarchy binary (reads tree, emits node→comm partition at
                              deepest level)
```

| Layer | Path |
|---|---|
| Python wrapper | [`src/louvain/run_louvain.py`](../../src/louvain/run_louvain.py) |
| Dispatcher    | [`src/louvain/pipeline.sh`](../../src/louvain/pipeline.sh) |
| LD_PRELOAD shim | [`src/louvain/seed_shim/louvain_seed_preload.c`](../../src/louvain/seed_shim/louvain_seed_preload.c) |
| Binary build | [`externals/louvain/Makefile`](../../externals/louvain/Makefile) |
| Driver (Phase 1 + Phase 2 loop) | [`externals/louvain/src/main_louvain.cpp:243-320`](../../externals/louvain/src/main_louvain.cpp) |
| Per-level sweep | [`externals/louvain/src/louvain.cpp:213-280`](../../externals/louvain/src/louvain.cpp) (`Louvain::one_level`) |
| Quality (modularity) | [`externals/louvain/src/modularity.cpp`](../../externals/louvain/src/modularity.cpp) + [`modularity.h`](../../externals/louvain/src/modularity.h) |
| Graph storage | [`externals/louvain/src/graph_binary.{h,cpp}`](../../externals/louvain/src/graph_binary.h) |
| JS port (algo-only) | [`vltanh.github.io/comdet/js/louvain/louvain.js`](../../vltanh.github.io/comdet/js/louvain/louvain.js) |
| JS shared primitives | [`vltanh.github.io/comdet/js/common/common.js`](../../vltanh.github.io/comdet/js/common/common.js) |
| Page glue | [`vltanh.github.io/comdet/js/louvain/page.js`](../../vltanh.github.io/comdet/js/louvain/page.js) |

The Python wrapper drives `convert → louvain → hierarchy`, parses the
deepest level partition, inverts the relabel file, drops singleton
clusters, renumbers cluster IDs 0..K-1 in sorted-unique-comm-ASC order,
and sorts the output by `node_id` ASC.

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

After admin-table substitution (Modularity::in[c] = 2·intra_c + Σ self-loops;
Modularity::tot[c] = Σ weighted_degree of constituents), the candidate-gain
form Louvain's inner loop evaluates collapses to:

\[
\text{gain}(i, C) = k_{i,C} - \Sigma_{tot}(C) \cdot k_i / 2m
\]

This is the post-remove gain at [`modularity.h:80-88`](../../externals/louvain/src/modularity.h),
mirrored on the JS side by `modGain` in [`louvain.js:273-276`](../../vltanh.github.io/comdet/js/louvain/louvain.js).
The maximum is picked by strict `>` comparison against a running best
initialised at zero; ties resolve to the candidate encountered earliest in
adjacency-iteration order.

## CLI flags

The shipping `louvain` binary at
[`externals/louvain/src/main_louvain.cpp:115-167`](../../externals/louvain/src/main_louvain.cpp)
exposes:

| Flag | Type | Default | Effect |
|---|---|---|---|
| `-q id` | int 0..9 | 0 (mod) | Quality function identifier |
| `-c alpha` | float | 0.5 | Owsinski-Zadrozny α (q=2 only) |
| `-k kmin` | int | 1 | Shi-Malik κ_min (q=8 only) |
| `-w file` | path | — | Read graph as weighted |
| `-p file` | path | — | Start from a given partition |
| `-e eps` | float | 1e-6 | Per-pass convergence threshold |
| `-l L` | int | -2 | Display level (-1 = hierarchy) |
| `-v` | flag | off | Verbose timing + level info |

Quality identifier decoding (used by [`run_louvain.py:32-43`](../../src/louvain/run_louvain.py)
and [`run_cd.sh`](../../src/run_cd.sh)):

| `--quality` | `id_qual` | Notes |
|---|---|---|
| `louvain-mod` | 0 | Newman-Girvan modularity (default) |
| `louvain-zahn` | 1 | Zahn-Condorcet |
| `louvain-owzad-<α>` | 2 | Owsinski-Zadrozny, α via `-c` |
| `louvain-goldberg` | 3 | Goldberg density |
| `louvain-condora` | 4 | A-weighted Condorcet |
| `louvain-devind` | 5 | Deviation to Indetermination |
| `louvain-devuni` | 6 | Deviation to Uniformity |
| `louvain-dp` | 7 | Profile Difference |
| `louvain-shimalik-<k>` | 8 | Shi-Malik, κ_min via `-k` |
| `louvain-balmod` | 9 | Balanced Modularity |

The JS port exposes only `mod` (q=0). The other nine variants are
reachable through the canonical pipeline; each is a separate Quality
class wired into the same Phase 1 + Phase 2 driver at
[`init_quality` in main_louvain.cpp:179-238](../../externals/louvain/src/main_louvain.cpp).

## Algorithm

Two phases per pass, iterated until modularity stops increasing.

### Phase 1 — modularity sweep

Per-level loop (matches `Louvain::one_level` at
[`louvain.cpp:213-280`](../../externals/louvain/src/louvain.cpp) and
`phase1` in [`louvain.js:536-618`](../../vltanh.github.io/comdet/js/louvain/louvain.js)):

```text
init: every node in its own community
shuffle node order via Fisher-Yates (one shuffle per level, reused across passes)
do:
  cur_qual = quality()
  nb_moves = 0
  for each v in shuffled order:
    neigh_comm(v)              # fill neigh_pos[0..neigh_last]
    remove(v from vComm)
    pick best c by strict > over neigh_pos
    insert(v into best_c)
    if best_c != vComm: nb_moves += 1
  new_qual = quality()
while nb_moves > 0 and new_qual - cur_qual > eps_impr  (eps_impr = 1e-6)
```

Differences from Leiden's `moveNodes`:

- No FIFO queue. Every node is revisited every sweep until the whole graph
  quiets down. Leiden's `moveNodes` revisits only neighbours of moved
  nodes, so cost grows with moves performed rather than nodes times passes.
- No "consider empty community" option. Louvain only considers neighbours'
  communities plus the node's own.
- Strict positive acceptance (`ΔQ > 0`). Leiden uses `> 10·ε`, equivalent
  on graphs that are not pathologically scaled.

### Phase 2 — aggregation

Pseudo-code (matches `Louvain::partition2graph_binary` at
[`louvain.cpp:147-211`](../../externals/louvain/src/louvain.cpp) and
`Graph.collapse` at
[`common.js:286-356`](../../vltanh.github.io/comdet/js/common/common.js)):

```text
super-graph: one node per surviving community in Phase 1 output,
             ids assigned by original-community-id ASC
super-edges: weight = sum of original edge weights between communities
intra-community edges: fold into a self-loop on the super-node;
                       weight = 2 * intra_c (per-direction iteration in
                       canonical adj walk)
re-run Phase 1 on super-graph
```

The self-loop weight at the super-node is `2 * intra_c`, not `intra_c`.
The factor of two comes from the canonical adjacency layout: each
non-self edge appears in both endpoint adjacency lists, and the
collapse iterates each constituent's adjacency once. The current
JS port mirrors this convention (see
[`common.js:316-339`](../../vltanh.github.io/comdet/js/common/common.js)).
A separate `collapseLeiden` path on the same Graph object emits the
igraph-style layout (self-loop weight `intra_c`, halved during the walk
to compensate for igraph's twice-listed self-loop entries) for the Leiden
port.

### Outer loop

```text
collapsedG = original
loop:
  P = phase1(collapsedG)
  P.renumber()                 # by original-id ASC (canonical partition2graph_binary)
  newCollapsed = collapsedG.collapse(P.membership, P.ncomm)
  if newCollapsed.vcount >= collapsedG.vcount: break
  if newCollapsed.vcount <= 1: break
  collapsedG = newCollapsed
return P projected back onto original (via composed fineMembership)
```

The exit condition `newCollapsed.vcount >= collapsedG.vcount` fires when
Phase 1 produced no merges. The `vcount <= 1` exit is a safety net that
matches the canonical's behaviour on graphs the algorithm has fully
collapsed.

## Reproducibility

Three RNG seeding paths matter:

| Layer | Source | Determinism contract |
|---|---|---|
| `main_louvain.cpp:245` | `srand(time(NULL)+getpid())` | Non-reproducible by default |
| LD_PRELOAD shim | `LOUVAIN_SEED=<n>` overrides the seed argument | Reproducible across runs |
| JS port | seeded Mersenne Twister (MT19937) at `LOUVAIN.run(graph, qfn, seed)` | Reproducible across runs |

The canonical pipeline (libc rand + long double) does NOT produce the
same partition as the JS port under matching numeric seed. The two
families generate different shuffles. The L4 verification leg (see
"Verification" below) substitutes both pieces in a parallel C++ build so
the JS port can be compared bit-for-bit against a JS-shape canonical.

| Knob | Default | Effect |
|---|---|---|
| `seed` | 42 (JS page) | Initial state of MT19937; controls per-level shuffle order |
| `eps_impr` | 1e-6 | Per-pass convergence threshold |

Earlier JS builds carried JS-only safety caps (`pass > 50` per level,
`level > 30` overall) that diverged from the canonical's unbounded loops
on large graphs (one empirical test, `google` with 15763 nodes, needed 69
L0 passes to converge). Both caps were removed for byte-equal parity;
the page now mirrors the canonical's unbounded loops exactly.

## Output shape

`COMDET.LOUVAIN.run(graph, qfn, seed, opts)` returns:

```text
{
  partition: Partition,        // final partition over original graph
  quality:   number,           // Q at the deepest level
  levels: [{
    level: 0,1,2,...,
    sweeps: [
      { nbMoves, totalImprov, qualityAfter, curQual,
        traces: [{ v, fromComm, toComm, moved, delta, candidates,
                   kv, selfLoop, dncBest, inCfromPre, totCfromPre,
                   inCfrom, inCto, totCfrom, totCto }] }
    ],
    collapsedVcountBefore: N,
    collapsedNcomm: K,
    finePost:        Int32Array,  // membership over original graph at this level
    newCollapsedVcount: K' = collapsedG.collapse(...).vcount(),
    totalWeightPre:  Float64,
    totalWeightPost: Float64,
    nAfterCollapse:  K',
    inBitsEntry:     Float64Array,  // pre-sweep in_/tot_ snapshot
    totBitsEntry:    Float64Array,
    inBitsExit:      Float64Array,  // post-sweep, pre-renumber snapshot
    totBitsExit:     Float64Array
  }, ...]
}
```

The page walker steps through `levels[0].sweeps[*].traces[*]` linearly;
the level table aggregates by level. The per-visit probe fields
(`kv`, `selfLoop`, `dncBest`, `inCfromPre`, `totCfromPre`, plus the
per-candidate `{ comm, dnc, gain, delta }` entries) feed the L4 bit-equal
tracer; consumers that do not need them can ignore.

The canonical pipeline emits `com.csv` per `run_cd.sh` contract:

```text
node_id,cluster_id
0,0
1,0
...
```

- Singletons dropped (per `pipeline_common.drop_singleton_clusters`).
- `cluster_id` contiguous 0..K-1 in sorted-unique-comm-ASC order.
- Rows sorted by `node_id` ASC.

## Behaviour on the comdet 32-node fixture

Approximate level table on seed 42 (the page recomputes the exact values
from the kernel on each render):

| Level | vc-before | ncomm | vc-after | sweeps | moves | Δ Q | Q after |
|---|---|---|---|---|---|---|---|
| 0 | 32 | ~8 | ~8 | 5 | ~32 | +0.616 | ~0.616 |
| 1 | ~8 | ~4 | ~4 | 2 | ~4 | +0.047 | ~0.663 |
| 2 | ~4 | ~3 | ~3 | 2 | ~1 | +0.006 | ~0.669 |
| 3 | ~3 | 1 | 1 | 2 | ~2 | +0.026 | ~0.695 |

Approximate final Q at the deepest level: 0.70 (varies slightly with
seed). The page reports the deepest-level output, which collapses to a
single community on this fixture (textbook modularity-resolution-limit
behaviour). Level 0 is the more meaningful community structure; the
canonical pipeline exposes any level via `hierarchy -l <L>`, the JS
walker steps through every level's trace in stage 4.

## Internally disconnected communities

Louvain's connectivity behaviour matches the paper: a community can
become internally disconnected when a bridge node moves out (Traag et al.
2019, Fig. 2). The page calls `disconnectedComms(membership)` on the
final partition to detect any such case; on the 32-node fixture none
survive at the deepest level (a single community is trivially connected).
On larger real-world networks the rate runs 1 to 17% per Traag et al.
2019. The Leiden walker on
[`leiden-cpm.html`](https://vltanh.me/comdet/leiden-cpm.html) recreates
the paper's Fig. 2 case directly.

## Verification

Three claims, three statuses:

1. **JS visualiser == JS-mirroring C++ tracer (swapped build)** — bit-for-bit
   under matching seed. Verified by L4 self-RNG end-to-end. Most recent
   pass: 17 fixtures × 9 seeds (3-tier panel) + 6 inputs × 9 seeds
   (synthetic) = 13.4M cumulative per-visit records, 0 bit divergences.
   Probes: per-visit `{level, pass, visit, v, fromComm, toComm, moved}`
   integer-equal + uint64-reinterpret-equal `{dSbits, dGainBits,
   inCfromBits, inCtoBits, totCfromBits, totCtoBits}` + per-pass
   `qualityAfter` + per-level `totalWeightPre/Post` + composed
   `fineMembership` integer-equal + `Q_final` uint64-reinterpret-equal.
2. **Canonical-faithful C++ tracer (canonical build) == unmodified
   canonical pipeline** — bit-for-bit. Verified by the pre-existing
   3-leg move-apply tracer at
   [`tools/viz_check/louvain/`](../../tools/viz_check/louvain/).
3. **JS visualiser == unmodified canonical pipeline** — false under
   matching numeric seed. Two fundamental divergences prevent bit
   equality: RNG family (libc rand vs MT19937) and FP precision (long
   double vs Float64). Both implementations produce valid Blondel
   Louvain partitions; the trajectories differ.

Verification grid + per-row audit status:
[`community-detection/louvain/audit.md`](../../../.claude/projects/-home-vltanh-Documents-netsci-research/memory/community-detection/louvain/audit.md)
(memory file).

Cumulative verification across the L4 panel: 13,423,264 per-visit
records, 0 bit divergences (mod q=0). The empirical 161-network × 50-seed
sweep (415M visits) was run on the prior tracer build and is not
re-verified after the post-2026-05-07 canonical-anchored rework; the
13.4M cumulative sample over the 3-tier panel and synthetic matrix is
sufficient to re-confirm bit equality at well above the 1M target.

## Audit drift notes (current as of 2026-05-12)

The audit memory has two stale claims that the current code no longer
matches; both are documented here for future reference:

1. The memory `codebase_map.md` section "KEY DIVERGENCE FROM CANONICAL —
   five places" lists five places where the JS port diverged from the
   canonical pipeline. After the 2026-05-07 canonical-anchored rework
   (commit `7a14411` on the gallery + `aef993b` on the CD repo), three
   of those five divergences have been eliminated: the JS port now
   uses canonical self-loop strength convention (`weighted_degree`
   counts self-loop once), canonical collapse self-loop weight
   (`2 * intra_c`), and canonical `Modularity::quality` operand order.
   Only the RNG family (MT19937 vs libc rand) and FP precision (Float64
   vs long double) remain. The current audit grid in `audit.md` post-dates
   the rework and is correct.
2. The prior docs/algorithms/louvain.md (pre-this-rewrite) stated that
   the Louvain collapse "halves self-loop weights on undirected graphs,
   matching the `igraph_create` storage convention used by libleidenalg".
   That claim describes the `collapseLeiden` path used by the Leiden
   port, not the `collapse` path used by Louvain. The Louvain path emits
   `2 * intra_c` self-loops, as the canonical gen-louvain reference does
   via per-direction adjacency iteration. The current document text
   reflects the as-shipped behaviour.

## Where to look next

- [`externals/louvain/src/louvain.cpp:213-280`](../../externals/louvain/src/louvain.cpp): per-level sweep
- [`externals/louvain/src/main_louvain.cpp:243-320`](../../externals/louvain/src/main_louvain.cpp): outer driver
- [`vltanh.github.io/comdet/js/louvain/louvain.js`](../../vltanh.github.io/comdet/js/louvain/louvain.js): JS port
- [`src/louvain/run_louvain.py`](../../src/louvain/run_louvain.py): canonical pipeline wrapper
- [`tools/viz_check/louvain/`](../../tools/viz_check/louvain/): verification harness
- [Interactive walkthrough: vltanh.me/comdet/louvain.html](https://vltanh.me/comdet/louvain.html)
- [Leiden-CPM page](https://vltanh.me/comdet/leiden-cpm.html): the modern replacement with refinement
- [Leiden-Mod page](https://vltanh.me/comdet/leiden-mod.html): same Leiden kernel under the modularity objective

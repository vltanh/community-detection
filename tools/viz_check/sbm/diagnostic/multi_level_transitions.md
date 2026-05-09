# Multi-level transition checklist (nested SBM)

Per byte-equal-tracer skill / "Multi-level flag": walk the audit grid at
every level transition, not just at level 0.

## Concrete pipeline

cpp tracer (`flat_traced.cpp` runNested) emits per-level state in the
trace JSON:

| Field | Source | What it tells you |
|-------|--------|-------------------|
| `level_S_init[l]`     | `runNested` post-rebuild | level-l entropy at the auto-hierarchy init partition, BEFORE level-l mcmc starts |
| `level_S_final[l]`    | end of run              | level-l entropy at the run's last `level-l` mcmc sweep |
| `level_Bne_init[l]`   | post-rebuild            | level-l initial non-empty block count |
| `level_final_membership[l]` | end of run         | level-l terminal membership |
| `sweeps[l][s]`        | per-sweep dump          | full per-visit trace at level l, sweep s |

JS replay (`kernel_check.mjs` nested branch) loads the trace, builds a
parallel `NestedBlockState`, applies the cpp tracer's per-visit
`(toS, accept)` via `proposalOracle` + `visitOrder`, and checks:

1. JS `state.entropy()` matches cpp `level_S_init[l]` after rebuild
   (bit-equal target).
2. Each sweep's `S_post` matches cpp's reported value.
3. Final per-level membership matches.

## Per-transition checklist (filled in for current SBM port)

| Concern | Audit row | Concrete site (cpp / JS) | Status |
|---------|-----------|---------------------------|--------|
| Re-indexing order at consolidate | H + L | cpp `buildLevelGraph` (flat_traced.cpp:741); JS `buildLevelGraph` (nested_state.js) | CLOSED — both use first-occurrence in level-l membership iteration |
| Aggregation sum order | H + E | cpp loops over level-l edge records; JS `KEY_MUL=2^20` pair-encoding map | CLOSED — both walk in input edge-record order; sum order deterministic |
| Convention shift at level boundary | I | self-loops emerge from intra-block aggregation (level-1+ has self-loops even when level-0 doesn't) | CLOSED — cpp adds 2.0 to e_rr for self-loop, JS line 81 doubles diagonal |
| Vertex / element weight propagation | G | level-(l+1) vertex weight = sum of level-l block sizes inside that super-vertex | NOT-USED — port doesn't track vertex weights (defaults to 1) |
| State-snapshot inheritance | K | each level's BlockState rebuilds e_rs from scratch from parent membership; no running tracker passed | CLOSED — both rebuild via `rebuildFromMembership` at level construction |
| Aliased admin storage | G + algo | e_rs symmetric off-diagonal: `ers[r*B+s] = ers[s*B+r]`; both sides assert at write time | CLOSED — cpp:467 + JS:78 keep symmetry under same `if (r != s)` guard |

## Open finding (2026-05-09)

**JS `edgesDl` uses `E = graph.totalWeight() = 2 * num_edges_actual`** while
cpp tracer uses `E = num_edges_actual`. Per Peixoto 2017 Eq 23 +
graph-tool's `get_edges_dl(B, E, g)` source, `E` is the actual edge
count (not stub-pair count), so cpp is correct.

Drift: ~403 nats on dnc (n=906, E=10429), ~9 nats on fixture32
(n=32, E=80). Final per-level memberships match (algorithmic trajectory
unchanged); only absolute Σ values diverge.

This is a level-0 bug; under the auto-hierarchy nested run it
propagates into every level-l > 0 BlockState constructor as well, since
each level's `E = level_graph.totalWeight()`. Audit table A-M closure
on level-1+ inherits the same residual.

## Re-run recipe

```sh
cd community-detection
bash tools/viz_check/sbm/instrumented/build.sh
python tools/viz_check/sbm/kernel_check.py --variant=nested-dc --seed=7 \
    --sweeps=10 --no-canonical-check
# Expect: cpp tracer levels=4; JS replay reports per-level S drift ~403
# (level 0) decaying with level depth as Bne shrinks. Final per-level
# membership match (memDiffs=0).
```

# IKC trace schema (gold-standard byte-equal pipeline)

Contract between canonical-tracer (Python instrumented fork of `run_ikc.py`) and JS-tracer (`ikc.js` + harness hooks). Both sides emit JSON of this exact shape, lockstep per-step. Diff harness walks records via uint64 reinterpret on numerics + array equality on member lists.

## Top-level

```jsonc
{
  "fixture": "<basename>",            // e.g. "fixture32", "dnc", "copenhagen_fb_friends"
  "k_floor": <int>,
  "n_orig": <int>,                    // before format_graph
  "m_orig": <int>,                    // edge count after self-loop removal
  "node_id_map": [                    // pd.unique encounter order; index = compact id
    {"orig": "<string>", "compact": <int>}, ...
  ],
  "iters": [<iter>, ...],
  "final": <final>
}
```

`orig` is always emitted as a string (canonical reads CSV as object dtype; JS reads as string). Diff harness compares string-equal.

## Per-iter record

```jsonc
{
  "iter": <int>,                                  // 0-indexed pass number
  "residual_n_before": <int>,                     // graph.numberOfNodes() at iter top
  "residual_m_before": <int>,                     // graph.numberOfEdges()
  "residual_compact_ids_sorted": [<int>, ...],    // ASC by compact-post-prev-compaction id
  "max_k": <int>,                                 // kc.maxCoreNumber()
  "core_numbers_sorted": [                        // [(compact_id, core_number), ...] ASC by compact_id
    [<int>, <int>], ...
  ],
  "kcore_n": <int>,                               // |kcore subgraph nodes|
  "kcore_m": <int>,                               // |kcore subgraph edges| (no self-loops)
  "kcore_compact_ids_sorted": [<int>, ...],
  "components": [<component>, ...],               // canonical iteration order = NetworKit getComponents()
  "nodes_to_remove_sorted": [<int>, ...],         // ASC; union of all peeled nodes this iter
  "kept_clusters_this_iter": <int>,
  "bail_L": <int> | null,                         // G8: full-graph edge count L echoed at bail iter; null on non-bail
  "bail_per_node_modularity": [<bail_node>, ...], // G8: per-node FP primitive on bail path (run_ikc.py:118-120); [] on non-bail
  "terminated": null | "max_k_below_floor" | "subgraph_none"
}
```

After this iter, residual is recompacted in old-compact-id-ascending order; next iter's `residual_compact_ids_sorted` starts at 0..n-1 of the new compaction.

### Per-bail-node record (G8)

```jsonc
{
  "compact_id": <int>,                            // current-residual compact id at bail
  "orig_compact_id": <int>,                       // pre-recompaction (orig_graph) compact id
  "d": <int>,                                     // orig_graph.degree(orig_compact_id) — directed out-degree (NetworKit semantics)
  "two_L": <int>,                                 // 2 * L (full original edge count)
  "ratio": <float>,                               // d / two_L (Python `/`, IEEE-754 double)
  "ratio_squared": <float>,                       // ratio * ratio — correctly-rounded r² (NOT canonical Python `**2`; see G8 note)
  "neg_ratio_squared": <float>                    // (-1) * ratio_squared (sign flip; bit pattern = ratio_squared XOR sign bit)
}
```

Only emitted on the bail iter (`terminated == "max_k_below_floor"`). Mirrors canonical's `run_ikc.py:118-120` exactly: one entry per `graph.iterNodes()` of the residual at bail time, in ASC-compact-id order. Even though every entry gets appended to `final_clusters` as a size-1 singleton (and gets filtered by `print_clusters`), the FP scalar IS computed in the shipped binary and feeds `len(final_clusters)` = `n_final_clusters_pre_singleton_filter`. Audit row D for IKC is N/A only on the dead-modularity-gate path; the bail path is live FP.

## Per-component record

```jsonc
{
  "comp_idx": <int>,                              // 0-indexed within iter; == canonical iteration order
  "comp_n": <int>,
  "members_iter_order": [<int>, ...],             // lex-smallest BFS from smallest compact id (see Contract notes)
  "members_sorted": [<int>, ...],                 // ASC; for set-equality fallback in diff
  "k_valid": <bool>,
  "k_valid_failing_compact_id": <int> | null,     // first node whose intra-kcore degree < kFloor; null if k_valid==true
  "k_valid_loop_scope": [<kv_scope_node>, ...],   // G2: every kcore-subgraph node in subgraph.iterNodes() ASC order
  "modular_value": 1.0,                           // dead-gate constant; canonical run_ikc.py:280
  "modular_pos": <bool>,                          // (modular > 0); always true under canonicalGate
  "kept": <bool>,                                 // (k_valid && modular_pos)
  "fate_reason": "accepted" | "failed k-valid" | "failed modularity",
  "nodes_to_remove_after_update_sorted": [<int>, ...]  // G9: accumulator state after this component's update(component)
}
```

### Per-k_valid-loop-scope-node record (G2)

```jsonc
{
  "subgraph_id": <int>,                           // residual-compact id (= kcore subgraph id since subgraphFromNodes preserves ids)
  "in_component": <bool>,                         // node ∈ component_nodes set
  "degIn": <int>,                                 // subgraph.degreeIn(node) — directed in-degree within kcore subgraph
  "degOut": <int>,                                // subgraph.degreeOut(node)
  "sum": <int>,                                   // degIn + degOut (the canonical k_valid threshold input)
  "check_result": "skipped" | "ok" | "fail" | "post_break_unvisited"
}
```

Iteration order = ASC by `subgraph_id` (canonical `subgraph.iterNodes()` returns ASC; JS scans kcoreMask 0..remCount-1 with kcoreMask set). `check_result`:
- `"skipped"`: not in component (canonical's `if node in component_nodes:` gate is false).
- `"ok"`: in component, sum >= kFloor, continue.
- `"fail"`: in component, sum < kFloor — short-circuits canonical's `break` at run_ikc.py:276. First such node = `k_valid_failing_compact_id`.
- `"post_break_unvisited"`: after the canonical break fires, the loop terminates; the tracer still records subsequent subgraph nodes for trace-scope visibility (full G2 loop-scope dump) but tags them as not actually visited by canonical.

## Final record

```jsonc
{
  "n_final_clusters_pre_singleton_filter": <int>,
  "n_final_clusters_post_singleton_filter": <int>,
  "csv_lines": <int>,                             // sum of per-cluster sizes (post-filter)
  "accepted_canonical_order": [
    {
      "max_k_at_acceptance": <int>,
      "members_compact_in_orig_id": [<int>, ...]  // compact id at the time of acceptance, mapped back to orig via node_id_map
    }, ...
  ]
}
```

## Determinism

IKC has no RNG. `iters[]` and `final` are determined by `(node_id_map, edges, k_floor)` alone. Two runs with identical input produce byte-equal trace.

## P0 probe inventory (closed 2026-05-10)

| Probe | Tag | JSON field(s) | Source line | Notes |
|---|---|---|---|---|
| G2 (k_valid loop scope + per-node degree) | `[TRACE-IKC-KV]` (stderr) | `components[].k_valid_loop_scope[]` | `run_ikc.py:266-277` | Exposes canonical's full `subgraph.iterNodes()` scope, not just the lex-BFS visit list. Per-node `degIn`/`degOut`/`sum` + `check_result` (skipped/ok/fail/post_break_unvisited). |
| G8 (bail-iter `(d/(2L))²` FP primitive) | `[TRACE-IKC-BAIL]` (stderr) | `iters[].bail_L`, `iters[].bail_per_node_modularity[]` | `run_ikc.py:118-120` | Live FP on bail path. Per-node `d`, `two_L`, `ratio`, `ratio_squared`, `neg_ratio_squared`. Canonical `**2` calls glibc `pow` (faithfully rounded, ~0.09% 1-ULP-off from correctly-rounded r²). V8 `Math.pow(x,2)` short-circuits to `x*x` for integer exponents. Both Python tracer and JS use `ratio*ratio` (correctly-rounded r²) — match principle (squaring), not glibc's specific approximation. Counter-example: physics_collab_pierreAuger iter 13 node 44 (d=12, 2L=12964): glibc `pow(r,2)`=0x3eacbff0c59c82ab vs `r*r`=0x3eacbff0c59c82aa; exact r² closer to aa. See `feedback_match_canonical_in_principle.md`. |
| G9 (`nodes_to_remove` mutation order) | `[TRACE-IKC-NTR]` (stderr) | `components[].nodes_to_remove_after_update_sorted` (per-component accumulator snapshot) | `run_ikc.py:131, 151, 160, 165, 193` | The set hash-iteration order at removal site (`run_ikc.py:193`) is implementation-defined and NOT bit-compared in JSON; stderr-only via `[TRACE-IKC-NTR] removal_iter_order=...`. The per-component sorted-accumulator snapshot IS in JSON; mutation sequence visible across `comp_idx` 0,1,2,... |

## Contract notes

- `members_iter_order` is **lex-smallest BFS** from the smallest unseen compact id, with each node's neighbour list sorted ASC before frontier expansion. This is the deterministic well-defined order both sides converge on. NetworKit's natural `getComponents()` order is implementation-defined (out-then-in directional adjacency in `forEdges`-insertion order on the recompacted graph) and DOES diverge from JS's natural order on some iterations (e.g. fixture32 iter 1 = `[12,13,14,16,17,18,15,19]` natural vs `[12,13,14,15,16,17,18,19]` lex-smallest). Both sides must canonicalize to lex-smallest BFS to converge: the JS kernel sorts adjacency in `compactSubgraph` + `buildResidualG`; the canonical-tracer must re-run BFS lex-smallest post-`getComponents()`. The diff harness compares array-equal (not set-equal); this is the H-row closure.
- `members_sorted` is a fallback diagnostic — if `members_iter_order` diverges but `members_sorted` matches, the partition is correct but BFS order differs (audit row H).
- `modular_value` is hardcoded `1.0` to mirror the dead gate (`run_ikc.py:280`). The paper formula is never computed in canonical-equal mode.
- Compact-id space changes between iters due to recompaction. `nodes_to_remove_sorted` and ids in subsequent iters refer to the recompacted space.
- Final `accepted_canonical_order.members_compact_in_orig_id` maps compact ids back to original (string) node names via `node_id_map` so the partition can be verified across the two implementations regardless of compaction drift.

## Build-pair / equivalence tests (playbook §1 step 1)

(a) **build-pair**: tracer with `TRACER_MODE=0` emits CSV; CSV must byte-equal `run_ikc.py` CSV (no print interference). For IKC, `TRACER_MODE=0` and `TRACER_MODE=1` are identical because there is no RNG/FP swap; the flag only gates JSON emission to stdout (silent vs verbose). CSV writing path is unchanged.

(b) **self-determinism**: two `TRACER_MODE=1` runs on the same input produce byte-equal trace JSON.

(c) **structural sanity**: tracer `TRACER_MODE=1` CSV byte-equals canonical CSV. (Subsumes (a) for IKC.)

(d) **JS hook-installed-vs-uninstalled**: JS `runIKC(nodeIds, edges, opts)` final return value (membership + accepted) is identical whether `globalThis.__IKC_HOOK` is installed or absent.

## Diagnostic ladder (playbook §11)

| L | IKC status | Reason |
|---|---|---|
| L0 RNG raw | N/A | no RNG |
| L1 RNG draw count | N/A | no RNG |
| L2 math primitives | N/A | dead FP path; `Math.pow(x,2)` only on unreachable paper formula |
| L3 oracle replay | N/A | no randomness to inject |
| **L4 self-RNG end-to-end** | target | collapses to "deterministic byte-equal trace" given identical input |

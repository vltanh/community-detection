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
  "terminated": null | "max_k_below_floor" | "subgraph_none"
}
```

After this iter, residual is recompacted in old-compact-id-ascending order; next iter's `residual_compact_ids_sorted` starts at 0..n-1 of the new compaction.

## Per-component record

```jsonc
{
  "comp_idx": <int>,                              // 0-indexed within iter; == canonical iteration order
  "comp_n": <int>,
  "members_iter_order": [<int>, ...],             // lex-smallest BFS from smallest compact id (see Contract notes)
  "members_sorted": [<int>, ...],                 // ASC; for set-equality fallback in diff
  "k_valid": <bool>,
  "k_valid_failing_compact_id": <int> | null,     // first node whose intra-kcore degree < kFloor; null if k_valid==true
  "modular_value": 1.0,                           // dead-gate constant; canonical run_ikc.py:280
  "modular_pos": <bool>,                          // (modular > 0); always true under canonicalGate
  "kept": <bool>,                                 // (k_valid && modular_pos)
  "fate_reason": "accepted" | "failed k-valid" | "failed modularity"
}
```

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

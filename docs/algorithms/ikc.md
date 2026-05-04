# IKC

[← back to index](../algorithms.md)

Iterative k-core Clustering. Wedell et al. 2022 (QSS 3(1):289-314,
[doi:10.1162/qss_a_00184](https://doi.org/10.1162/qss_a_00184)).

The page at `vltanh.github.io/comdet/ikc.html` walks the algorithm on the
32-node fixture; this file is the technical companion: where the code
lives, what each stage does, and how the shipped binary differs from
the paper.

## Entrypoint

`iterative_k_core_decomposition_MCS_ES(graph, k, inverted_orig_node_ids)`
at [`src/ikc/run_ikc.py:81`](../../src/ikc/run_ikc.py#L81). Driven by
the wrapper `main(args)` at [`src/ikc/run_ikc.py:13`](../../src/ikc/run_ikc.py#L13)
which loads the edgelist, normalises node ids via `format_graph` (line
301), runs the core loop, and writes `com.csv` with consecutive
0-indexed cluster ids (singletons filtered).

## CLI flags

| Flag | What it sets |
| --- | --- |
| `-e / --edgelist` | Path to the input CSV (header `source,target`) |
| `-o / --output-directory` | Output dir; produces `com.csv` |
| `-k / --kvalue` | Connectivity floor; defaults to `0` (= accept everything) |
| `-q / --quiet` | Suppresses per-iteration log lines |

The CM-paper benchmark sweeps `k = 10` (Park 2024 §4); the page shows
`k ∈ [2, 5]` because the 32-node fixture has no 5-core (max core
number = 4 from A's K_5).

## Outer loop

`run_ikc.py:103-204`. Each pass:

1. **Compute the current residual's core decomposition.** Calls
   `kc(graph)` at [`run_ikc.py:222`](../../src/ikc/run_ikc.py#L222),
   which delegates to `nk.centrality.CoreDecomposition`. Returns the
   subgraph induced by every node with core number `≥ max_k`, the
   current `max_k`, and the raw `kc` object.
2. **Bail check.** If `max_k < k` (line 114), every remaining node is
   emitted as a singleton (with a degree-aware modularity score that
   has no downstream effect since singletons are filtered in
   `print_clusters`).
3. **Connected-components split.** `nk.components.WeaklyConnectedComponents`
   on the kcore subgraph (line 125-127). Each component is a
   candidate cluster.
4. **k-validity gate.** `k_valid(component, subgraph, k)` at
   [`run_ikc.py:264`](../../src/ikc/run_ikc.py#L264): every node in
   the component must have `degreeIn + degreeOut ≥ k` *inside the kcore
   subgraph*. Stricter than membership in the global k-core, since
   peeling could leave a node with low residual degree even after it
   crossed the global core threshold.
5. **Modularity gate.** `modular(component, orig_graph,
   inverted_orig_node_ids)` at
   [`run_ikc.py:278`](../../src/ikc/run_ikc.py#L278). The first line of
   that function is `return POSITIVE_VALUE = 1`; the paper's formula
   `mod(s) = l_s/L − (d_s/2L)²` is dead code below the early return.
   Ship-hard divergence from the paper: in practice every k-valid
   component passes this gate.
6. **Emit + remove.** Surviving components are appended to
   `final_clusters` with their `max_k` value (the iteration's k, used
   downstream as a per-cluster k-bin label). Their nodes are removed
   from the residual graph; the loop iterates with one fewer max core.
7. **Compaction.** Lines 195-204: NetworKit's continuous-id requirement
   forces a re-pack of node ids after each removal (`getCompactedGraph`
   + the inverse-id-dict roll-forward).

## kc() core extractor

[`src/ikc/run_ikc.py:222-261`](../../src/ikc/run_ikc.py#L222). Wraps
`nk.centrality.CoreDecomposition(graph, storeNodeOrder=True)`. The
output is a `nk.structures.Partition` keyed by node id, value = core
number. The function extracts every node with core number in
`[k, max_k]` (line 254-256) and returns the induced subgraph. With
`k = max_k` (default when called without an explicit `k`), this is
exactly the `max_k`-core.

## Modularity-gate dead code

The paper's gate (Wedell 2022 §2.2.2):

```
mod(s) = ℓ_s / L − (d_s / 2L)²
```

where `ℓ_s` = intra-edges of cluster `s`, `d_s` = sum of intra-degrees,
`L` = total edges of the original graph. Implemented at
`run_ikc.py:282-294` but unreachable thanks to
[`run_ikc.py:280`](../../src/ikc/run_ikc.py#L280):

```python
def modular(component, orig_graph, inverted_orig_node_ids):
    POSITIVE_VALUE = 1
    return POSITIVE_VALUE  # ← paper's formula below this line is dead
```

Net effect: IKC's only effective filter is k-validity, and every
accepted cluster is logged with `modularity = 1`, which shows up in
any post-processing that consumes the per-cluster modularity column.

The page's stage-2 walker computes the paper formula for display, with
a "strict mod gate" toggle that re-enables `mod(s) > 0` enforcement.
On the 32-node fixture every accepted cluster has positive modularity
already, so the toggle does not move the partition. On larger
benchmarks the divergence shows up.

## Output

`com.csv` written by `print_clusters`
([`src/ikc/run_ikc.py:43`](../../src/ikc/run_ikc.py#L43)):

| Column | Meaning |
| --- | --- |
| `node_id` | Original node id (post-inverse-map) |
| `cluster_id` | Consecutive 0-indexed cluster label |

Singleton clusters (size ≤ 1) are dropped in the writer (line 65),
so dropped + bailed nodes never appear in `com.csv`.

## Place in the kmp-clustering pipeline

Wedell 2022 §2.2.3 defines a 4-stage `kmp-clustering` framework:

1. **Stage 1**: cluster `N` into k-valid + m-valid groups.
   Implementation = IKC (this page).
2. **Stage 2** (optional): break each non-singleton cluster from Stage
   1 into smaller k-valid pairwise disjoint clusters via Graclus.
3. **Stage 3** (optional): augment each non-singleton cluster with
   unclustered nodes as noncore (peripheral) members, provided they're
   adjacent to ≥ p core nodes in the chosen cluster.
4. **Stage 4**: assign core / noncore status + retain only kmp-valid
   clusters.

Only Stage 1 is implemented in `src/ikc/`; Stages 2-4 live in
`src/cm/` and the CM pipeline at
[`run_cd.sh`](../../run_cd.sh) (the post-processing chain that wraps
IKC with WCC / CM filters per Park 2024).

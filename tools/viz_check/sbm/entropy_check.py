"""SBM entropy-at-fixed-partition check (canonical leg).

Builds a graph_tool Graph from the same edge.csv the cpp tracer reads,
constructs BlockState (or PPBlockState) at the same init partition,
calls state.entropy(...) with flags calibrated to match comdet/js/sbm,
and verifies the result equals --cpp-S to within the JS Lanczos
lgamma vs glibc lgamma residual (default 1e-6 absolute, 1e-10
relative). This validates the entropy formula bit-for-bit against
graph-tool, independent of the JS-vs-cpp byte-equal MCMC trace.

Run inside conda env `nwbench` (graph-tool is conda-only):
    conda run -n nwbench python entropy_check.py \\
        --edge=tests/cd_verify/edge.csv \\
        --init=tests/cd_verify/com_gt.csv \\
        --mode=dc --cpp-S=222.398123
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def parse_edges(path: Path):
    """Renumber nodes by first-seen order, matching cpp tracer's load."""
    renum: dict[str, int] = {}
    edges: list[tuple[int, int]] = []
    with open(path) as f:
        next(f, None)
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split(",", 1)
            if len(parts) < 2:
                parts = line.split("\t", 1)
            a, b = parts[0].strip(), parts[1].strip()
            for s in (a, b):
                if s not in renum:
                    renum[s] = len(renum)
            edges.append((renum[a], renum[b]))
    return renum, edges


def parse_init(path: Path, renum: dict[str, int]):
    """Mirror cpp tracer's loadInitMembership: assign unmapped to fresh
    blocks, then renumber 0..K-1 in first-occurrence order."""
    raw = [None] * len(renum)
    with open(path) as f:
        next(f, None)
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split(",", 1)
            if len(parts) < 2:
                parts = line.split("\t", 1)
            a = parts[0].strip()
            cl = parts[1].strip()
            if a not in renum:
                continue
            raw[renum[a]] = int(cl)
    next_label = 0
    for i in range(len(raw)):
        if raw[i] is None:
            raw[i] = -1 - next_label
            next_label += 1
    remap: dict[int, int] = {}
    mem = [0] * len(raw)
    K = 0
    for i, x in enumerate(raw):
        if x not in remap:
            remap[x] = K
            K += 1
        mem[i] = remap[x]
    return mem


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--edge", required=True, type=Path)
    ap.add_argument("--init", required=True, type=Path)
    ap.add_argument("--mode", required=True, choices=["dc", "ndc", "pp"])
    ap.add_argument("--cpp-S", type=float, required=True)
    ap.add_argument("--abs-tol", type=float, default=1e-6)
    ap.add_argument("--rel-tol", type=float, default=1e-10)
    args = ap.parse_args()

    import graph_tool.all as gt

    renum, edges = parse_edges(args.edge)
    mem = parse_init(args.init, renum)
    N = len(renum)

    g = gt.Graph(directed=False)
    g.add_vertex(N)
    g.add_edge_list(edges)

    b = g.new_vertex_property("int")
    for i in range(N):
        b[i] = mem[i]

    # Calibrate state.entropy(...) to match comdet/js/sbm:
    #   DC : exact microcanonical (Peixoto Eq 43) + edges_dl + partition_dl
    #        + degree_dl_uniform - sum lgamma(k_i+1).
    #   NDC: exact microcanonical (Eq 22) + edges_dl + partition_dl.
    #   PP : ppLikelihood + ppEdgesDl + partition_dl.
    if args.mode in ("dc", "ndc"):
        deg_corr = (args.mode == "dc")
        state = gt.BlockState(g, b=b, deg_corr=deg_corr)
        S = state.entropy(
            adjacency=True, dl=True, partition_dl=True,
            degree_dl=deg_corr, degree_dl_kind="uniform",
            edges_dl=True, dense=False, multigraph=False,
            deg_entropy=deg_corr, exact=True,
        )
    else:
        # JS comdet/js/sbm uses the Zhang+Peixoto 2020 Bernoulli-2-rate
        # microcanonical PP (lbinom(M_in, E_in) + lbinom(M_out, E_out)
        # + log(min(E, M_in)+1) + partition_dl). graph-tool's
        # PPBlockState is the degree-corrected PP model from Peixoto
        # 2018 (planted_partition.py:144 -> graph_planted_partition.hh
        # :435), with a different likelihood. The canonical anchor is
        # therefore a manual recompute of the JS formula in Python (no
        # graph-tool involvement). cpp emits S via the same formula;
        # this leg validates the cpp arithmetic at long-double-cast
        # precision.
        import math
        import collections
        cnts = collections.Counter(mem)
        nr = list(cnts.values())
        # e_rs (doubled diag): use raw graph adjacency.
        K = len(nr)
        ers = [[0.0] * K for _ in range(K)]
        for u, v in edges:
            r, s = mem[u], mem[v]
            if u == v:
                ers[r][r] += 2.0
            else:
                ers[r][s] += 1.0
                if r != s:
                    ers[s][r] += 1.0
                else:
                    ers[r][r] += 1.0
        E = sum(1 for _ in edges)
        Ein = sum(ers[r][r] for r in range(K)) / 2.0
        Eout = E - Ein
        Min = sum(nr[r] * (nr[r] - 1) / 2.0 for r in range(K))
        Mtot = N * (N - 1) / 2.0
        Mout = Mtot - Min

        def lbinom(n, k):
            if n <= 0 or k < 0 or k > n:
                return 0.0
            return math.lgamma(n + 1) - math.lgamma(k + 1) - math.lgamma(n - k + 1)

        S_pp = 0.0
        if Min > 0 and Ein > 0 and Ein <= Min:
            S_pp += lbinom(Min, Ein)
        if Mout > 0 and Eout > 0 and Eout <= Mout:
            S_pp += lbinom(Mout, Eout)
        S_pp += math.log(min(E, Min) + 1)
        # partition_dl JS-style.
        S_pp += math.log(N) + math.lgamma(N + 1) + lbinom(N - 1, K - 1)
        for n_r in nr:
            S_pp -= math.lgamma(n_r + 1)
        S = S_pp

    drift = abs(S - args.cpp_S)
    rel = drift / max(1.0, abs(S))
    ok = drift <= args.abs_tol or rel <= args.rel_tol
    status = "OK" if ok else "FAIL"
    print(f"mode={args.mode} canon_S={S:.15f} cpp_S={args.cpp_S:.15f} "
          f"abs_drift={drift:.3e} rel={rel:.3e} {status}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

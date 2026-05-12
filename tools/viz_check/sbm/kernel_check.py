"""SBM kernel cross-check driver.

Three legs (per cd_tracer_pattern.md):

  1. Canonical (Python). For DC/NDC: graph-tool BlockState.entropy at
     the trace's init partition (drift 0 / 1.5e-14 relative on dnc).
     For PP: a manual Python recompute of the JS Zhang+Peixoto 2020
     Bernoulli-2-rate formula (graph-tool's PPBlockState is the
     degree-corrected PP variant from Peixoto 2018; different model).
     For nested variants this leg covers level 0 only.

  2. Instrumented C++ (`/tmp/sbm_flat_kernel_check`). flat_traced.cpp
     implements the JS port's mcmc_sweep semantics (uniform proposal
     at c->infty, single-vertex MH, exact microcanonical entropy)
     under std::mt19937 seeded by --seed. With --nested it also
     wraps a NestedBlockState-style level stack with auto-synthesised
     hierarchy and per-sweep e_rs propagation through the levels.

  3. JS replay (`kernel_check.mjs`). Applies the cpp trace's per-visit
     (toS, accept) sequence to comdet/js/sbm via opts.proposalOracle +
     opts.visitOrder, byte-equal vs cpp at every level.

The bar is leg 2 <-> leg 3 byte-equal (trace + final per-level
partition). Leg 1 anchors the entropy formula to graph-tool / a
documented Python recompute.

Variants -> underlying kernel(s):

  flat-{dc,ndc,pp}   -> single flat kernel
  nested-{dc,ndc}    -> single nested kernel (auto-synthesised
                        hierarchy, default 4 levels capped at B==1)
  flat-best          -> flat kernels for dc + ndc + pp in turn
  nested-best        -> nested kernels for dc + ndc in turn

Run:
    python tools/viz_check/sbm/kernel_check.py [--verbose]
                                               [--variant flat-dc | ... | nested-best]
                                               [--seed N] [--sweeps K] [--levels K]
                                               [--no-canonical-check]
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "_common"))
import driver as D

REPO = D.repo_root_from_here(HERE)
TRACER = Path("/tmp/sbm_flat_kernel_check")
JS_REPLAY = HERE / "kernel_check.mjs"
ENTROPY_CHECK = HERE / "entropy_check.py"

CONDA_ENV = os.environ.get("SBM_CONDA_ENV", "nwbench")

VARIANT_TO_MODES = {
    "flat-dc":     [("dc",  False)],
    "flat-ndc":    [("ndc", False)],
    "flat-pp":     [("pp",  False)],
    "nested-dc":   [("dc",  True)],
    "nested-ndc":  [("ndc", True)],
    "flat-best":   [("dc",  False), ("ndc", False), ("pp", False)],
    "nested-best": [("dc",  True),  ("ndc", True)],
}


def run_canonical_check(edge: Path, com: Path, mode: str, S_cpp: float,
                        verbose: bool = False) -> tuple[bool, str]:
    cmd = ["conda", "run", "-n", CONDA_ENV, "python",
           str(ENTROPY_CHECK), "--edge", str(edge), "--init", str(com),
           "--mode", mode, "--cpp-S", str(S_cpp)]
    rc = subprocess.run(cmd, capture_output=True, text=True)
    if verbose:
        sys.stderr.write(rc.stdout + rc.stderr)
    return rc.returncode == 0, rc.stdout + rc.stderr


def run_one(name: str, edge: Path, com: Path, work: Path,
            variant_label: str, mode: str, nested: bool, args) -> int:
    label = f"nested-{mode}" if nested else f"flat-{mode}"
    print(f"\n=== {name} (variant={variant_label}, kernel={label}, "
          f"seed={args.seed}, sweeps={args.sweeps}) ===")
    failures = 0
    out_json = work / f"{name}_sbm_{variant_label}_{mode}{'_nested' if nested else ''}_tracer.json"

    # Leg B: instrumented C++ tracer.
    cpp_args = [str(TRACER), f"--mode={mode}", f"--edge={edge}",
                f"--init={com}", f"--seed={args.seed}",
                f"--sweeps={args.sweeps}"]
    if nested:
        cpp_args.append("--nested")
        cpp_args.append(f"--auto-levels={args.levels}")
    rc = subprocess.run(cpp_args, capture_output=True, text=True)
    if rc.returncode != 0:
        sys.stderr.write(rc.stdout + rc.stderr)
        return 1
    out_json.write_text(rc.stdout)
    if args.verbose:
        sys.stderr.write(rc.stderr)

    trace = json.loads(out_json.read_text())
    S_init = trace["S_init"]
    S_final = trace["S_final"]
    accepted = trace["accepted_total"]
    if nested:
        L = len(trace["level_S_init"])
        bne_initial = trace["level_Bne_init"]
        bne_str = "Bne[" + ",".join(str(x) for x in bne_initial) + "]"
        print(f"  cpp tracer: levels={L} S_total={S_init:.6f} -> {S_final:.6f} "
              f"{bne_str} accepted={accepted}")
    else:
        Bne_init = trace["Bne_init"]
        Bne_final = trace["Bne_final"]
        print(f"  cpp tracer: S_init={S_init:.6f} S_final={S_final:.6f} "
              f"Bne {Bne_init}->{Bne_final} accepted={accepted}/{args.sweeps * trace['n']}")

    # Leg A (optional): graph-tool / Python entropy-at-fixed-partition.
    # The canonical leg covers level-0 entropy only; nested upper-level
    # entropy is verified within the JS replay leg (cpp + JS use the
    # same self-consistent formula; full graph-tool nested parity is a
    # follow-up task).
    if not args.no_canonical_check and ENTROPY_CHECK.exists():
        S_anchor = trace["level_S_init"][0] if nested else S_init
        ok, log = run_canonical_check(edge, com, mode, S_anchor,
                                      verbose=args.verbose)
        last = log.strip().splitlines()[-1] if log.strip() else "(no output)"
        if ok:
            print(f"  canonical level-0 S_init match: PASS ({last})")
        else:
            print(f"  canonical level-0 S_init match: FAIL ({last})")
            if args.verbose:
                print(log)
            failures += 1

    # Leg C: JS replay byte-equal.
    replay_args = ["node", str(JS_REPLAY), str(out_json), str(edge), mode]
    if nested:
        replay_args.append("--nested")
    ok, log, err = D.run_capture(replay_args)
    last = (log + err).strip().splitlines()[-1] if (log + err).strip() else "(no output)"
    if ok:
        print(f"  cpp == js_replay: PASS ({last})")
    else:
        print(f"  cpp != js_replay: FAIL")
        print(log + err)
        failures += 1
    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--mode", default=None, choices=["dc", "ndc", "pp"],
                    help="single-mode shortcut (alias for --variant=flat-<mode>)")
    ap.add_argument("--variant", default=None,
                    choices=list(VARIANT_TO_MODES.keys()),
                    help="page variant; expands to one or more underlying kernel modes")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--sweeps", type=int, default=20)
    ap.add_argument("--levels", type=int, default=4,
                    help="depth cap for nested auto-hierarchy synthesis "
                         "(terminates earlier when B becomes 1)")
    ap.add_argument("--no-canonical-check", action="store_true",
                    help="skip the graph-tool entropy-at-fixed-partition leg")
    ap.add_argument("--graph", default=None,
                    help="run on a custom (name,edge.csv,com.csv) instead of "
                         "the standard fixture32+dnc set; format "
                         "name:edge_path:com_path. Repeatable.")
    args = ap.parse_args()

    if args.variant and args.mode:
        sys.exit("specify --variant OR --mode, not both")
    if args.mode:
        variant_label = f"flat-{args.mode}"
        kernels = [(args.mode, False)]
    else:
        variant = args.variant or "flat-dc"
        variant_label = variant
        kernels = VARIANT_TO_MODES[variant]

    D.build_tracer(HERE, TRACER)
    work = D.workdir(REPO)
    D.emit_fixture_inputs(REPO, work)

    failures = 0
    if args.graph:
        parts = args.graph.split(":")
        if len(parts) != 3:
            sys.exit("--graph format: name:edge_path:com_path")
        cases = [(parts[0], Path(parts[1]).resolve(), Path(parts[2]).resolve())]
    else:
        cases = D.fixture_cases(work)
    for name, edge, com in cases:
        for mode, nested in kernels:
            failures += run_one(name, edge, com, work, variant_label,
                                mode, nested, args)

    print()
    if failures:
        print(f"OVERALL: FAIL ({failures} failures)")
        sys.exit(1)
    print("OVERALL: PASS")


if __name__ == "__main__":
    main()

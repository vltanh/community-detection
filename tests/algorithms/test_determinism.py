"""Reproducibility tests for leiden + sbm bases and their post-procs.

Two reproducibility properties tested per (algo, postproc?) combo:

  seed_repro             : 5 seeds, 5 runs each. Asserts BOTH:
                              within-seed determinism (3 runs per seed
                              are byte-identical), AND
                              cross-seed sensitivity (≥2 distinct outputs
                              across the 5 seed-references — catches dead
                              --seed wiring; tolerates degenerate
                              convergence on a subset).
  pythonhashseed_invariant: 5 PYTHONHASHSEED values (fixed --seed 1).
                              All 5 outputs must be byte-identical
                              (catches set/dict-iteration leakage).

Scope: leiden-cpm-{0.0001,0.01}, leiden-mod, sbm-flat-{dc,ndc,pp,best},
sbm-nested-{dc,ndc,best}. Each base also tested with applicable post-procs:
sbm-* with cc + wcc(piecewise); leiden-* with cm(piecewise). The dispatcher
disables non-applicable combos automatically per algo.
"""

import os
import subprocess

import pytest

pytestmark = pytest.mark.slow

NETWORK_ID = "dnc"

# Bases to exercise. Add/remove freely.
LEIDEN_BASES = ["leiden-cpm-0.0001", "leiden-cpm-0.01", "leiden-mod"]
SBM_FLAT_BASES = ["sbm-flat-dc", "sbm-flat-ndc", "sbm-flat-pp", "sbm-flat-best"]
SBM_NESTED_BASES = ["sbm-nested-dc", "sbm-nested-ndc", "sbm-nested-best"]
ALL_BASES = LEIDEN_BASES + SBM_FLAT_BASES + SBM_NESTED_BASES

# (base_algo, postproc_gate_flag, postproc_dirname_suffix). Each entry runs the
# base, then the listed post-proc, and asserts reproducibility on the post-proc
# leaf's com.csv. Post-proc gating in run_cd.sh: cc/wcc are sbm-only, cm is
# leiden-only.
POSTPROC_VARIANTS = (
    [(b, ["--run-cc"], "+cc") for b in SBM_FLAT_BASES + SBM_NESTED_BASES]
    + [(b, ["--run-wcc", "--criterion", "piecewise"], "+wcc(piecewise)")
       for b in SBM_FLAT_BASES + SBM_NESTED_BASES]
    + [(b, ["--run-cm", "--criterion", "piecewise"], "+cm(piecewise)")
       for b in LEIDEN_BASES]
)

# Per-class run multiplicity. Bumping these increases coverage and wall time
# linearly; pytest's tmp_path keeps runs independent.
SEED_REPRO_SEEDS = ["1", "2", "7", "42", "1234"]
SEED_REPRO_RUNS_PER = 5
PYTHONHASHSEEDS = ["0", "7", "13", "42", "12345"]


def _env(hash_seed="0"):
    e = os.environ.copy()
    e["PYTHONHASHSEED"] = hash_seed
    e["OMP_NUM_THREADS"] = "1"
    return e


def _run(repo_root, algo, edgelist, output, *, seed="1", hash_seed="0", gates=()):
    return subprocess.run(
        [
            "bash", str(repo_root / "run_cd.sh"),
            "--algo", algo,
            "--input-edgelist", str(edgelist),
            "--output-dir", str(output),
            "--network", NETWORK_ID,
            "--seed", seed,
            *gates,
        ],
        capture_output=True, text=True, env=_env(hash_seed=hash_seed),
    )


def _read_com(output, algo):
    """Read clustering output bytes for a given base or post-proc dirname.
    For sbm-best com.csv is a symlink into a winner variant — read_bytes
    follows it transparently."""
    return (output / "clusterings" / algo / NETWORK_ID / "com.csv").read_bytes()


def _run_target(repo_root, base, gates, edgelist, output, *, seed, hash_seed):
    rc = _run(repo_root, base, edgelist, output, seed=seed, hash_seed=hash_seed,
              gates=gates)
    assert rc.returncode == 0, f"{base}{gates}: run failed (seed={seed} " \
        f"hashseed={hash_seed}): {rc.stderr}"


# -------- seed reproducibility (within-seed det + cross-seed sensitivity) --

def _check_seed_repro(repo_root, tmp_path, edgelist, base, gates, leaf_dirname):
    """For each seed in SEED_REPRO_SEEDS, run SEED_REPRO_RUNS_PER times.
    Assert (a) within each seed all runs are byte-identical, and
    (b) across seeds at least 2 distinct outputs exist.

    (a) catches nondeterminism; (b) catches dead --seed wiring while
    tolerating degenerate convergence on a subset of seeds."""
    seed_refs = []
    for seed in SEED_REPRO_SEEDS:
        outputs = []
        for run_idx in range(SEED_REPRO_RUNS_PER):
            out = tmp_path / f"s{seed}_r{run_idx}"
            _run_target(repo_root, base, gates, edgelist, out,
                        seed=seed, hash_seed="0")
            outputs.append(_read_com(out, leaf_dirname))
        ref = outputs[0]
        for i, body in enumerate(outputs[1:], 1):
            assert body == ref, \
                f"{leaf_dirname}: same-seed nondeterminism (seed={seed}, " \
                f"run 0 vs run {i})"
        seed_refs.append(ref)
    distinct = len(set(seed_refs))
    assert distinct >= 2, \
        f"{leaf_dirname}: --seed has no effect across {SEED_REPRO_SEEDS} " \
        f"({distinct} distinct output)"


@pytest.mark.parametrize("algo", ALL_BASES)
def test_seed_repro_base(repo_root, tmp_path, dnc_edgelist, algo):
    _check_seed_repro(repo_root, tmp_path, dnc_edgelist, algo, (), algo)


@pytest.mark.parametrize("base,gates,suffix", POSTPROC_VARIANTS)
def test_seed_repro_postproc(repo_root, tmp_path, dnc_edgelist, base, gates, suffix):
    _check_seed_repro(repo_root, tmp_path, dnc_edgelist, base, gates, f"{base}{suffix}")


# -------- PYTHONHASHSEED invariance (multiple hashseeds, fixed --seed) -----

def _check_pythonhashseed(repo_root, tmp_path, edgelist, base, gates, leaf_dirname):
    outputs = []
    for hs in PYTHONHASHSEEDS:
        out = tmp_path / f"h{hs}"
        _run_target(repo_root, base, gates, edgelist, out,
                    seed="1", hash_seed=hs)
        outputs.append((hs, _read_com(out, leaf_dirname)))
    ref_hs, ref = outputs[0]
    for hs, body in outputs[1:]:
        assert body == ref, \
            f"{leaf_dirname}: PYTHONHASHSEED-dependent output " \
            f"(hash_seed {ref_hs} vs {hs})"


@pytest.mark.parametrize("algo", ALL_BASES)
def test_pythonhashseed_invariant_base(repo_root, tmp_path, dnc_edgelist, algo):
    _check_pythonhashseed(repo_root, tmp_path, dnc_edgelist, algo, (), algo)


@pytest.mark.parametrize("base,gates,suffix", POSTPROC_VARIANTS)
def test_pythonhashseed_invariant_postproc(repo_root, tmp_path, dnc_edgelist,
                                            base, gates, suffix):
    _check_pythonhashseed(repo_root, tmp_path, dnc_edgelist, base, gates, f"{base}{suffix}")

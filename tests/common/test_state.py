"""Unit tests for src/_common/state.sh.

Verbatim port from network-generation/tests/common/test_state.py with paths
adjusted for CD layout. State.sh is byte-equal to NG's per
tools/check_common_sync.sh; tests should pass identically.
"""

import subprocess
from pathlib import Path

import pytest


def run_bash(state_sh, snippet, *, env=None, cwd=None):
    """Run a bash one-liner with state.sh sourced. Returns CompletedProcess."""
    full = f"set -u\nsource {state_sh}\n{snippet}\n"
    return subprocess.run(
        ["bash", "-c", full],
        capture_output=True,
        text=True,
        env=env,
        cwd=cwd,
    )


def test_is_step_done_missing_done_returns_1(state_sh, tmp_path):
    rc = run_bash(state_sh, f'is_step_done {tmp_path}/missing "{tmp_path}/x"')
    assert rc.returncode == 1


def test_is_step_done_missing_output_returns_1(state_sh, tmp_path):
    out = tmp_path / "out.txt"
    out.write_text("data\n")
    done = tmp_path / "done"
    done.write_text(f"$(sha256sum {out})\n")
    out.unlink()
    rc = run_bash(state_sh, f'is_step_done {done} "{out}"')
    assert rc.returncode == 1


def test_is_step_done_empty_output_returns_1(state_sh, tmp_path):
    out = tmp_path / "out.txt"
    out.write_text("")
    done = tmp_path / "done"
    done.write_text("(unused)\n")
    rc = run_bash(state_sh, f'is_step_done {done} "{out}"')
    assert rc.returncode == 1


def test_mark_done_then_is_step_done_passes(state_sh, tmp_path):
    inp = tmp_path / "in.txt"
    out = tmp_path / "out.txt"
    inp.write_text("input data\n")
    out.write_text("output data\n")
    done = tmp_path / "done"

    rc = run_bash(state_sh, f'mark_done {done} "test" "{inp}" "{out}"')
    assert rc.returncode == 0, rc.stderr

    rc = run_bash(state_sh, f'is_step_done {done} "{out}"')
    assert rc.returncode == 0


def test_mark_done_fails_atomically_when_sha256sum_fails(state_sh, tmp_path):
    """Declared input missing -> mark_done exits 1 + leaves no partial done file."""
    out = tmp_path / "out.txt"
    out.write_text("data\n")
    done = tmp_path / "done"
    rc = run_bash(state_sh, f'mark_done {done} "test" "/nonexistent_input" "{out}"')
    assert rc.returncode == 1
    assert not done.exists()


def test_mark_done_fails_when_output_missing(state_sh, tmp_path):
    done = tmp_path / "done"
    rc = run_bash(state_sh, f'mark_done {done} "test" "" "{tmp_path}/missing"')
    assert rc.returncode == 1
    assert "was not created" in rc.stdout


def test_mark_done_fails_when_output_empty(state_sh, tmp_path):
    out = tmp_path / "empty.txt"
    out.write_text("")
    done = tmp_path / "done"
    rc = run_bash(state_sh, f'mark_done {done} "test" "" "{out}"')
    assert rc.returncode == 1
    assert "completely empty" in rc.stdout


def test_tampered_output_invalidates_done(state_sh, tmp_path):
    out = tmp_path / "out.txt"
    out.write_text("original\n")
    done = tmp_path / "done"
    run_bash(state_sh, f'mark_done {done} "test" "" "{out}"').check_returncode()
    out.write_text("tampered\n")
    rc = run_bash(state_sh, f'is_step_done {done} "{out}"')
    assert rc.returncode == 1


def test_write_params_file_requires_at_least_one_kv(state_sh, tmp_path):
    rc = run_bash(state_sh, f'write_params_file {tmp_path}/p.txt')
    assert rc.returncode == 2


def test_write_params_file_sorts_lc_all_c(state_sh, tmp_path):
    rc = run_bash(state_sh, f'write_params_file {tmp_path}/p.txt "z=1" "a=2" "m=3"')
    assert rc.returncode == 0
    assert (tmp_path / "p.txt").read_text() == "a=2\nm=3\nz=1\n"


def test_write_params_file_atomic(state_sh, tmp_path):
    """Tmp file is mv'd; never visible mid-write."""
    rc = run_bash(state_sh, f'write_params_file {tmp_path}/p.txt "k=v"')
    assert rc.returncode == 0
    leftover_tmps = list(tmp_path.glob("p.txt.tmp.*"))
    assert leftover_tmps == []


def test_is_state_tree_consistent_missing_dir(state_sh, tmp_path):
    rc = run_bash(state_sh, f'is_state_tree_consistent {tmp_path}/missing')
    assert rc.returncode == 1


def test_is_state_tree_consistent_empty_dir(state_sh, tmp_path):
    rc = run_bash(state_sh, f'is_state_tree_consistent {tmp_path}')
    assert rc.returncode == 1


def test_is_state_tree_consistent_with_valid_done(state_sh, tmp_path):
    sub = tmp_path / "stage"
    sub.mkdir()
    out = sub / "out.txt"
    out.write_text("data\n")
    done = sub / "done"
    run_bash(state_sh, f'mark_done {done} "stage" "" "{out}"').check_returncode()
    rc = run_bash(state_sh, f'is_state_tree_consistent {tmp_path}')
    assert rc.returncode == 0


def test_is_state_tree_consistent_with_stale_done(state_sh, tmp_path):
    sub = tmp_path / "stage"
    sub.mkdir()
    out = sub / "out.txt"
    out.write_text("data\n")
    done = sub / "done"
    run_bash(state_sh, f'mark_done {done} "stage" "" "{out}"').check_returncode()
    out.write_text("tampered\n")
    rc = run_bash(state_sh, f'is_state_tree_consistent {tmp_path}')
    assert rc.returncode == 1


def test_run_stage_appends_header_and_exit(state_sh, tmp_path):
    log = tmp_path / "log"
    rc = run_bash(state_sh, f'TIMEOUT=5s run_stage {log} /usr/bin/true')
    assert rc.returncode == 0
    text = log.read_text()
    assert "EXECUTED" in text
    assert "exit=0" in text


def test_run_stage_propagates_failure(state_sh, tmp_path):
    log = tmp_path / "log"
    rc = run_bash(state_sh, f'TIMEOUT=5s run_stage {log} bash -c "exit 7"')
    assert rc.returncode == 7
    assert "exit=7" in log.read_text()


def test_note_stage_skipped_appends_skip_line(state_sh, tmp_path):
    log = tmp_path / "log"
    rc = run_bash(state_sh, f'note_stage_skipped {log}')
    assert rc.returncode == 0
    assert "SKIPPED (cache hit)" in log.read_text()


def test_log_invocation_header_writes(state_sh, tmp_path):
    log = tmp_path / "run.log"
    rc = run_bash(state_sh, f'log_invocation_header {log} 1234 1')
    assert rc.returncode == 0
    text = log.read_text()
    assert "Invocation" in text
    assert "seed=1234" in text
    assert "keep_state=1" in text


def test_append_stage_log_idempotent_on_missing_source(state_sh, tmp_path):
    dest = tmp_path / "dest.log"
    rc = run_bash(state_sh, f'append_stage_log {dest} stage1 {tmp_path}/missing')
    assert rc.returncode == 0
    assert not dest.exists()


def test_append_stage_log_prefixes_label(state_sh, tmp_path):
    src = tmp_path / "src.log"
    src.write_text("line1\nline2\n")
    dest = tmp_path / "dest.log"
    rc = run_bash(state_sh, f'append_stage_log {dest} stage1 {src}')
    assert rc.returncode == 0
    text = dest.read_text()
    assert "[stage1] line1" in text
    assert "[stage1] line2" in text
    assert "=== [stage1]" in text

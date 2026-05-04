"""pipeline_common.py must not pull heavy scientific deps at top-import.

CD's pipeline_common.py is a verbatim subset of NG's, but the heavy-import
discipline still applies: any caller that imports the module shouldn't
implicitly drag scipy/numpy/pymincut into memory unless it asks for them.
"""

import ast
from pathlib import Path

import pytest


FORBIDDEN_TOP = {"scipy", "numpy", "pymincut"}


def _top_imports(path):
    tree = ast.parse(path.read_text())
    out = []
    for node in tree.body:
        if isinstance(node, ast.Import):
            out.extend(n.name.split(".")[0] for n in node.names)
        elif isinstance(node, ast.ImportFrom):
            out.append((node.module or "").split(".")[0])
    return out


def test_pipeline_common_no_heavy_top_imports(shared_dir):
    imports = _top_imports(shared_dir / "pipeline_common.py")
    bad = set(imports) & FORBIDDEN_TOP
    assert not bad, f"forbidden top-imports in pipeline_common.py: {bad}"

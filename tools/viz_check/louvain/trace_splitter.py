"""Split a giant Louvain L4 cpp tracer JSON into meta + NDJSON.

Node's V8 has a 512 MB hard cap on string size, so JSON.parse on a
single trace JSON >512 MB (which happens on dense T3 fixtures e.g.
google n=15763 seed=1 → ~1 GB) fails outright. This splitter uses
Python's ijson streaming parser to walk the trace and emit:

    <out>/trace.meta.json     — everything EXCEPT levels[L].visits[*]
                                (small; fits in a JS string)
    <out>/trace.visits.ndjson  — one line per visit, each line is
                                {"L":<level>,"P":<pass>,"V":<visit>,
                                 ...visit fields...}

`kernel_check_l4.mjs` then reads meta as a regular JSON, and streams
the NDJSON line by line. Streaming preserves byte-equal semantics —
every visit field stays bit-identical to the original JSON.

Usage:
    python trace_splitter.py <trace.json> <out_dir>
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import ijson  # type: ignore


def split_trace(trace_path: Path, out_dir: Path) -> tuple[Path, Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    meta_path = out_dir / "trace.meta.json"
    nd_path = out_dir / "trace.visits.ndjson"

    # First pass: stream top-level scalars + per-level scalars (everything
    # except the giant per-visit + visit_order_per_pass arrays).
    meta: dict = {"levels": []}
    with trace_path.open("rb") as fh:
        parser = ijson.parse(fh)
        # Track location via prefix; emit meta keys + per-level meta inline.
        cur_level: dict | None = None
        cur_visit: dict | None = None
        cur_cand: dict | None = None
        cur_arr_name: str | None = None
        cur_arr: list | None = None
        sub_arr_stack: list[tuple[str, list]] = []

        # Open NDJSON sink.
        ndf = nd_path.open("w", encoding="utf-8")
        try:
            visits_count_per_level: list[int] = []
            level_idx = -1
            pass_idx = -1
            visit_idx = -1

            for prefix, event, value in parser:
                # Top-level scalars: "mode", "seed", "n", "m2",
                # "fine_membership_post_renumber", "Q_final_bits".
                if event in ("string", "number", "boolean", "null"):
                    if prefix == "mode" or prefix == "seed" or prefix == "n":
                        meta[prefix.split(".")[-1]] = value
                    elif prefix == "m2":
                        meta["m2"] = value
                    elif prefix == "Q_final_bits":
                        meta["Q_final_bits"] = value
                    elif prefix == "fine_membership_post_renumber.item":
                        meta.setdefault("fine_membership_post_renumber", []).append(value)
                    elif prefix.startswith("levels.item."):
                        sub = prefix[len("levels.item."):]
                        if cur_level is None:
                            continue
                        if sub == "level" or sub == "n_before" or sub == "passes":
                            cur_level[sub] = value
                        elif sub == "totalWeightBitsPre" or sub == "totalWeightBitsPost" \
                                or sub == "nAfterCollapse":
                            cur_level[sub] = value
                        elif sub.startswith("visit_order_per_pass.item.item"):
                            cur_arr.append(value)  # type: ignore
                        elif sub.startswith("qualityPerPassBits.item"):
                            cur_level.setdefault("qualityPerPassBits", []).append(value)
                        elif sub.startswith("curQualPerPassBits.item"):
                            cur_level.setdefault("curQualPerPassBits", []).append(value)
                        elif sub.startswith("nbMovesPerPass.item"):
                            cur_level.setdefault("nbMovesPerPass", []).append(value)
                        elif sub.startswith("n2c_post.item"):
                            cur_level.setdefault("n2c_post", []).append(value)
                        elif sub.startswith("inBitsEntry.item"):
                            cur_level.setdefault("inBitsEntry", []).append(value)
                        elif sub.startswith("totBitsEntry.item"):
                            cur_level.setdefault("totBitsEntry", []).append(value)
                        elif sub.startswith("inBitsExit.item"):
                            cur_level.setdefault("inBitsExit", []).append(value)
                        elif sub.startswith("totBitsExit.item"):
                            cur_level.setdefault("totBitsExit", []).append(value)
                        elif sub.startswith("visits.item."):
                            # Per-visit field. Defer NDJSON write until end_map.
                            fld = sub[len("visits.item."):]
                            if cur_visit is None:
                                continue
                            if fld.startswith("candidates.item."):
                                cfld = fld[len("candidates.item."):]
                                if cur_cand is None:
                                    continue
                                cur_cand[cfld] = value
                            else:
                                cur_visit[fld] = value
                elif event == "start_array":
                    if prefix == "levels":
                        pass  # levels[]; cur_level set on item start_map
                    elif prefix == "levels.item.visit_order_per_pass":
                        cur_level["visit_order_per_pass"] = []
                    elif prefix == "levels.item.visit_order_per_pass.item":
                        cur_arr = []
                        cur_level["visit_order_per_pass"].append(cur_arr)
                    elif prefix == "levels.item.visits":
                        # streaming target
                        pass
                    elif prefix == "levels.item.visits.item.candidates":
                        if cur_visit is not None:
                            cur_visit["candidates"] = []
                elif event == "end_array":
                    if prefix == "levels.item.visit_order_per_pass.item":
                        cur_arr = None
                elif event == "start_map":
                    if prefix == "levels.item":
                        cur_level = {}
                        level_idx += 1
                    elif prefix == "levels.item.visits.item":
                        cur_visit = {}
                    elif prefix == "levels.item.visits.item.candidates.item":
                        cur_cand = {}
                elif event == "end_map":
                    if prefix == "levels.item":
                        # Drop the visits[] payload before writing meta;
                        # we stream visits via NDJSON instead.
                        cur_level.pop("visits", None)
                        meta["levels"].append(cur_level)
                        cur_level = None
                    elif prefix == "levels.item.visits.item":
                        # Emit NDJSON line.
                        vobj = cur_visit if cur_visit is not None else {}
                        vobj["L"] = level_idx
                        ndf.write(json.dumps(vobj, separators=(",", ":")))
                        ndf.write("\n")
                        cur_visit = None
                    elif prefix == "levels.item.visits.item.candidates.item":
                        if cur_visit is not None and cur_cand is not None:
                            cur_visit.setdefault("candidates", []).append(cur_cand)
                        cur_cand = None
        finally:
            ndf.close()

    meta_path.write_text(json.dumps(meta))
    return meta_path, nd_path


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: trace_splitter.py <trace.json> <out_dir>\n")
        return 2
    trace = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    meta, nd = split_trace(trace, out_dir)
    print(f"meta: {meta} ({meta.stat().st_size:,} bytes)")
    print(f"ndjson: {nd} ({nd.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

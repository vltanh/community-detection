# cd_verify

Kernel-vs-binary verification harness for the comdet JS ports of CC,
WCC, and CM. Mirrors the netgen kernel-port workflow: run the canonical
binary and the JS kernel on the same input, diff the partitions.

## Files

| File | Purpose |
|---|---|
| `emit_fixture.js` | Emit the 32-node comdet fixture as `edge.csv` + `com_gt.csv` (header + rows; binary's `node_id1,node_id2` and `node_id,cluster_id` formats). |
| `run_kernel.js` | Run a JS kernel (CC / WCC / CM) over the hard-coded fixture. |
| `run_kernel_csv.js` | Run a JS kernel over arbitrary `edge.csv` + `com.csv` input. Mirrors the binary's `GetOriginalToNewIdMap` insertion-order semantics so node-id mapping is byte-equal. |
| `diff_partitions.js` | Compare two clustering CSVs: identity check, ARI, overlap matrix. |

Inputs and binary output land in this dir but are gitignored; only the
harness scripts are tracked.

## Quickstart

```sh
# 1. Build the binary (once):
cd ../../constrained-clustering
cmake -B build -S .
cmake --build build -j

# 2. Emit fixture inputs:
cd ../tests/cd_verify
node emit_fixture.js .

# 3. Run binary + kernel + diff (CC):
BIN=../../constrained-clustering/build/bin/constrained_clustering
$BIN MincutOnly --edgelist edge.csv --existing-clustering com_gt.csv \
     --output-file out_cc.csv --log-file out_cc.log \
     --connectedness-criterion 0 --num-processors 1
node run_kernel.js cc js_cc.csv
node diff_partitions.js out_cc.csv js_cc.csv binary kernel
```

## Verified results (2026-05-04, 32-node fixture + dnc)

| algo | dataset | ARI | byte-equal cluster ids |
|---|---|---|---|
| CC | fixture | 1.000 | yes |
| CC | dnc | 1.000 | yes |
| WCC `1log_10(n)` | fixture | 1.000 | partition-equivalent (peel residual differs by 1 node) |
| WCC `1log_10(n)` | dnc | 0.995 | no — Stoer-Wagner vs VieCut tie-break drift |
| CM `1log_10(n)` leiden-cpm 0.0001 | fixture | 1.000 | partition-equivalent (lineage ids differ) |
| CM `1log_10(n)` leiden-cpm 0.0001 | dnc | 0.780 | no — JS Leiden diverges from C++ libleidenalg under RNG-driven re-cluster |

See `memory/cc_wcc_cm_kernel_verification.md` for the full report.

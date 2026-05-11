# Bumped 3-tier panel results (L4 byte-equal)

Panel: 161 networks (n ≤ 30000) × 50 seeds = 8050 cells per algo per variant.
Tier buckets: T1 n<3000 (117 nets), T2 3000≤n<15000 (31 nets), T3 15000≤n≤30000 (13 nets).
See [`_common/empirical_panel.py`](_common/empirical_panel.py) for the full panel.

## Run log (per algo, per variant — Phase B)

| algo    | variant                       | cells PASS / total | cumulative records  | wall (s) | log                          |
|---------|-------------------------------|--------------------|---------------------|----------|------------------------------|
| louvain | mod                           | 8050 / 8050        | 412,689,860 visits  | 3896.8   | `/tmp/louvain_mod_full.log`  |

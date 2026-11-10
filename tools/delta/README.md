# Kernel delta tooling

> 🚧 **Active development** — part of a library that grows regularly. New
> modules, stages and target profiles land as they pass their QEMU gates;
> interfaces and evidence are updated together.

Reproduce the file-to-file and feature delta between two kernel trees.

```sh
export KB_OLD=$HOME/realme-gt3-kernel        # vendor 5.10.236
export KB_NEW=$HOME/src/kernel/linux-7.4-rc2 # latest beta
export KB_OUT=$HOME/kernel-delta-out
python3 tools/delta/inventory_diff.py   # manifest + summary
python3 tools/delta/analyze_features.py # LOC/Kconfig/syscalls/new files
python3 tools/delta/line_delta.py       # per-top and per-file line diff
python3 tools/delta/build_maps.py       # DELTA-MAP/FEATURE-DELTA/BACKPORT-MAP/LEDGER
```

Outputs:
- `delta-manifest.tsv.gz` — every path: status + sizes.
- `file-line-delta.tsv.gz` — per-file line ±.
- `delta-summary.json`, `feature-delta.json`, `line-delta.json`.
- `DELTA-MAP.md`, `FEATURE-DELTA.md`, `BACKPORT-MAP.md`, `BACKPORT-LEDGER.tsv`.

`KB_OLD` must be a git tree for `git ls-files`; `KB_NEW` can be a tarball
extract. The scripts exclude build artifacts; they do not execute target code.

# HybridAcc

## Script Organization

All setup and helper scripts are centralized under `scripts/` with clear separation:

- `scripts/install/`: install/download scripts, one tool per script.
- `scripts/fast_entry/`: fast testbench/simulator entry scripts.
- `scripts/setup.sh`: unified shell entry for install/env/fast.

Backward-compatible wrappers are still kept in:

- `scripts/tools/` -> forwards to `scripts/install/`
- `scripts/sim/` and `scripts/test/` -> forwards to `scripts/fast_entry/`

## Unified Setup Entry

Use shell entry:

```bash
scripts/setup.sh all
```

Use Python CLI entry (from `pyproject.toml`):

```bash
uv run hacc-setup all
```

Common subcommands:

```bash
scripts/setup.sh install all
scripts/setup.sh install riscv --prefix $HOME/.local/riscv
scripts/setup.sh env --riscv-prefix $HOME/.local/riscv
scripts/setup.sh fast cluster-sim run conv_k3c4
```

## Sweep And E2E Workflow

Sweep YAMLs are generated on demand instead of being stored in git. A typical conv3x3 sweep flow is:

```bash
uv run hacc-sweep gen --workload conv3x3 --output-dir ./output/hacc-conv3x3-sweeps
./scripts/fast_entry/run_e2e.sh $(cat ./output/hacc-conv3x3-sweeps/lists/conv3x3_all.list) --output-dir ./output/hacc-conv3x3-results --skip-build --jobs "$(nproc)"
uv run hacc-sweep report --manifest ./output/hacc-conv3x3-sweeps/manifest.json --results-root ./output/hacc-conv3x3-results --output-dir ./output/hacc-conv3x3-report
```

When more than one workload is passed to `run_e2e.sh`, batch mode uses `nproc` workers by default and renders a live progress dashboard in the terminal. Use `--jobs N` to cap or override the worker count. Each workload keeps its full pipeline log under `<output-dir>/<case>/e2e_run.log`.

The sweep report now distinguishes two MAC utilization metrics:

- `core_level_macs_utilization_pct`: MAC utilization over the core-level interval from firmware start until EBREAK.
- `cluster_level_macs_utilization_pct`: MAC utilization over the cluster `RUN` interval measured directly inside the simulator.

The simulator writes `[SIM] Cluster RUN cycles: <n>` into `sim.log`, and `hacc-sweep report` consumes that value directly without requiring a trace dump.

For more detailed workload and sweep examples, see `design/hybridacc-cc/example/README.md`.

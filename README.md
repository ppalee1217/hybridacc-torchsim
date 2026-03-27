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

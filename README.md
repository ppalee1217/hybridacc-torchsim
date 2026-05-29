# HybridAcc

HybridAcc is a hardware/software co-design framework for accelerator research. The repo ties together four layers:

1. `design/hybridacc-cc/`: workload lowering and firmware generation for HybridAcc kernels.
2. `design/hybridacc-ESL/`: SystemC ESL simulator for fast functional bring-up and performance probes.
3. `design/hybridacc-RTL/`: SystemVerilog RTL, VCS simulation, synthesis, STA, PrimePower, and Jasper/Superlint flows.
4. `python/`: repo-wide Python CLIs for compile, verification, report parsing, sweep, and workflow helpers.

Start from [doc/index.md](doc/index.md) for the full manual split. User-facing commands live in [doc/user-manual/index.md](doc/user-manual/index.md); repo maintenance notes live in [doc/developer-manual/index.md](doc/developer-manual/index.md).

## Documentation Tree

- Repo entry: [doc/index.md](doc/index.md)
- User workflows: [doc/user-manual/index.md](doc/user-manual/index.md)
- Developer and repo maintenance: [doc/developer-manual/index.md](doc/developer-manual/index.md)
- Compiler design docs: [design/hybridacc-cc/doc/00_Overview.md](design/hybridacc-cc/doc/00_Overview.md)
- ESL subsystem docs: [design/hybridacc-ESL/doc/index.md](design/hybridacc-ESL/doc/index.md)
- RTL subsystem docs: [design/hybridacc-RTL/doc/README.md](design/hybridacc-RTL/doc/README.md)
- PE ISA toolchain docs: [design/hybridacc-pe-isa/README.md](design/hybridacc-pe-isa/README.md)
- Python CLI and parser docs: [doc/user-manual/python-cli-reference.md](doc/user-manual/python-cli-reference.md)

## Documentation Checks And Preview

Use the repo script to validate local markdown links, then launch the curated docs site from the repo root.

```bash
cd "$(git rev-parse --show-toplevel)"
uv run python scripts/check/validate_markdown_links.py
uv run python scripts/check/sync_docs_site.py
uvx --with mkdocs-material mkdocs serve --config-file mkdocs.yml
```

CI runs the same checks through [.github/workflows/docs.yml](.github/workflows/docs.yml).

## Quick Setup

Use `uv` for Python tooling.

```bash
cd "$(git rev-parse --show-toplevel)"
uv sync
scripts/setup.sh all
scripts/env_check.sh
```

The same setup flow is also exposed as a Python CLI:

```bash
uv run hacc-setup all
```

If you only want to check the host environment without probing EDA tools:

```bash
scripts/env_check.sh --no-eda
```

## Minimal ESL Run

Build the SystemC simulator and run one firmware task.

```bash
scripts/setup.sh fast hybridacc-sim build
uv run hacc-compile design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml -o output/quickstart-conv3x3 --dump-ir
uv run python -m hybridacc_verify.gen.gen_test_dram --ir output/quickstart-conv3x3/hardware_ir.json --workload design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml --output-dir output/quickstart-conv3x3
scripts/setup.sh fast hybridacc-sim run-task output/quickstart-conv3x3
uv run python -m hybridacc_verify.check.compare_golden output/quickstart-conv3x3
```

For larger batches, use the E2E runner and sweep tools:

```bash
uv run hacc-sweep gen --workload conv3x3 --output-dir output/hacc-conv3x3-sweeps
scripts/fast_entry/run_e2e.sh $(cat output/hacc-conv3x3-sweeps/lists/conv3x3_all.list) --output-dir output/hacc-conv3x3-results --skip-build --jobs "$(nproc)"
uv run hacc-sweep report --manifest output/hacc-conv3x3-sweeps/manifest.json --results-root output/hacc-conv3x3-results --output-dir output/hacc-conv3x3-report
```

## Minimal RTL Run

RTL and EDA flows should run through `tcsh` so the site VCS/Synopsys environment is loaded.

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make sim_tb_corecontroller_smoke'
```

The single-wave firmware regression exercises the top-level RTL firmware path:

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make rtl_regress_single_wave'
```

## Script Layout

- `scripts/setup.sh`: unified setup/install/fast-entry shell front door.
- `scripts/env_check.sh`: host, Python, SystemC, RISC-V, and optional EDA environment checker.
- `scripts/fast_entry/`: daily ESL/testbench shortcuts.
- `scripts/install/`: install/configuration helpers.
- `python/hybridacc_tools/`: packaged helper CLIs such as `hacc-e2e-monitor`, `hacc-flat-fw-mem`, and `hacc-wave-gap-summary`.

RTL script entrypoints are under `design/hybridacc-RTL/script/tcl/` and `design/hybridacc-RTL/script/python/`; top-level RTL Tcl/Python wrappers are not kept.

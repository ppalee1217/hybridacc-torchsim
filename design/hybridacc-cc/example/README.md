# HybridAcc-CC Example Workloads

This directory keeps only hand-authored base workloads and a small set of feature regressions.

## Base workloads

- `conv3x3/conv2d_3x3_single_wave.yaml`
- `conv3x3/conv2d_3x3_multi_wave.yaml`
- `conv1x1/conv2d_1x1_single_wave.yaml`
- `conv1x1/conv2d_1x1_multi_wave.yaml`
- `gemm/gemm_single_wave.yaml`
- `gemm/gemm_multi_wave.yaml`

## Feature regressions

- `test/test_conv3x3_pad1_h16.yaml`
- `test/test_conv3x3_pad1_relu_h16.yaml`
- `test/test_conv3x3_2layer_pad1.yaml`
- `test/test_isolation_2x_singlewave.yaml`
- `test/test_isolation_2x_multiwave.yaml`

## Sweep generation

Sweep YAMLs are no longer stored in git. Generate them into a scratch directory instead:

```bash
uv run hacc-sweep gen --workload conv3x3 --output-dir /tmp/hacc-conv3x3-sweeps
uv run hacc-sweep gen --workload conv1x1 --output-dir /tmp/hacc-conv1x1-sweeps
```

Each generated suite contains:

- `manifest.json`: case metadata for the parser/report step
- `lists/*.list`: absolute-path list files for `run_e2e.sh`
- `yaml/<dim>/*.yaml`: generated workloads grouped by sweep dimension

Run a generated suite and produce the HTML report:

```bash
./scripts/fast_entry/run_e2e.sh $(cat /tmp/hacc-conv3x3-sweeps/lists/conv3x3_all.list) --output-dir /tmp/hacc-conv3x3-results --skip-build
uv run hacc-sweep report --manifest /tmp/hacc-conv3x3-sweeps/manifest.json --results-root /tmp/hacc-conv3x3-results --output-dir /tmp/hacc-conv3x3-report
```

Padding is only valid for `conv2d_3x3` workloads. If you need padded sweeps, use `hacc-sweep gen --workload conv3x3 --padding 1`.
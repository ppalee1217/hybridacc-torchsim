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
uv run hacc-sweep gen --workload conv3x3 --output-dir ./output/hacc-conv3x3-sweeps
uv run hacc-sweep gen --workload conv1x1 --output-dir ./output/hacc-conv1x1-sweeps
uv run hacc-sweep gen --workload gemm --output-dir ./output/hacc-gemm-sweeps
```

By default, `hacc-sweep gen` uses `--mode product`, so `--dimensions oh,ow,ic,oc` generates every `OH x OW x IC x OC` combination.

If you want the old one-dimension-at-a-time behavior, use `--mode onehot`. In that mode, each listed dimension is swept independently while the other dimensions stay at the profile base shape.

Each generated suite contains:

- `manifest.json`: case metadata for the parser/report step
- `lists/*.list`: absolute-path list files for `run_e2e.sh`
- `yaml/<sweep-group>/*.yaml`: generated workloads grouped by the selected sweep set, for example `yaml/ohxoc/*.yaml` in `product` mode or `yaml/oh/*.yaml` and `yaml/oc/*.yaml` in `onehot` mode

Run a generated suite and produce the HTML report:

```bash
./scripts/fast_entry/run_e2e.sh $(cat ./output/hacc-conv3x3-sweeps/lists/conv3x3_all.list) --output-dir ./output/hacc-conv3x3-results --skip-build --jobs "$(nproc)"
uv run hacc-sweep report --manifest ./output/hacc-conv3x3-sweeps/manifest.json --results-root ./output/hacc-conv3x3-results --output-dir ./output/hacc-conv3x3-report
```

When multiple workloads are passed, `run_e2e.sh` enters batch mode. It uses `nproc` workers by default, shows a live progress dashboard in the terminal, and lets you cap concurrency with `--jobs N`. Each case writes its full pipeline log to `<output-dir>/<case>/e2e_run.log`.

## Report metrics

`hacc-sweep report` now exports both core-level and cluster-level MAC utilization:

- `core_level_macs_utilization_pct`: MAC utilization using `core_probe_cycles_total`; when available this aligns the denominator to `drain_out_end_cycle`, otherwise it falls back to the TB-visible EBREAK cycle.
- `cluster_level_macs_utilization_pct`: MAC utilization using only the cycles where the cluster control state is `RUN`.
- `macs_utilization_pct`: compatibility alias of `core_level_macs_utilization_pct` for older report consumers.

The simulator writes `[SIM] Cluster RUN cycles: <n>` to each `sim.log`, so the cluster-level metric is collected directly from the simulator without requiring trace files.

The generated HTML report renders a dedicated 3D scatter section for each utilization metric, with both linear-scale and log-scale color plots.

Padding is only valid for `conv2d_3x3` workloads. If you need padded sweeps, use `hacc-sweep gen --workload conv3x3 --padding 1`.
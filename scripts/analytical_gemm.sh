# uv run python -m python.analytical.cli \
#    --workload ./python/analytical/workload_examples/conv_case1.json \
#    --outdir ./output/conv_case1 \
#    --emit-hw-config \
#    --compact-ir \
#    --print_summary


uv run python -m python.analytical.cli \
    --workload ./python/analytical/workload_examples/gemm_case1.json \
    --outdir ./output/gemm_case1 \
    --emit-hw-config \
    --compact-ir \
    --print_summary
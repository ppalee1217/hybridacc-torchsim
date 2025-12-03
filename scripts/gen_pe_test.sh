#!/bin/bash

PYTHON="uv run"
ENTRYPOINT="python/utils/pe_sim_gen.py"

$PYTHON $ENTRYPOINT conv --mode "a" --out-ch 16 --in-width 800 --fmt "bin" --out-dir ./output/data/conv3x3 --seed 123 --no-ps
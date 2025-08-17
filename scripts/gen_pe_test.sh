#!/bin/bash

PYTHON="uv run"
ENTRYPOINT="python/utils/pe_sim_gen.py"

$PYTHON $ENTRYPOINT --mode "a" --out-ch 16 --in-width 800 --fmt "bin" --out-dir ./design/hybridacc-isa/data/conv3x3 --seed 123 --no-ps
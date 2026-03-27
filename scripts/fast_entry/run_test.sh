#!/bin/bash
PYTHON="uv run"
ENTRYPOINT="python/main.py"
TEST=convolution

if [ -z "$1" ]; then
    echo -e "\e[34mNo test specified. Running default test: $TEST\e[0m"
else
    TEST=$1
    echo -e "\e[34mRunning specified test: $TEST\e[0m"
fi

$PYTHON $ENTRYPOINT --test $TEST
if [ $? -ne 0 ]; then
    echo -e "\e[31mTest failed: $TEST\e[0m"
    exit 1
else
    echo -e "\e[32mTest passed: $TEST\e[0m"
fi
echo "All tests completed successfully."
# End of scripts/run_test.sh
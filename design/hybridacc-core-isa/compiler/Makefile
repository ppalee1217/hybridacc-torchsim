.PHONY: help install test clean compile examples

help:
	@echo "Core-ISA Compiler Makefile"
	@echo "=========================="
	@echo "Available targets:"
	@echo "  install    - Install dependencies and package in development mode"
	@echo "  test       - Run all tests"
	@echo "  test-unit  - Run unit tests only"
	@echo "  test-integration - Run integration tests"
	@echo "  clean      - Clean up generated files"
	@echo "  compile    - Compile example files"
	@echo "  examples   - Generate assembly for all examples"
	@echo "  format     - Format code with black"
	@echo "  lint       - Run flake8 linter"
	@echo "  type-check - Run mypy type checker"

install:
	pip install -r requirements.txt
	pip install -e .

test: test-unit test-integration

test-unit:
	python -m unittest discover tests -p "test_*.py" -v

test-integration:
	python tests/integration_test.py

clean:
	find . -type f -name "*.pyc" -delete
	find . -type d -name "__pycache__" -delete
	find . -type d -name "*.egg-info" -exec rm -rf {} +
	rm -f examples/*.asm

compile:
	@echo "Compiling simple example..."
	python -m src.main examples/simple.txt -o examples/simple.asm -v
	@echo "Compiling vector operations example..."
	python -m src.main examples/vector_ops.txt -o examples/vector_ops.asm -v

examples: compile
	@echo "Generated examples:"
	@ls -la examples/*.asm

format:
	black src/ tests/

lint:
	flake8 src/ tests/ --max-line-length=100

type-check:
	mypy src/ --ignore-missing-imports

all: install test examples

# Development shortcuts
dev-test: format lint test

# Debug targets
debug-tokens:
	python -m src.main examples/simple.txt --debug-tokens

debug-ast:
	python -m src.main examples/simple.txt --debug-ast
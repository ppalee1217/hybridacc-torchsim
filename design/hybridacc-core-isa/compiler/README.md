# Core-ISA Compiler

A compiler for the Core-ISA language that targets RISC-V RV32I assembly code.

## Features

- **Tokenizer/Lexer**: Converts source code into tokens
- **Parser**: Builds Abstract Syntax Tree (AST) from tokens
- **Code Generator**: Generates RISC-V RV32I assembly from AST
- **Vector Operations**: Supports vector load/store operations
- **Control Flow**: Supports for loops, if statements, and function calls

## Installation

```bash
# Install dependencies
pip install -r requirements.txt

# Install in development mode
pip install -e .
```

## Usage

### Command Line Interface

```bash
# Compile a source file
python -m src.main input.txt -o output.asm

# Debug tokenization
python -m src.main input.txt --debug-tokens

# Debug AST
python -m src.main input.txt --debug-ast

# Verbose output
python -m src.main input.txt -o output.asm --verbose
```

### Python API

```python
from src.main import Compiler

compiler = Compiler()

# Compile source code
source_code = """
let x 10
for i from 0 to x step 1 do
    let temp i * 2
endfor
"""

assembly = compiler.compile_source(source_code)
print(assembly)
```

## Language Syntax

### Variable Declaration
```
let variable_name value
let x 42
let addr 0x10000
```

### Vector Variables
```
vector data = VLOAD(address)
```

### For Loops
```
for variable from start to end step increment do
    # statements
endfor
```

### If Statements
```
if condition then
    # statements
else
    # statements
end
```

### Function Calls
```
VLOAD(address)
VSTORE(address, data)
```

### Arithmetic Operations
```
let result a + b * c
let diff x - y
```

### Comparison Operations
```
if x < 10 then
    # ...
end
```

## Supported Instructions

The compiler generates the following RISC-V instructions:

- **Arithmetic**: ADD, SUB, MUL, ADDI
- **Logical**: AND, OR, XOR, SLT, SLTU
- **Memory**: LDV (Load Vector), STV (Store Vector)
- **Control Flow**: BEQ, JMP, LOOP
- **Immediate**: LI, LUI
- **Pseudo**: NOP, MV

## Architecture

```
Source Code
     ↓
  Tokenizer (Lexical Analysis)
     ↓
   Parser (Syntax Analysis)
     ↓
    AST (Abstract Syntax Tree)
     ↓
Code Generator (Code Generation)
     ↓
  RISC-V Assembly
```

## Examples

See the `examples/` directory for sample programs:

- `simple.txt`: Basic arithmetic and control flow
- `vector_ops.txt`: Vector operations example

## Testing

```bash
# Run all tests
python -m unittest discover tests

# Run specific test
python -m unittest tests.test_tokenizer

# Run with coverage
pytest --cov=src tests/
```

## Development

### Project Structure

```
compiler/
├── src/
│   ├── __init__.py
│   ├── main.py              # Main compiler entry point
│   ├── tokenizer/           # Lexical analysis
│   │   ├── __init__.py
│   │   ├── token.py         # Token definitions
│   │   └── tokenizer.py     # Tokenizer implementation
│   ├── parser/              # Syntax analysis
│   │   ├── __init__.py
│   │   └── parser.py        # Parser implementation
│   ├── ast/                 # Abstract Syntax Tree
│   │   ├── __init__.py
│   │   └── nodes.py         # AST node definitions
│   └── codegen/             # Code generation
│       ├── __init__.py
│       └── generator.py     # Code generator
├── tests/                   # Unit tests
├── examples/                # Example programs
├── docs/                    # Documentation
├── requirements.txt         # Dependencies
└── setup.py                # Package configuration
```

### Adding New Features

1. **New Tokens**: Add to `TokenType` enum in `token.py`
2. **New AST Nodes**: Add to `nodes.py` and implement visitor methods
3. **New Grammar**: Update parser methods in `parser.py`
4. **New Instructions**: Add to code generator in `generator.py`

## License

This project is part of the HybridAcc research project.
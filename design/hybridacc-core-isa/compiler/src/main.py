#!/usr/bin/env python3
"""
Core-ISA Compiler
Compiles high-level language to RISC-V RV32I assembly
"""

import sys
import argparse
from pathlib import Path
from typing import Optional

# Handle both relative and absolute imports
try:
    from .tokenizer.tokenizer import Tokenizer
    from .parser.parser import Parser
    from .codegen.generator import CodeGenerator
except ImportError:
    from tokenizer.tokenizer import Tokenizer
    from parser.parser import Parser
    from codegen.generator import CodeGenerator

class Compiler:
    def __init__(self):
        self.tokenizer = None
        self.parser = None
        self.code_generator = None

    def compile_file(self, input_file: str, output_file: Optional[str] = None) -> str:
        """Compile a source file to assembly"""

        # Read input file
        try:
            with open(input_file, 'r', encoding='utf-8') as f:
                source_code = f.read()
        except FileNotFoundError:
            raise FileNotFoundError(f"Input file '{input_file}' not found")
        except Exception as e:
            raise Exception(f"Error reading input file: {e}")

        return self.compile_source(source_code, output_file)

    def compile_source(self, source_code: str, output_file: Optional[str] = None) -> str:
        """Compile source code to assembly"""

        try:
            # Tokenization
            print("Tokenizing...", file=sys.stderr)
            self.tokenizer = Tokenizer(source_code)
            tokens = self.tokenizer.tokenize()

            # Filter out comments for parsing (but keep them for debugging)
            filtered_tokens = [token for token in tokens if token.type.name != 'COMMENT']

            # Parsing
            print("Parsing...", file=sys.stderr)
            self.parser = Parser(filtered_tokens)
            ast = self.parser.parse()

            # Code generation
            print("Generating code...", file=sys.stderr)
            self.code_generator = CodeGenerator()
            assembly_code = self.code_generator.visit_program(ast)

            # Write output file if specified
            if output_file:
                try:
                    with open(output_file, 'w', encoding='utf-8') as f:
                        f.write(assembly_code)
                    print(f"Assembly written to '{output_file}'", file=sys.stderr)
                except Exception as e:
                    raise Exception(f"Error writing output file: {e}")

            return assembly_code

        except Exception as e:
            raise Exception(f"Compilation failed: {e}")

    def debug_tokens(self, source_code: str):
        """Debug: Print all tokens"""
        tokenizer = Tokenizer(source_code)
        tokens = tokenizer.tokenize()

        print("=== TOKENS ===")
        for token in tokens:
            print(token)
        print("=== END TOKENS ===\n")

    def debug_ast(self, source_code: str):
        """Debug: Print AST structure"""
        tokenizer = Tokenizer(source_code)
        tokens = tokenizer.tokenize()
        filtered_tokens = [token for token in tokens if token.type.name != 'COMMENT']

        parser = Parser(filtered_tokens)
        ast = parser.parse()

        print("=== AST ===")
        self._print_ast_node(ast, 0)
        print("=== END AST ===\n")

    def _print_ast_node(self, node, indent=0):
        """Helper method to print AST nodes recursively"""
        prefix = "  " * indent
        print(f"{prefix}{node.__class__.__name__}")

        for attr_name, attr_value in node.__dict__.items():
            if isinstance(attr_value, list):
                if attr_value and hasattr(attr_value[0], '__dict__'):
                    print(f"{prefix}  {attr_name}:")
                    for item in attr_value:
                        self._print_ast_node(item, indent + 2)
                else:
                    print(f"{prefix}  {attr_name}: {attr_value}")
            elif hasattr(attr_value, '__dict__'):
                print(f"{prefix}  {attr_name}:")
                self._print_ast_node(attr_value, indent + 2)
            else:
                print(f"{prefix}  {attr_name}: {attr_value}")

def main():
    """Main entry point for the compiler"""
    parser = argparse.ArgumentParser(
        description="Core-ISA Compiler - Compile to RISC-V RV32I Assembly",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  core-isa-compile input.txt -o output.asm    # Compile file
  core-isa-compile input.txt --debug-tokens   # Show tokens
  core-isa-compile input.txt --debug-ast      # Show AST
        """
    )

    parser.add_argument(
        'input_file',
        help='Input source file to compile'
    )

    parser.add_argument(
        '-o', '--output',
        help='Output assembly file (default: stdout)',
        default=None
    )

    parser.add_argument(
        '--debug-tokens',
        action='store_true',
        help='Print tokenization debug information'
    )

    parser.add_argument(
        '--debug-ast',
        action='store_true',
        help='Print AST debug information'
    )

    parser.add_argument(
        '--verbose', '-v',
        action='store_true',
        help='Enable verbose output'
    )

    args = parser.parse_args()

    if not Path(args.input_file).exists():
        print(f"Error: Input file '{args.input_file}' does not exist", file=sys.stderr)
        sys.exit(1)

    compiler = Compiler()

    try:
        # Read source code
        with open(args.input_file, 'r', encoding='utf-8') as f:
            source_code = f.read()

        # Debug modes
        if args.debug_tokens:
            compiler.debug_tokens(source_code)

        if args.debug_ast:
            compiler.debug_ast(source_code)

        # Compile
        assembly_code = compiler.compile_source(source_code, args.output)

        # Print to stdout if no output file specified
        if not args.output:
            print(assembly_code)

        if args.verbose:
            print("Compilation completed successfully!", file=sys.stderr)

    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    main()
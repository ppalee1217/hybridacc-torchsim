#!/usr/bin/env python3

import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'src'))

from main import Compiler

def test_simple_compilation():
    print("=== Testing Simple Compilation ===")
    compiler = Compiler()

    source = """
    let x 10
    let y 20
    let result x + y
    """

    try:
        assembly = compiler.compile_source(source)
        print("✓ Simple compilation successful")
        print("Generated assembly:")
        print(assembly[:500] + "..." if len(assembly) > 500 else assembly)
    except Exception as e:
        print(f"✗ Simple compilation failed: {e}")

def test_loop_compilation():
    print("\n=== Testing Loop Compilation ===")
    compiler = Compiler()

    source = """
    for i from 0 to 5 step 1 do
        let temp i * 2
    endfor
    """

    try:
        assembly = compiler.compile_source(source)
        print("✓ Loop compilation successful")
        # Check for expected instructions
        if "loop_start" in assembly and "SLT" in assembly:
            print("✓ Loop control structures generated correctly")
        else:
            print("✗ Missing expected loop control structures")
    except Exception as e:
        print(f"✗ Loop compilation failed: {e}")

def test_vector_operations():
    print("\n=== Testing Vector Operations ===")
    compiler = Compiler()

    source = """
    let addr 0x10000
    vector data = VLOAD(addr)
    VSTORE(addr, data)
    """

    try:
        assembly = compiler.compile_source(source)
        print("✓ Vector operations compilation successful")
        if "LDV" in assembly and "STV" in assembly:
            print("✓ Vector instructions generated correctly")
        else:
            print("✗ Missing expected vector instructions")
    except Exception as e:
        print(f"✗ Vector operations compilation failed: {e}")

def test_processing_pass_compilation():
    print("\n=== Testing PROCESSING_PASS Compilation ===")
    compiler = Compiler()

    # Read the original PROCESSING_PASS file
    processing_pass_file = os.path.join(os.path.dirname(__file__), '..', '..', 'PROCESSING_PASS')

    try:
        with open(processing_pass_file, 'r') as f:
            source = f.read()

        assembly = compiler.compile_source(source)
        print("✓ PROCESSING_PASS compilation successful")

        # Write output to a new file for comparison
        output_file = os.path.join(os.path.dirname(__file__), '..', 'examples', 'PROCESSING_PASS_compiled.asm')
        with open(output_file, 'w') as f:
            f.write(assembly)
        print(f"✓ Assembly written to {output_file}")

    except FileNotFoundError:
        print("✗ PROCESSING_PASS file not found, skipping this test")
    except Exception as e:
        print(f"✗ PROCESSING_PASS compilation failed: {e}")

if __name__ == "__main__":
    print("Core-ISA Compiler Test Suite")
    print("=" * 40)

    test_simple_compilation()
    test_loop_compilation()
    test_vector_operations()
    test_processing_pass_compilation()

    print("\n" + "=" * 40)
    print("Test suite completed!")
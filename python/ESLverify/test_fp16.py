import struct
import argparse

# FP16 <-> float conversion helpers
def fp16_to_float(h):
    """Convert 16-bit hex FP16 to Python float"""
    h = int(h, 16) if isinstance(h, str) else h
    s = (h >> 15) & 0x1
    e = (h >> 10) & 0x1F
    f = h & 0x3FF

    if e == 0:
        # subnormal
        val = (f / 1024) * 2**(-14)
    elif e == 0x1F:
        # Inf/NaN
        val = float('inf') if f == 0 else float('nan')
    else:
        val = (1 + f / 1024) * 2**(e - 15)

    return -val if s else val

def float_to_fp16(value):
    """Convert Python float to 16-bit FP16 hex"""
    if value == 0.0:
        return 0x0000
    s = 0 if value >= 0 else 1
    value = abs(value)
    e = int(value).bit_length() - 1 if value != 0 else 0
    # normalize
    exp = 0
    frac = 0
    import math
    exp = int(math.floor(math.log(value,2)))
    frac_val = value / (2**exp) - 1
    frac = int(round(frac_val * 1024))
    exp += 15
    if exp <= 0:
        # subnormal
        frac = int(round(value / 2**(-14) ))
        exp = 0
    elif exp >= 31:
        # overflow → inf
        exp = 31
        frac = 0
    h = (s << 15) | (exp << 10) | (frac & 0x3FF)
    return h

# FP16 adder
def fp16_add(a_hex, b_hex):
    a = fp16_to_float(a_hex)
    b = fp16_to_float(b_hex)
    result = a + b
    return hex(float_to_fp16(result))

def fp16_mul(a_hex, b_hex):
    a = fp16_to_float(a_hex)
    b = fp16_to_float(b_hex)
    result = a * b
    return hex(float_to_fp16(result))

# Example usage
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="FP16 Adder Test")
    parser.add_argument("a", type=str, help="First FP16 operand in hex (e.g., 0x3C00 for 1.0)")
    parser.add_argument("b", type=str, help="Second FP16 operand in hex (e.g., 0x4000 for 2.0)")

    args = parser.parse_args()
    a = int(args.a, 16)
    b = int(args.b, 16)
    print(f"{a} + {b} ≈ {fp16_add(a, b)}")
    print(f"{a} * {b} ≈ {fp16_mul(a, b)}")

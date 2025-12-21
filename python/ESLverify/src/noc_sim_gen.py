import argparse
import json
import numpy as np
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Any
from dataclasses import dataclass, asdict, field
import torch
import torch.nn.functional as F

# Constants matching C++ definitions
class PERouterMode:
    PLI_FROM_LN_PLO_TO_LN = 0
    PLI_FROM_BUS_PLO_TO_LN = 1
    PLI_FROM_LN_PLO_TO_BUS = 2
    PLI_FROM_BUS_PLO_TO_BUS = 3

@dataclass
class ScanChainConfig:
    ps_id: int
    pd_id: int
    pli_id: int
    plo_id: int
    route_mode: int
    enable: bool = True

    def pack(self) -> int:
        """Pack the configuration into a 32-bit integer matching ScanChainFormat."""
        data = 0
        data |= (self.ps_id & 0x3F) << 4
        data |= (self.pd_id & 0x3F) << 10
        data |= (self.pli_id & 0x3F) << 16
        data |= (self.plo_id & 0x3F) << 22
        data |= (self.route_mode & 0x03) << 28
        if self.enable:
            data |= (1 << 30)
        return data

@dataclass
class TestData:
    name: str
    description: str
    inputs: Dict[str, np.ndarray]
    outputs: Dict[str, np.ndarray]
    scan_chain: List[int]  # Packed scan chain data for each PE
    config: Dict[str, Any] = field(default_factory=dict)
    datatype: np.dtype = np.float16

    def save(self, output_dir: Path):
        output_dir = Path(output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)

        # Save tensors as .bin (raw float16)
        for name, data in self.inputs.items():
            data.astype(self.datatype).tofile(output_dir / f"input_{name}.bin")
        for name, data in self.outputs.items():
            data.astype(self.datatype).tofile(output_dir / f"output_{name}.bin")

        # Save scan chain as .bin (int32)
        np.array(self.scan_chain, dtype=np.int32).tofile(output_dir / "scan_chain.bin")

        # Save config.txt
        with open(output_dir / "config.txt", "w") as f:
            # Write config items
            for k, v in self.config.items():
                f.write(f"{k}:{v}\n")

            f.write("format:bin\n")
            f.write("layout:channels_last\n")

            # Write shapes
            for name, data in self.inputs.items():
                f.write(f"input_{name}_shape:{str(data.shape)}\n")
            for name, data in self.outputs.items():
                f.write(f"output_{name}_shape:{str(data.shape)}\n")

            f.write(f"scan_chain_length:{len(self.scan_chain)}\n")

        print(f"Test case '{self.name}' saved to {output_dir}")

def generate_conv2d_test(num_pes: int = 64, num_bus: int = 4, stride = 1) -> TestData:
    """
    Generate Conv2d test case.
    Spec:
    1. conv2d 3x3, stride 1, padding 0
    input activation (NHWC): (1,18,200,4)
    weight (OC, H, W, C): (16,3,3,4)
    input partial sum (NHWC): (1,16,198,16)
    output partial sum (NHWC): (1,16,198,16)
    """
    print("Generating Conv2d test data...")

    # Shapes from spec
    N, H, W, C = 1, 18, 200, 4
    OC, KH, KW, _ = 16, 3, 3, 4

    # Generate random data
    input_act = torch.randn(N, H, W, C)
    weight = torch.randn(OC, KH, KW, C)
    input_ps = torch.randn(1, 16, 198, 16)

    # PyTorch expects NCHW for input and (OC, C, KH, KW) for weight
    input_nchw = input_act.permute(0, 3, 1, 2) # (N, C, H, W)
    weight_nchw = weight.permute(0, 3, 1, 2)   # (OC, C, KH, KW)
    input_ps_nchw = input_ps.permute(0, 3, 1, 2) # (N, OC, H_out, W_out)

    # Calculate Conv2d
    output_conv = F.conv2d(input_nchw, weight_nchw, stride=1, padding=0)

    # Add input partial sum
    output_final = output_conv + input_ps_nchw

    # Convert back to NHWC
    output_final_nhwc = output_final.permute(0, 2, 3, 1)

    # Prepare numpy arrays
    inputs = {
        "activation": input_act.numpy(),
        "weight": weight.numpy(),
        "partial_sum": input_ps.numpy()
    }
    outputs = {
        "partial_sum": output_final_nhwc.numpy()
    }

    scan_chain = []

    def get_route_mode(row_idx: int, kh: int) -> int:
        if row_idx == 0: # First row
            return PERouterMode.PLI_FROM_BUS_PLO_TO_LN
        elif row_idx == kh-1: # Last row
            return PERouterMode.PLI_FROM_LN_PLO_TO_BUS
        else: # Middle rows
            return PERouterMode.PLI_FROM_LN_PLO_TO_LN

    num_pes_per_bus = num_pes // num_bus
    for i in range(num_bus):
        for j in range(num_pes_per_bus):
            cfg = ScanChainConfig(
                ps_id=i,
                pd_id=(i+j)*stride,
                pli_id=j if i==0 else 63,
                plo_id=j if i==KH-1 else 63,
                route_mode=get_route_mode(i, KH),
                enable=True if i<KH else False
            )
            scan_chain.append(cfg.pack())

    config = {
        "mode": "conv2d",
        "kernel_size": KH,
        "in_ch": C,
        "stride": 1,
        "out_ch": OC,
        "in_height": H,
        "in_width": W,
        "out_height": output_final_nhwc.shape[1],
        "out_width": output_final_nhwc.shape[2],
        "partial_sum_zero": False,
        "seed": 123
    }

    return TestData(
        name="conv2d_3x3",
        description="Conv2d 3x3, stride 1, padding 0",
        inputs=inputs,
        outputs=outputs,
        scan_chain=scan_chain,
        config=config
    )

def generate_gemm_test(num_pes: int = 64) -> TestData:
    """
    Generate GEMM test case.
    Size suggestion: M=32, N=32, K=32
    C = A * B + D (Input PS)
    """
    print("Generating GEMM test data...")

    M, N, K = 32, 32, 32

    # Generate random data
    A = torch.randn(M, K)
    B = torch.randn(K, N)
    D = torch.randn(M, N) # Input PS

    # Calculate GEMM
    C = torch.matmul(A, B) + D

    inputs = {
        "A": A.numpy(),
        "B": B.numpy(),
        "D": D.numpy()
    }
    outputs = {
        "C": C.numpy()
    }

    scan_chain = []
    for i in range(num_pes):
        cfg = ScanChainConfig(
            ps_id=i, pd_id=i, pli_id=i, plo_id=i,
            route_mode=PERouterMode.PLI_FROM_BUS_PLO_TO_BUS, enable=True
        )
        scan_chain.append(cfg.pack())

    config = {
        "mode": "gemm",
        "M": M,
        "N": N,
        "K": K,
        "partial_sum_zero": False,
        "seed": 123
    }

    return TestData(
        name="gemm_32x32x32",
        description="GEMM M=32, N=32, K=32",
        inputs=inputs,
        outputs=outputs,
        scan_chain=scan_chain,
        config=config
    )

def main():
    parser = argparse.ArgumentParser(description="Generate NoC/PE simulation test data")
    parser.add_argument("--output-dir", type=str, default="output/noc_test_data", help="Directory to save test data")
    parser.add_argument("--num-pes", type=int, default=64, help="Total number of PEs")
    parser.add_argument("--num-bus", type=int, default=4, help="Total number of buses")
    parser.add_argument("--seed", type=int, default=123, help="Random seed")
    args = parser.parse_args()

    torch.manual_seed(args.seed)
    np.random.seed(args.seed)

    output_path = Path(args.output_dir)

    # Generate Conv2d
    conv_test = generate_conv2d_test(args.num_pes, args.num_bus)
    conv_test.save(output_path / "conv2d")

    # Generate GEMM
    gemm_test = generate_gemm_test(args.num_pes)
    gemm_test.save(output_path / "gemm")

if __name__ == "__main__":
    main()

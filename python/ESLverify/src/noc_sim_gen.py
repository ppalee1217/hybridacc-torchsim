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

    def save(self, output_dir: Path):
        output_dir = Path(output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)

        # Save tensors as .npy
        for name, data in self.inputs.items():
            np.save(output_dir / f"input_{name}.npy", data)
        for name, data in self.outputs.items():
            np.save(output_dir / f"output_{name}.npy", data)

        # Save metadata and scan chain
        meta = {
            "name": self.name,
            "description": self.description,
            "scan_chain": self.scan_chain,
            "config": self.config,
            "tensor_shapes": {
                "inputs": {k: v.shape for k, v in self.inputs.items()},
                "outputs": {k: v.shape for k, v in self.outputs.items()}
            }
        }
        with open(output_dir / "test_config.json", "w") as f:
            json.dump(meta, f, indent=2)
        print(f"Test case '{self.name}' saved to {output_dir}")

def generate_conv2d_test(num_pes: int = 64) -> TestData:
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

    # Generate random data (float32 for calculation, will need conversion for simulator)
    # Using integers to make verification easier/exact if needed, or small floats
    input_act = torch.randn(N, H, W, C)
    weight = torch.randn(OC, KH, KW, C)
    input_ps = torch.randn(1, 16, 198, 16) # Spec says (1, 16, 198, 16) which is (N, H_out, W_out, OC)

    # PyTorch expects NCHW for input and (OC, C, KH, KW) for weight
    input_nchw = input_act.permute(0, 3, 1, 2) # (N, C, H, W)
    weight_nchw = weight.permute(0, 3, 1, 2)   # (OC, C, KH, KW)
    input_ps_nchw = input_ps.permute(0, 3, 1, 2) # (N, OC, H_out, W_out)

    # Calculate Conv2d
    # stride=1, padding=0
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

    # Generate Scan Chain Configuration
    # Assumption: 64 PEs available.
    # We have 16 Output Channels.
    # Mapping: Assign 1 PE per Output Channel? Or distribute?
    # For simplicity, let's assign PE[i] to handle OC[i] for i in 0..15.
    # The other PEs (16..63) are unused or can duplicate work.

    scan_chain = []
    for i in range(num_pes):
        # Default config
        cfg = ScanChainConfig(
            ps_id=i,      # Unique PS ID
            pd_id=i,      # Unique PD ID
            pli_id=i,     # Unique PLI ID
            plo_id=i,     # Unique PLO ID
            route_mode=PERouterMode.PLI_FROM_BUS_PLO_TO_BUS, # Independent PEs
            enable=True
        )

        # For the first 16 PEs, we might want a specific mapping
        # But for now, unique IDs allow full flexibility in the testbench
        # to send packets to specific PEs.

        scan_chain.append(cfg.pack())

    return TestData(
        name="conv2d_3x3",
        description="Conv2d 3x3, stride 1, padding 0. Input (1,18,200,4), OC=16.",
        inputs=inputs,
        outputs=outputs,
        scan_chain=scan_chain,
        config={"N": N, "H": H, "W": W, "C": C, "OC": OC, "KH": KH, "KW": KW}
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

    # Scan Chain Config
    # For GEMM, we might use a systolic array configuration?
    # Or just independent PEs computing blocks?
    # For this data generation, we just provide the data and a default scan chain.
    # The mapping logic (how A/B are split) depends on the simulator/compiler.
    # We provide unique IDs so the testbench can load data to any PE.

    scan_chain = []
    for i in range(num_pes):
        cfg = ScanChainConfig(
            ps_id=i,
            pd_id=i,
            pli_id=i,
            plo_id=i,
            route_mode=PERouterMode.PLI_FROM_BUS_PLO_TO_BUS,
            enable=True
        )
        scan_chain.append(cfg.pack())

    return TestData(
        name="gemm_32x32x32",
        description="GEMM M=32, N=32, K=32",
        inputs=inputs,
        outputs=outputs,
        scan_chain=scan_chain,
        config={"M": M, "N": N, "K": K}
    )

def main():
    parser = argparse.ArgumentParser(description="Generate NoC/PE simulation test data")
    parser.add_argument("--output-dir", type=str, default="output/noc_test_data", help="Directory to save test data")
    parser.add_argument("--num-pes", type=int, default=64, help="Total number of PEs")
    args = parser.parse_args()

    output_path = Path(args.output_dir)

    # Generate Conv2d
    conv_test = generate_conv2d_test(args.num_pes)
    conv_test.save(output_path / "conv2d")

    # Generate GEMM
    gemm_test = generate_gemm_test(args.num_pes)
    gemm_test.save(output_path / "gemm")

if __name__ == "__main__":
    main()

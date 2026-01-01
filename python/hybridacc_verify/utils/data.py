from dataclasses import dataclass, field
from typing import Dict, List, Any
import numpy as np
from pathlib import Path

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

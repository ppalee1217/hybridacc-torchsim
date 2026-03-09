from dataclasses import dataclass, field
from typing import Dict, List, Any
import numpy as np
from pathlib import Path
from .config import ScanChainConfig, PERouterMode

# for NoC test data
@dataclass
class TestData:
    name: str
    description: str
    inputs: Dict[str, np.ndarray]
    outputs: Dict[str, np.ndarray]
    scan_chain: List[ScanChainConfig]  # Packed scan chain data for each PE
    config: Dict[str, Any] = field(default_factory=dict)
    datatype: np.dtype = np.float16

    def _save_config(self, output_dir: Path):
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

            # Write datatype
            f.write(f"datatype:{self.datatype}\n")

            # Write scan chain info
            f.write(f"scan_chain_length:{len(self.scan_chain)}\n")

    def _save_scan_chain(self, output_dir: Path):
        # Save scan chain as .bin (int32)
        np.array([i.pack() for i  in self.scan_chain], dtype=np.int32).tofile(output_dir / "scan_chain.bin")

        # Save scan chain as .txt for human readability
        with open(output_dir / "scan_chain.txt", "w") as f:
            # Scan-chain table header
            f.write("scan_chain_table\n\n|{:6s}|{:6s}|{:6s}|{:6s}|{:6s}|{:10s}|{:6s}|\n".format(
                " ","ps_id", "pd_id", "pli_id", "plo_id", "route_mode", "enable"))
            # Scan-chain entries
            for idx, sc in enumerate(self.scan_chain):
                f.write("|{:6d}|{:6d}|{:6d}|{:6d}|{:6d}|{:10s}|{:6s}|\n".format(
                    idx, sc.ps_id, sc.pd_id, sc.pli_id, sc.plo_id,
                    PERouterMode.to_symbol(sc.route_mode), "@" if sc.enable else "-"))

    def _save_tensors(self, output_dir: Path):
        # Save tensors as .bin (raw float16)
        for name, data in self.inputs.items():
            data.astype(self.datatype).tofile(output_dir / f"input_{name}.bin")
        for name, data in self.outputs.items():
            data.astype(self.datatype).tofile(output_dir / f"output_{name}.bin")

    def save(self, output_dir: Path):
        output_dir = Path(output_dir)
        output_dir.mkdir(parents=True, exist_ok=True)

        self._save_tensors(output_dir)
        self._save_config(output_dir)
        self._save_scan_chain(output_dir)

        print(f"Test case '{self.name}' saved to {output_dir}")


import json
@dataclass
class ClusterTestData(TestData):
    name: str
    description: str
    inputs: Dict[str, np.ndarray]
    outputs: Dict[str, np.ndarray]
    scan_chain: List[ScanChainConfig]  # Packed scan chain data for each PE
    config: Dict[str, Any] = field(default_factory=dict)
    datatype: np.dtype = np.float16

    # override to save config as JSON for better structure
    def _save_config(self, output_dir: Path):
        with open(output_dir / "config.json", "w") as f:
            json.dump(self.config, f, indent=4)

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
from typing import Dict, List, Optional

import pandas as pd


@dataclass(frozen=True)
class ParseResult:
    file_path: str
    metrics: Dict[str, float]


class NocSimLogParser:
    """Parser for NoC simulator output logs."""

    KEY_PATTERNS = {
        "total_elements": r"Total Elements:\s*(\d+)",
        "mismatches": r"Mismatches:\s*(\d+)",
        "tolerance": r"Tolerance:\s*([+\-0-9.eE]+)",
        "cosine_similarity": r"Cosine Similarity:\s*([+\-0-9.eE]+)",
        "max_difference": r"Max Difference:\s*([+\-0-9.eE]+)",
        "mse": r"MSE:\s*([+\-0-9.eE]+)",
        "pe_avg_instruction_count": r"PEs Average instruction count:\s*([+\-0-9.eE]+)",
        "pe_avg_cycle_count": r"PEs Average cycle count:\s*([+\-0-9.eE]+)",
        "pe_avg_ipc": r"PEs Average IPC:\s*([+\-0-9.eE]+)",
        "noc_cycle_count": r"Total NoC cycle count:\s*([+\-0-9.eE]+)",
        "clock_rate_ghz": r"Clock rate:\s*([+\-0-9.eE]+)\s*GHz",
        "noc_data_movement_kb": r"Total NoC data movement:\s*([+\-0-9.eE]+)\s*KB",
        "noc_throughput_gbs": r"NoC Throughput:\s*([+\-0-9.eE]+)\s*GB/s",
        "performance_gmacs": r"Performance \(MACs per second\):\s*([+\-0-9.eE]+)\s*GMACs/s",
        "performance_gflops": r"Performance \(MACs per second\):\s*[+\-0-9.eE]+\s*GMACs/s\(([+\-0-9.eE]+)\s*GFLOPS\)",
        "arithmetic_intensity": r"Arithmetic Intensity:\s*([+\-0-9.eE]+)",
    }

    def __init__(self, parse_type: str = "noc_sim"):
        if parse_type != "noc_sim":
            raise ValueError(f"Unsupported parse type: {parse_type}")
        self.parse_type = parse_type

    @staticmethod
    def list_log_files(folder: str | Path) -> List[Path]:
        root = Path(folder)
        if not root.exists() or not root.is_dir():
            raise FileNotFoundError(f"Folder not found: {root}")
        candidates = [
            path
            for path in root.iterdir()
            if path.is_file() and path.suffix.lower() == ".log"
        ]
        return sorted(candidates, key=lambda p: p.name)

    def parse_file(self, file_path: str | Path) -> ParseResult:
        path = Path(file_path)
        if not path.exists() or not path.is_file():
            raise FileNotFoundError(f"Log file not found: {path}")

        text = path.read_text(encoding="utf-8", errors="ignore")
        metrics: Dict[str, float] = {}

        for key, pattern in self.KEY_PATTERNS.items():
            match = re.search(pattern, text)
            if match:
                value = self._parse_numeric(match.group(1))
                if value is not None:
                    metrics[key] = value

        metrics["reported_mismatch_lines"] = float(
            len(re.findall(r"^Mismatch at index", text, flags=re.MULTILINE))
        )

        test_data_dir = self._extract_test_data_dir(text)
        if test_data_dir:
            metrics["_test_data_dir"] = test_data_dir

        passed = "PASSED" in text
        failed = "FAILED" in text
        metrics["passed"] = 1.0 if passed and not failed else 0.0

        return ParseResult(file_path=str(path), metrics=metrics)

    def parse_files(self, files: List[str | Path]) -> pd.DataFrame:
        rows: List[Dict[str, object]] = []
        for file_path in files:
            result = self.parse_file(file_path)
            row: Dict[str, object] = {
                "file": Path(result.file_path).name,
                "file_path": result.file_path,
            }
            row.update(result.metrics)
            rows.append(row)

        if not rows:
            return pd.DataFrame(columns=["file", "file_path"])

        df = pd.DataFrame(rows)
        if "_test_data_dir" in df.columns:
            df = df.rename(columns={"_test_data_dir": "test_data_dir"})
        return df

    def parse_folder(self, folder: str | Path) -> pd.DataFrame:
        files = self.list_log_files(folder)
        return self.parse_files(files)

    @staticmethod
    def numeric_columns(df: pd.DataFrame) -> List[str]:
        if df.empty:
            return []
        return [
            col
            for col in df.select_dtypes(include=["number"]).columns.tolist()
            if col != "passed"
        ]

    @staticmethod
    def _extract_test_data_dir(text: str) -> Optional[str]:
        match = re.search(r"\[TB\]\s+Loading test data from\s+(.+)", text)
        return match.group(1).strip() if match else None

    @staticmethod
    def _parse_numeric(value: str) -> Optional[float]:
        try:
            return float(value)
        except (TypeError, ValueError):
            return None
from dataclasses import dataclass
from typing import Optional

MAX_INPUT_WIDTH = 800

SUPPORTED_CONV_MODES_DICR =  {
            3: 4,
            5: 2,
            7: 1,
            1: 12
        }
SUPPORTED_CONV_MODES = [f'k{k}c{c}' for k, c in SUPPORTED_CONV_MODES_DICR.items()]

class ConvMode:
    K = 3
    C = 4
    def __str__(self) -> str:
        return f'k{self.K}c{self.C}'
    def __call__(self, mode_str: str) -> None:
        # Parse mode string like 'k3c4'
        if not mode_str.startswith('k') or 'c' not in mode_str:
            raise ValueError(f'Invalid mode string: {mode_str}')
        k_part, c_part = mode_str[1:].split('c')
        self.K = int(k_part)
        self.C = int(c_part)
    @staticmethod
    def channels_from_kernel_size(k: int) -> int:
        if k not in SUPPORTED_CONV_MODES_DICR:
            raise ValueError(f"Unsupported kernel size: {k}")
        return SUPPORTED_CONV_MODES_DICR[k]
class PERouterMode:
    PLI_FROM_LN_PLO_TO_LN = 0
    PLI_FROM_BUS_PLO_TO_LN = 1
    PLI_FROM_LN_PLO_TO_BUS = 2
    PLI_FROM_BUS_PLO_TO_BUS = 3

    def to_str(mode: int) -> str:
        mapping = {
            PERouterMode.PLI_FROM_LN_PLO_TO_LN: "PLI_FROM_LN_PLO_TO_LN",
            PERouterMode.PLI_FROM_BUS_PLO_TO_LN: "PLI_FROM_BUS_PLO_TO_LN",
            PERouterMode.PLI_FROM_LN_PLO_TO_BUS: "PLI_FROM_LN_PLO_TO_BUS",
            PERouterMode.PLI_FROM_BUS_PLO_TO_BUS: "PLI_FROM_BUS_PLO_TO_BUS",
        }
        return mapping.get(mode, "UNKNOWN_MODE")

    def to_symbol(mode: int) -> str:
        mapping = {
            PERouterMode.PLI_FROM_LN_PLO_TO_LN: "(IL, OL)",
            PERouterMode.PLI_FROM_BUS_PLO_TO_LN: "(IB, OL)",
            PERouterMode.PLI_FROM_LN_PLO_TO_BUS: "(IL, OB)",
            PERouterMode.PLI_FROM_BUS_PLO_TO_BUS: "(IB, OB)",
        }
        return mapping.get(mode, "?")

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
class ConvConfig:
    """Conv1D 配置"""
    mode: str
    out_ch: int
    in_width: int
    fmt: str = 'bin'
    seed: int = 0
    no_ps: bool = False
    layout: str = 'channels_last'
    out_dir: str = './output'

    def validate(self):
        if self.mode not in SUPPORTED_CONV_MODES:
            raise ValueError(f"mode 必須為 {SUPPORTED_CONV_MODES}, 收到: {self.mode}")

        conv_mode = ConvMode()
        conv_mode(self.mode)
        if not (1 <= self.out_ch <= 16):
            raise ValueError(f"out_ch 範圍 1~16, 收到: {self.out_ch}")
        if not (3 <= self.in_width <= MAX_INPUT_WIDTH):
            raise ValueError(f"in_width 範圍 3~800, 收到: {self.in_width}")
        if self.layout not in ('channels_first', 'channels_last'):
            raise ValueError(f"layout 需為 channels_first 或 channels_last, 收到: {self.layout}")
        if self.fmt not in ('bin', 'hex'):
            raise ValueError(f"fmt 需為 bin 或 hex, 收到: {self.fmt}")


@dataclass
class GemmConfig:
    """GEMM 配置"""
    out_width: int
    in_width: int
    dim: int
    fmt: str = 'bin'
    seed: int = 0
    no_ps: bool = False
    out_dir: str = './output'

    def validate(self):
        if not (1 <= self.out_width <= MAX_INPUT_WIDTH):
            raise ValueError(f"out_width 範圍 1~800, 收到: {self.out_width}")
        if not (3 <= self.in_width <= MAX_INPUT_WIDTH):
            raise ValueError(f"in_width 範圍 3~800, 收到: {self.in_width}")
        if self.dim < 1:
            raise ValueError(f"dim 必須 >= 1, 收到: {self.dim}")
        if self.fmt not in ('bin', 'hex'):
            raise ValueError(f"fmt 需為 bin 或 hex, 收到: {self.fmt}")

@dataclass
class NocConvConfig:
    """NoC Conv2d Configuration"""
    num_pes: int
    num_bus: int
    stride: int
    input_h: int
    input_w: int
    input_c: int
    out_ch: int
    kernel_h: int
    kernel_w: int
    seed: int = 123
    padding: int = 0
    ultra_mode: bool = False
    mode: str = "conv2d" # To identify the task type in config file
    out_dir: str = "./output/conv2d"

    def validate(self):
        if self.num_pes <= 0:
            raise ValueError(f"num_pes must be > 0, got {self.num_pes}")
        if self.num_bus <= 0:
            raise ValueError(f"num_bus must be > 0, got {self.num_bus}")
        if self.stride <= 0:
            raise ValueError(f"stride must be > 0, got {self.stride}")

@dataclass
class NocGemmConfig:
    """NoC GEMM Configuration"""
    num_pes: int
    num_bus: int
    M: int
    N: int
    K: int
    seed: int = 123
    mode: str = "gemm"
    ultra_mode: bool = False
    out_dir: str = "./output/gemm"

    def validate(self):
        if self.num_pes <= 0:
            raise ValueError(f"num_pes must be > 0, got {self.num_pes}")
        if self.M <= 0 or self.N <= 0 or self.K <= 0:
            raise ValueError(f"M, N, K must be > 0, got {self.M}, {self.N}, {self.K}")

import json
from pathlib import Path
from typing import Dict

try:
    import yaml
    HAS_YAML = True
except ImportError:
    HAS_YAML = False

class ConfigLoader:
    """配置文件載入器"""

    @staticmethod
    def load_from_json(path: Path) -> Dict:
        """從 JSON 載入配置"""
        with path.open('r', encoding='utf-8') as f:
            return json.load(f)

    @staticmethod
    def load_from_yaml(path: Path) -> Dict:
        """從 YAML 載入配置"""
        if not HAS_YAML:
            raise ImportError("請安裝 PyYAML: pip install pyyaml")
        with path.open('r', encoding='utf-8') as f:
            return yaml.safe_load(f)

    @staticmethod
    def load(path: Path) -> Dict:
        """自動偵測並載入配置文件"""
        if not path.exists():
            raise FileNotFoundError(f"配置文件不存在: {path}")

        suffix = path.suffix.lower()
        if suffix == '.json':
            return ConfigLoader.load_from_json(path)
        elif suffix in ('.yaml', '.yml'):
            return ConfigLoader.load_from_yaml(path)
        else:
            raise ValueError(f"不支援的配置文件格式: {suffix}")

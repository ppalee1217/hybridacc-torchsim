from dataclasses import dataclass
from typing import Optional

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
        valid_modes = ['k3c4', 'k5c2', 'k7c1', 'k1c12']
        if self.mode not in valid_modes:
            raise ValueError(f"mode 必須為 {valid_modes}，收到: {self.mode}")
        if not (1 <= self.out_ch <= 16):
            raise ValueError(f"out_ch 範圍 1~16，收到: {self.out_ch}")
        if not (3 <= self.in_width <= 800):
            raise ValueError(f"in_width 範圍 3~800，收到: {self.in_width}")
        if self.layout not in ('channels_first', 'channels_last'):
            raise ValueError(f"layout 需為 channels_first 或 channels_last，收到: {self.layout}")
        if self.fmt not in ('bin', 'hex'):
            raise ValueError(f"fmt 需為 bin 或 hex，收到: {self.fmt}")


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
        if not (1 <= self.out_width <= 800):
            raise ValueError(f"out_width 範圍 1~800，收到: {self.out_width}")
        if not (3 <= self.in_width <= 800):
            raise ValueError(f"in_width 範圍 3~800，收到: {self.in_width}")
        if self.dim < 1:
            raise ValueError(f"dim 必須 >= 1，收到: {self.dim}")
        if self.fmt not in ('bin', 'hex'):
            raise ValueError(f"fmt 需為 bin 或 hex，收到: {self.fmt}")

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
        suffix = path.suffix.lower()
        if suffix == '.json':
            return ConfigLoader.load_from_json(path)
        elif suffix in ('.yaml', '.yml'):
            return ConfigLoader.load_from_yaml(path)
        else:
            raise ValueError(f"不支援的配置文件格式: {suffix}")

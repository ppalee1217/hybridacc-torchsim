"""
HybridAcc PE 測試資料生成工具 (fp16)

支援功能:
1. Conv1D 模式 (k3c4/k5c2/k7c1/k1c12)
2. GEMM 模式
3. 支援 JSON/YAML 配置文件輸入
4. 支援 channels_first/channels_last layout
"""

import argparse
import json
import numpy as np
from pathlib import Path
from typing import Dict, Optional, Tuple
from dataclasses import dataclass, asdict
import torch
import torch.nn.functional as F

try:
    import yaml
    HAS_YAML = True
except ImportError:
    HAS_YAML = False


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


class DataGenerator:
    """資料生成器基類"""

    MODES = {
        # 直觀命名: k{kernel_size}c{in_channels}
        'k3c4': dict(kernel_size=3, in_ch=4, stride=1),
        'k5c2': dict(kernel_size=5, in_ch=2, stride=1),
        'k7c1': dict(kernel_size=7, in_ch=1, stride=2),
        'k1c12': dict(kernel_size=1, in_ch=12, stride=1),
    }

    @staticmethod
    def save_array(arr: np.ndarray, path: Path, fmt: str):
        """儲存陣列為 bin 或 hex 格式"""
        arr16 = arr.astype(np.float16)
        if fmt == 'bin':
            path.write_bytes(arr16.tobytes())
        else:
            u16 = arr16.view(np.uint16).reshape(-1)
            with path.open('w') as f:
                for v in u16:
                    f.write(f"{v:04x}\n")

    @staticmethod
    def conv1d_valid(act: np.ndarray, weight: np.ndarray, stride: int) -> np.ndarray:
        """執行 1D 卷積運算"""
        # act: (C_in, W_in), weight: (C_out, C_in, K)
        act_torch = torch.tensor(act[None, :, :], dtype=torch.float32)
        weight_torch = torch.tensor(weight, dtype=torch.float32)
        out_torch = F.conv1d(act_torch, weight_torch, stride=stride)
        return out_torch.numpy()[0]

    @staticmethod
    def pack_weight_mode_b(weight_cf: np.ndarray, layout: str) -> np.ndarray:
        """Mode B 權重打包"""
        C_out, C_in, K = weight_cf.shape
        assert (C_in, K) == (2, 5), "mode b 預期 C_in=2 且 K=5"

        packed = []
        for co in range(C_out):
            seq = weight_cf[co].reshape(-1)
            out12 = np.empty(12, dtype=seq.dtype)
            out12[0] = seq[0]
            out12[1] = seq[3]
            out12[2] = seq[5]
            out12[3] = seq[8]
            out12[4] = seq[1]
            out12[5] = seq[4]
            out12[6] = seq[6]
            out12[7] = seq[9]
            out12[8] = seq[2]
            out12[9] = 0.0
            out12[10] = seq[7]
            out12[11] = 0.0

            block = out12.reshape(6, 2) if layout == 'channels_last' else out12.reshape(2, 6)
            packed.append(block)

        return np.stack(packed, axis=0).copy(order='C')

    @staticmethod
    def pack_weight_mode_c(weight_cf: np.ndarray, layout: str) -> np.ndarray:
        """Mode C 權重打包"""
        C_out, C_in, K = weight_cf.shape
        assert (C_in, K) == (1, 7), "mode c 預期 C_in=1 且 K=7"

        packed = []
        for co in range(C_out):
            seq = weight_cf[co].reshape(-1)
            out12 = np.empty(12, dtype=seq.dtype)
            out12[0] = seq[0]
            out12[1] = seq[3]
            out12[2] = seq[6]
            out12[3] = 0.0
            out12[4] = seq[1]
            out12[5] = seq[4]
            out12[6] = 0.0
            out12[7] = 0.0
            out12[8] = seq[2]
            out12[9] = seq[5]
            out12[10:] = 0.0

            block = out12.reshape(12, 1) if layout == 'channels_last' else out12.reshape(1, 12)
            packed.append(block)

        return np.stack(packed, axis=0).copy(order='C')


class ConvGenerator(DataGenerator):
    """Conv1D 資料生成器"""

    def __init__(self, config: ConvConfig):
        self.config = config
        self.config.validate()

    def generate(self):
        """生成 Conv1D 測試資料"""
        cfg = self.MODES[self.config.mode]
        k, in_ch, stride = cfg['kernel_size'], cfg['in_ch'], cfg['stride']

        if self.config.in_width < k:
            raise ValueError(f"input width ({self.config.in_width}) 需 >= kernel size ({k})")

        np.random.seed(self.config.seed)

        # 內部統一使用 channels_first 計算
        act_in_cf = np.random.uniform(-1, 1, size=(in_ch, self.config.in_width)).astype(np.float32)
        weight_cf = np.random.uniform(-1, 1, size=(self.config.out_ch, in_ch, k)).astype(np.float32)
        conv_out_cf = self.conv1d_valid(act_in_cf, weight_cf, stride)

        ps_shape_cf = conv_out_cf.shape
        ps_in_cf = (np.zeros(ps_shape_cf, dtype=np.float32) if self.config.no_ps
                    else np.random.uniform(-0.5, 0.5, size=ps_shape_cf).astype(np.float32))
        act_out_cf = conv_out_cf + ps_in_cf

        # 依 layout 轉換欲儲存的 tensor
        if self.config.layout == 'channels_first':
            act_in_save = act_in_cf
            ps_in_save = ps_in_cf
            act_out_save = act_out_cf
            weight_save = self._pack_weight_channels_first(weight_cf)
        else:  # channels_last
            act_in_save = np.transpose(act_in_cf, (1, 0))
            ps_in_save = np.transpose(ps_in_cf, (1, 0))
            act_out_save = np.transpose(act_out_cf, (1, 0))
            weight_save = self._pack_weight_channels_last(weight_cf)

        # 儲存檔案
        self._save_files(act_in_save, weight_save, ps_in_save, act_out_save,
                        act_in_cf, weight_cf, conv_out_cf, k, in_ch, stride)

    def _pack_weight_channels_first(self, weight_cf: np.ndarray) -> np.ndarray:
        """Channels first layout 權重打包"""
        if self.config.mode == 'k5c2':
            return self.pack_weight_mode_b(weight_cf, 'channels_first')
        elif self.config.mode == 'k7c1':
            return self.pack_weight_mode_c(weight_cf, 'channels_first')
        return weight_cf

    def _pack_weight_channels_last(self, weight_cf: np.ndarray) -> np.ndarray:
        """Channels last layout 權重打包"""
        if self.config.mode == 'k5c2':
            return self.pack_weight_mode_b(weight_cf, 'channels_last')
        elif self.config.mode == 'k7c1':
            return self.pack_weight_mode_c(weight_cf, 'channels_last')
        return np.transpose(weight_cf, (0, 2, 1))

    def _save_files(self, act_in_save, weight_save, ps_in_save, act_out_save,
                   act_in_cf, weight_cf, conv_out_cf, k, in_ch, stride):
        """儲存所有檔案"""
        out_dir = Path(self.config.out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)

        self.save_array(act_in_save, out_dir / f"activation_input.{self.config.fmt}", self.config.fmt)
        self.save_array(weight_save, out_dir / f"weight.{self.config.fmt}", self.config.fmt)
        self.save_array(ps_in_save, out_dir / f"ps_input.{self.config.fmt}", self.config.fmt)
        self.save_array(act_out_save, out_dir / f"activation_output.{self.config.fmt}", self.config.fmt)

        meta = {
            "task": "conv1d",
            "mode": self.config.mode,
            "kernel_size": k,
            "in_ch": in_ch,
            "stride": stride,
            "out_ch": self.config.out_ch,
            "in_width": self.config.in_width,
            "out_width": conv_out_cf.shape[1],
            "format": self.config.fmt,
            "seed": self.config.seed,
            "partial_sum_zero": self.config.no_ps,
            "layout": self.config.layout,
            "activation_input_shape_saved": act_in_save.shape,
            "activation_input_shape_internal_cf": act_in_cf.shape,
            "weight_shape_saved": weight_save.shape,
            "weight_shape_internal_cf": weight_cf.shape,
            "ps_input_shape_saved": ps_in_save.shape,
            "activation_output_shape_saved": act_out_save.shape,
        }

        (out_dir / "meta.txt").write_text(
            "\n".join(f"{k}:{v}" for k, v in meta.items()),
            encoding='utf-8'
        )
        print(f"✓ Conv1D 資料生成完成: {out_dir}")


class GemmGenerator(DataGenerator):
    """GEMM 資料生成器"""

    def __init__(self, config: GemmConfig):
        self.config = config
        self.config.validate()

    def generate(self):
        """生成 GEMM 測試資料"""
        np.random.seed(self.config.seed)

        A_cf = np.random.uniform(-1, 1, size=(self.config.dim, self.config.in_width)).astype(np.float32)
        W_cf = np.random.uniform(-1, 1, size=(self.config.out_width, self.config.dim)).astype(np.float32)

        # GEMM: C = W @ A
        C_cf = W_cf @ A_cf
        ps_shape_cf = C_cf.shape
        PS_cf = (np.zeros(ps_shape_cf, dtype=np.float32) if self.config.no_ps
                 else np.random.uniform(-0.5, 0.5, size=ps_shape_cf).astype(np.float32))
        C_out_cf = C_cf + PS_cf

        W_save = np.transpose(W_cf, (1, 0))

        # 儲存檔案
        self._save_files(A_cf, W_save, PS_cf, C_out_cf, W_cf)

    def _save_files(self, A_cf, W_save, PS_cf, C_out_cf, W_cf):
        """儲存所有檔案"""
        out_dir = Path(self.config.out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)

        self.save_array(A_cf, out_dir / f"activation_input.{self.config.fmt}", self.config.fmt)
        self.save_array(W_save, out_dir / f"weight.{self.config.fmt}", self.config.fmt)
        self.save_array(PS_cf, out_dir / f"ps_input.{self.config.fmt}", self.config.fmt)
        self.save_array(C_out_cf, out_dir / f"activation_output.{self.config.fmt}", self.config.fmt)

        meta = {
            "task": "gemm",
            "dim": self.config.dim,
            "out_width": self.config.out_width,
            "in_width": self.config.in_width,
            "format": self.config.fmt,
            "seed": self.config.seed,
            "partial_sum_zero": self.config.no_ps,
            "A_shape_saved": A_cf.shape,
            "W_shape_internal_cf": W_cf.shape,
            "W_shape_saved": W_save.shape,
            "PS_shape_saved": PS_cf.shape,
            "C_shape_saved": C_out_cf.shape,
        }

        (out_dir / "meta.txt").write_text(
            "\n".join(f"{k}:{v}" for k, v in meta.items()),
            encoding='utf-8'
        )
        print(f"✓ GEMM 資料生成完成: {out_dir}")


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


def parse_args():
    """解析命令列參數"""
    p = argparse.ArgumentParser(
        description="HybridAcc PE 測試資料生成工具 (fp16)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
範例:
  # 使用命令列參數
  %(prog)s conv --mode k3c4 --out-ch 8 --in-width 128 --out-dir ./output/conv_k3c4
  %(prog)s gemm --out-width 64 --in-width 128 --dim 32 --out-dir ./output/gemm

  # 使用配置文件
  %(prog)s --config examples/conv_k3c4.json
  %(prog)s --config examples/gemm.yaml

  # 支援的 Conv1D 模式:
  k3c4: kernel=3, in_ch=4, stride=1
  k5c2: kernel=5, in_ch=2, stride=1
  k7c1: kernel=7, in_ch=1, stride=2
  k1c12: kernel=1, in_ch=12, stride=1
        """
    )

    # 配置文件選項
    p.add_argument('--config', type=Path, help='配置文件路徑 (JSON/YAML)')

    sub = p.add_subparsers(dest="task")

    # Conv 子命令
    pc = sub.add_parser("conv", help="產生 conv1D 測試資料")
    pc.add_argument('--mode', choices=['k3c4','k5c2','k7c1','k1c12'],
                   required=True, help='卷積模式')
    pc.add_argument('--out-ch', type=int, required=True, help='輸出 channel 數量 (1~16)')
    pc.add_argument('--in-width', type=int, required=True, help='輸入寬度 (3~800)')
    pc.add_argument('--fmt', choices=['bin','hex'], default='bin', help='輸出格式')
    pc.add_argument('--out-dir', type=str, required=True, help='輸出目錄')
    pc.add_argument('--seed', type=int, default=0, help='隨機種子')
    pc.add_argument('--no-ps', action='store_true', help='Partial sum 設為 0')
    pc.add_argument('--layout', choices=['channels_first','channels_last'],
                   default='channels_last', help='資料 layout')

    # GEMM 子命令
    pg = sub.add_parser("gemm", help="產生 GEMM 測試資料")
    pg.add_argument('--out-width', type=int, required=True, help='輸出寬度 (1~800)')
    pg.add_argument('--in-width', type=int, required=True, help='輸入寬度 (3~800)')
    pg.add_argument('--dim', type=int, required=True, help='中間維度')
    pg.add_argument('--fmt', choices=['bin','hex'], default='bin', help='輸出格式')
    pg.add_argument('--out-dir', type=str, required=True, help='輸出目錄')
    pg.add_argument('--seed', type=int, default=0, help='隨機種子')
    pg.add_argument('--no-ps', action='store_true', help='Partial sum 設為 0')

    return p.parse_args()


def main():
    """主程式入口"""
    args = parse_args()

    try:
        # 從配置文件載入
        if args.config:
            config_dict = ConfigLoader.load(args.config)
            task = config_dict.get('task')

            if task == 'conv' or task == 'conv1d':
                config = ConvConfig(**{k: v for k, v in config_dict.items() if k != 'task'})
                generator = ConvGenerator(config)
            elif task == 'gemm':
                config = GemmConfig(**{k: v for k, v in config_dict.items() if k != 'task'})
                generator = GemmGenerator(config)
            else:
                raise ValueError(f"未知的 task: {task}")

        # 從命令列參數載入
        elif args.task == 'gemm':
            config = GemmConfig(
                out_width=args.out_width,
                in_width=args.in_width,
                dim=args.dim,
                fmt=args.fmt,
                out_dir=args.out_dir,
                seed=args.seed,
                no_ps=args.no_ps
            )
            generator = GemmGenerator(config)

        elif args.task == 'conv':
            config = ConvConfig(
                mode=args.mode,
                out_ch=args.out_ch,
                in_width=args.in_width,
                fmt=args.fmt,
                out_dir=args.out_dir,
                seed=args.seed,
                no_ps=args.no_ps,
                layout=args.layout
            )
            generator = ConvGenerator(config)

        else:
            print("請指定 task (conv/gemm) 或提供配置文件 (--config)")
            return

        generator.generate()

    except Exception as e:
        print(f"❌ 錯誤: {e}")
        raise


if __name__ == "__main__":
    main()

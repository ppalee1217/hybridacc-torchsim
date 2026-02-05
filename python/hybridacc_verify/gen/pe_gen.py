import numpy as np
from pathlib import Path
from typing import Dict, Optional
from ..utils.config import ConvConfig, GemmConfig, ConvMode
from ..model.conv import golden_conv1d
from ..utils.io import save_array

class DataGenerator:
    """資料生成器基類"""

    STRIDE_BY_MODE = {
        'k7c1': 2,
    }

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
        self.conv_mode = ConvMode()
        self.conv_mode(self.config.mode)

    def generate(self):
        """生成 Conv1D 測試資料"""
        k = self.conv_mode.K
        in_ch = self.conv_mode.C
        stride = self.STRIDE_BY_MODE.get(self.config.mode, 1)

        if self.config.in_width < k:
            raise ValueError(f"input width ({self.config.in_width}) 需 >= kernel size ({k})")

        np.random.seed(self.config.seed)

        # 內部統一使用 channels_first 計算
        act_in_cf = np.random.uniform(-1, 1, size=(in_ch, self.config.in_width)).astype(np.float32)
        weight_cf = np.random.uniform(-1, 1, size=(self.config.out_ch, in_ch, k)).astype(np.float32)
        conv_out_cf = golden_conv1d(act_in_cf, weight_cf, stride)

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
        if self.conv_mode.K == 5 and self.conv_mode.C == 2:
            return self.pack_weight_mode_b(weight_cf, 'channels_first')
        elif self.conv_mode.K == 7 and self.conv_mode.C == 1:
            return self.pack_weight_mode_c(weight_cf, 'channels_first')
        return weight_cf

    def _pack_weight_channels_last(self, weight_cf: np.ndarray) -> np.ndarray:
        """Channels last layout 權重打包"""
        if self.conv_mode.K == 5 and self.conv_mode.C == 2:
            return self.pack_weight_mode_b(weight_cf, 'channels_last')
        elif self.conv_mode.K == 7 and self.conv_mode.C == 1:
            return self.pack_weight_mode_c(weight_cf, 'channels_last')
        return np.transpose(weight_cf, (0, 2, 1))

    def _save_files(self, act_in_save, weight_save, ps_in_save, act_out_save,
                   act_in_cf, weight_cf, conv_out_cf, k, in_ch, stride):
        """儲存所有檔案"""
        out_dir = Path(self.config.out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)

        save_array(act_in_save, out_dir / f"activation_input.{self.config.fmt}", self.config.fmt)
        save_array(weight_save, out_dir / f"weight.{self.config.fmt}", self.config.fmt)
        save_array(ps_in_save, out_dir / f"ps_input.{self.config.fmt}", self.config.fmt)
        save_array(act_out_save, out_dir / f"activation_output.{self.config.fmt}", self.config.fmt)

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

        save_array(A_cf, out_dir / f"activation_input.{self.config.fmt}", self.config.fmt)
        save_array(W_save, out_dir / f"weight.{self.config.fmt}", self.config.fmt)
        save_array(PS_cf, out_dir / f"ps_input.{self.config.fmt}", self.config.fmt)
        save_array(C_out_cf, out_dir / f"activation_output.{self.config.fmt}", self.config.fmt)

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

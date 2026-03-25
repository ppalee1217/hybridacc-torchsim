from __future__ import annotations

from .ir import ConvAttr, DataType, GemmAttr, OpIR, OpType, TensorShape


class Frontend:
    @staticmethod
    def create_conv2d(
        name: str,
        dtype: DataType,
        n: int,
        ic: int,
        ih: int,
        iw: int,
        oc: int,
        kh: int,
        kw: int,
        stride_h: int,
        stride_w: int,
        pad_h: int,
        pad_w: int,
        with_nlu: bool = False,
    ) -> OpIR:
        op = OpIR(name=name, op_type=OpType.CONV2D, dtype=dtype)
        op.input = TensorShape(n=n, c=ic, h=ih, w=iw)
        op.weight = TensorShape(n=oc, c=ic, h=kh, w=kw)
        oh = (ih + 2 * pad_h - kh) // stride_h + 1
        ow = (iw + 2 * pad_w - kw) // stride_w + 1
        op.output = TensorShape(n=n, c=oc, h=oh, w=ow)
        op.attr = ConvAttr(
            kernel_h=kh,
            kernel_w=kw,
            stride_h=stride_h,
            stride_w=stride_w,
            pad_h=pad_h,
            pad_w=pad_w,
        )
        op.allow_nlu = with_nlu
        if not Frontend.validate(op):
            raise ValueError(f"Conv2D validation failed for: {name}")
        return op

    @staticmethod
    def create_gemm(
        name: str,
        dtype: DataType,
        M: int,
        N: int,
        K: int,
        trans_a: bool = False,
        trans_b: bool = False,
        with_nlu: bool = False,
    ) -> OpIR:
        op = OpIR(name=name, op_type=OpType.GEMM, dtype=dtype)
        op.input = TensorShape(n=1, c=1, h=M, w=K)
        op.weight = TensorShape(n=1, c=1, h=K, w=N)
        op.output = TensorShape(n=1, c=1, h=M, w=N)
        op.attr = GemmAttr(M=M, N=N, K=K, trans_a=trans_a, trans_b=trans_b)
        op.allow_nlu = with_nlu
        if not Frontend.validate(op):
            raise ValueError(f"GEMM validation failed for: {name}")
        return op

    @staticmethod
    def validate(op: OpIR) -> bool:
        if op.input.numel() == 0 or op.weight.numel() == 0 or op.output.numel() == 0:
            return False
        if op.op_type == OpType.CONV2D:
            conv = op.conv()
            if conv.kernel_h == 0 or conv.kernel_w == 0 or conv.stride_h == 0 or conv.stride_w == 0:
                return False
            expected_oh = (op.input.h + 2 * conv.pad_h - conv.kernel_h) // conv.stride_h + 1
            expected_ow = (op.input.w + 2 * conv.pad_w - conv.kernel_w) // conv.stride_w + 1
            if expected_oh != op.output.h or expected_ow != op.output.w:
                return False
            if expected_oh <= 0 or expected_ow <= 0:
                return False
        else:
            gemm = op.gemm()
            if gemm.M == 0 or gemm.N == 0 or gemm.K == 0:
                return False
        return True
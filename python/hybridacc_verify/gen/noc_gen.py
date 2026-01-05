import torch
import numpy as np
from typing import Dict, Any, List
from ..utils.config import ScanChainConfig, PERouterMode, NocConvConfig, NocGemmConfig
from ..utils.data import TestData
from ..model.conv import golden_conv2d
from ..model.gemm import golden_gemm
from .pe_gen import DataGenerator

def generate_conv2d_test(config: NocConvConfig) -> List[TestData]:
    """
    Generate Conv2d test case based on config.
    """
    print("Generating Conv2d test data...")
    config.validate()

    # Shapes from config
    N = 1
    H, W, C = config.input_h, config.input_w, config.input_c
    OC, KH, KW = config.out_ch, config.kernel_h, config.kernel_w
    stride = config.stride
    padding = config.padding
    num_pes = config.num_pes
    num_bus = config.num_bus

    torch.manual_seed(config.seed)
    np.random.seed(config.seed)

    # Generate random data
    input_act = torch.randn(N, H, W, C).numpy()
    weight = torch.randn(OC, KH, KW, C).numpy()

    test_data_list = []

    # Determine split configuration
    splits = []
    if KH > num_bus:
        if KH == 5 and num_bus == 4:
             splits = [3, 2]
        else:
             # Generic split: chunks of num_bus
             remaining = KH
             while remaining > 0:
                 splits.append(min(remaining, num_bus))
                 remaining -= min(remaining, num_bus)
    else:
        splits = [KH]

    current_kh_start = 0
    previous_output = None

    # Calculate expected final output shape (based on full kernel)
    # We assume padding applies to the full operation.
    # For the split parts, we will manually slice the input to produce this exact output height.
    out_h_final = (H + 2*padding - KH) // stride + 1
    out_w_final = (W + 2*padding - KW) // stride + 1

    for idx, split_kh in enumerate(splits):
        # Slice weights
        weight_part = weight[:, current_kh_start:current_kh_start+split_kh, :, :]

        # Calculate required input height for this split to match out_h_final
        # H_in = (H_out - 1) * stride + K - 2*P_part
        # We assume P_part=0 for height as we slice the valid region.
        req_h_part = (out_h_final - 1) * stride + split_kh

        input_slice_start = current_kh_start
        input_slice_end = input_slice_start + req_h_part

        # Slice inputs
        # Note: This assumes the original input is large enough (i.e. padding=0 or handled)
        if input_slice_end > H:
             # Fallback or error handling if padding was expected to extend input
             # For the user's specific case (H=20, K=5, P=0), this works.
             print(f"Warning: Input slice [{input_slice_start}:{input_slice_end}] exceeds input height {H}")
             input_slice_end = H

        input_act_part = input_act[:, input_slice_start:input_slice_end, :, :]

        if previous_output is None:
             input_ps_part = torch.randn(N, out_h_final, out_w_final, OC).numpy() # NHWC
        else:
             input_ps_part = previous_output

        # Calculate Golden
        # We use padding=(0, padding) to disable height padding (since we sliced)
        # but keep width padding.
        output_part = golden_conv2d(input_act_part, weight_part, input_ps_part, stride=stride, padding=(0, padding))
        previous_output = output_part

        # Pack weights if needed
        weight_packed = weight_part
        if KW == 5 and C == 2:
             # Pack for k5c2
             # weight_part: (OC, split_kh, 5, 2)
             w_flat = weight_part.reshape(-1, 5, 2).transpose(0, 2, 1) # (N, 2, 5)
             packed = DataGenerator.pack_weight_mode_b(w_flat, 'channels_last') # (N, 6, 2)
             weight_packed = packed.reshape(OC, split_kh, 6, 2)
        elif KW == 7 and C == 1:
             # Pack for k7c1
             # weight_part: (OC, split_kh, 7, 1)
             w_flat = weight_part.reshape(-1, 7, 1).transpose(0, 2, 1) # (N, 1, 7)
             packed = DataGenerator.pack_weight_mode_c(w_flat, 'channels_last') # (N, 12, 1)
             weight_packed = packed.reshape(OC, split_kh, 12, 1)

        # Prepare numpy arrays
        inputs_part = {
            "activation": input_act_part,
            "weight": weight_packed,
            "partial_sum": input_ps_part
        }
        outputs_part = {
            "partial_sum": output_part
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
                if config.ultra_mode:
                    # Ultra Mode: Distribute workload across all buses
                    output_row_idx = j
                    enable = (output_row_idx < out_h_final)
                    route_mode = PERouterMode.PLI_FROM_BUS_PLO_TO_BUS
                    pd_id = output_row_idx * stride if enable else 63
                    ps_id = 0 if enable else 63
                    pli_id = output_row_idx if enable else 63
                    plo_id = output_row_idx if enable else 63
                else: # Normal Mode: Each bus handles a chunk of the kernel height
                    enable = (i < split_kh and j < out_h_final)
                    route_mode = PERouterMode.PLI_FROM_BUS_PLO_TO_BUS

                    if enable:
                        route_mode = get_route_mode(i, split_kh)

                    ps_id = i if enable else 63
                    pd_id = (i+j)*stride if enable else 63
                    pli_id = j if i==0 else 63
                    plo_id = j if i==split_kh-1 else 63

                cfg = ScanChainConfig(
                    ps_id=ps_id,
                    pd_id=pd_id,
                    pli_id=pli_id,
                    plo_id=plo_id,
                    route_mode=route_mode,
                    enable=enable
                )
                scan_chain.append(cfg)

        test_config = {
            "mode": "conv2d",
            "ultra_mode": config.ultra_mode,
            "kernel_size": split_kh,
            "in_ch": C,
            "stride": stride,
            "out_ch": OC,
            "in_height": input_act_part.shape[1],
            "in_width": input_act_part.shape[2],
            "out_height": output_part.shape[1],
            "out_width": output_part.shape[2],
            "partial_sum_zero": False,
            "seed": config.seed + idx
        }

        name_suffix = f"_part{idx+1}" if len(splits) > 1 else ""

        test_data_list.append(TestData(
            name=f"conv2d_custom{name_suffix}",
            description=f"Conv2d {split_kh}x{KW} (Split {idx+1}/{len(splits)})",
            inputs=inputs_part,
            outputs=outputs_part,
            scan_chain=scan_chain,
            config=test_config
        ))

        current_kh_start += split_kh

    return test_data_list

def generate_gemm_test(config: NocGemmConfig) -> TestData:
    """
    Generate GEMM test case based on config.
    C = A * B + D (Input PS)
    """
    print("Generating GEMM test data...")
    config.validate()

    M, N, K = config.M, config.N, config.K
    num_pes = config.num_pes

    torch.manual_seed(config.seed)
    np.random.seed(config.seed)

    # Generate random data
    A = torch.randn(M, K).numpy()
    B = torch.randn(K, N).numpy()
    D = torch.randn(M, N).numpy() # Input PS

    # Calculate GEMM
    C = golden_gemm(A, B, D)

    inputs = {
        "A": A,
        "B": B,
        "D": D
    }
    outputs = {
        "C": C
    }

    scan_chain = []
    for i in range(num_pes):
        cfg = ScanChainConfig(
            ps_id=i, pd_id=i, pli_id=i, plo_id=i,
            route_mode=PERouterMode.PLI_FROM_BUS_PLO_TO_BUS, enable=True
        )
        scan_chain.append(cfg)

    test_config = {
        "mode": "gemm",
        "M": M,
        "N": N,
        "K": K,
        "partial_sum_zero": False,
        "seed": config.seed
    }

    return TestData(
        name=f"gemm_{M}x{N}x{K}",
        description=f"GEMM M={M}, N={N}, K={K}",
        inputs=inputs,
        outputs=outputs,
        scan_chain=scan_chain,
        config=test_config
    )

# test conv2D behavior between torch and custom implementation
# This file is part of HybridAcc project

import torch
import torch.nn.functional as F
import numpy as np


def test_conv2d():
    # Define input tensor and weights
    input_tensor = torch.randn(1, 3, 32, 32)  # Batch size 1, 3 channels, 32x32 image
    weights = torch.randn(16, 3, 3, 3)  # 16 filters, 3 channels, 3x3 kernel

    # Perform convolution using PyTorch
    output_torch = F.conv2d(input_tensor, weights)

    # Convert to numpy for custom implementation
    input_np = input_tensor.numpy()
    weights_np = weights.numpy()

    # Custom convolution implementation (placeholder)
    def custom_conv2d(input_np, weights_np):
        # This should contain the actual custom convolution logic
        return np.random.randn(1, 16, 30, 30)  # Placeholder output shape

    output_custom = custom_conv2d(input_np, weights_np)

    # Compare outputs
    assert np.allclose(output_torch.numpy(), output_custom), "Outputs do not match!"
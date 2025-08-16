
# HybridAcc/python/simulator/PEarray.py

import torch
from torch.nn import functional as F
from ProssesElement import ProcessElement, ProcessElementParams


class PEarray:
    def __init__(self, params):
        self.params = params
        self.pe_sim = ProcessElement(params)

    def processConv2D(self, input_data, weights, psum, channels, kernel_size, stride=1):
        # 2D convolution logic would go here
        in_channels = input_data.shape[1]
        out_channels = weights.shape[0]
        kernel_size = weights.shape[2]

        # Process each tile of the input and weights
        for i in range(0, in_channels, self.params.in_channels_per_tile):
            for j in range(0, out_channels, self.params.out_channels_per_tile):
                print(f"Processing tile: input channels {i}-{i+self.params.in_channels_per_tile}, "
                      f"output channels {j}-{j+self.params.out_channels_per_tile}")

                for out_h in range(0, input_data.shape[2], stride):
                    psum_tile = psum[:, j:j+self.params.out_channels_per_tile, out_h, :]

                    # Process each kernel row (2D convolution made of 1D convolutions)
                    for k in range(0, kernel_size):
                        # calculate input high
                        input_h = out_h * stride + k - kernel_size // 2
                        # print(f"Processing kernel row {k}, input height {input_h}, output height {out_h}")
                        if input_h < 0 or input_h >= input_data.shape[2]: # padding check
                            continue
                        # Get the current tile of input and weights
                        input_tile = input_data[:, i:i+self.params.in_channels_per_tile, input_h, :]
                        weight_tile = weights[j:j+self.params.out_channels_per_tile, i:i+self.params.in_channels_per_tile, k, :]

                        # Process the tile (1D convolution for each kernel row)
                        psum_tile = self.pe_sim.processConv(input_tile, weight_tile, psum_tile, channels=self.params.in_channels_per_tile, kernel_size=kernel_size)

                    # Store the result back in psum
                    psum[:, j:j+self.params.out_channels_per_tile, out_h, :] = psum_tile


def testConv2D(params: ProcessElementParams):
    # random seed
    torch.manual_seed(0)

    # Example test for the PEarray
    inputs_tensor = torch.randn(1, 8, 32, 32)  # Batch size 1, 8 channels, 32x32 image
    weights = torch.randn(64, 8, 3, 3)  # 64 filters, 8 channels, 3x3 kernel
    psum = torch.zeros(1, 64, 32, 32)  # Output size after convolution
    pe_array = PEarray(params)
    pe_array.processConv2D(inputs_tensor, weights, psum, channels=8, kernel_size=3)

    # Compare outputs with F.conv2d
    expected_output = F.conv2d(inputs_tensor, weights, stride=1, padding=1)
    # Compute cosine similarity between psum and expected_output
    psum_flat = psum.view(-1)
    expected_flat = expected_output.view(-1)
    cosine_similarity = F.cosine_similarity(psum_flat.unsqueeze(0), expected_flat.unsqueeze(0))

    if cosine_similarity.item() < 0.999:  # Threshold for similarity
        print(f"Test failed: Cosine similarity is too low ({cosine_similarity.item():.6f})!")
        return

    print("Test passed successfully!")

if __name__ == "__main__":
    # Example usage
    params = ProcessElementParams(
        element_size=16, # 16 bits per element
        input_buffer_size=12,
        weight_buffer_size=7*32,
        psum_buffer_size=24,
        input_bandwidth=1,
        weight_bandwidth=4,
        psum_bandwidth=4,
        in_channels_per_tile=4,
        out_channels_per_tile=24
    )

    test(params)

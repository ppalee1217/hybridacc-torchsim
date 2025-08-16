import simulator
from test.convolution import test_conv2d
import argparse

def parse_args():
    parser = argparse.ArgumentParser(description="Run HybridAcc tests")
    parser.add_argument('--test', type=str, default='convolution', help='Test to run')
    return parser.parse_args()

def main():
    args = parse_args()
    if args.test == 'convolution':
        test_conv2d()

if __name__ == "__main__":
    main()
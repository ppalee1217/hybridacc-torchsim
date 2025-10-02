from analytical_base import *
import argparse

def parse_args():
    parser = argparse.ArgumentParser(description="Run analytical tests")
    parser.add_argument('--test', type=str, default='convolution', help='Test to run')
    return parser.parse_args()


def main():
    args = parse_args()




if __name__ == "__main__":
    main()
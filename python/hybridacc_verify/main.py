import argparse
import sys
from .gen.noc_gen import generate_conv2d_test as gen_noc_conv, generate_gemm_test as gen_noc_gemm
from .gen.pe_gen import ConvGenerator, GemmGenerator
from .utils.config import ConvConfig, GemmConfig, ConfigLoader
from .check.comparator import main as check_main

def main():
    parser = argparse.ArgumentParser(description="HybridAcc Verification Tool")
    subparsers = parser.add_subparsers(dest="command", help="Command to run")

    # Gen PE
    pe_parser = subparsers.add_parser("gen-pe", help="Generate PE test data")
    pe_parser.add_argument('--config', type=str, help='Config file path')
    # Add other PE args if needed, or rely on config file for now to keep it simple
    # Or replicate the args from pe_sim_gen.py

    # Gen NoC
    noc_parser = subparsers.add_parser("gen-noc", help="Generate NoC test data")
    noc_parser.add_argument("--output-dir", type=str, default="output/noc_test_data", help="Directory to save test data")
    noc_parser.add_argument("--num-pes", type=int, default=64, help="Total number of PEs")
    noc_parser.add_argument("--num-bus", type=int, default=4, help="Total number of buses")
    noc_parser.add_argument("--seed", type=int, default=123, help="Random seed")

    # Check
    check_parser = subparsers.add_parser("check", help="Compare simulation results")
    check_parser.add_argument('--sim', required=True, help='Simulation output (.bin)')
    check_parser.add_argument('--expected', required=True, help='Expected/Golden output (.bin)')
    check_parser.add_argument('--rtol', type=float, default=1e-2, help='Relative tolerance')
    check_parser.add_argument('--atol', type=float, default=1e-3, help='Absolute tolerance')
    check_parser.add_argument('--show', type=int, default=10, help='Show top N mismatches')
    check_parser.add_argument('--quiet', action='store_true', help='Quiet mode')
    check_parser.add_argument('--dump-csv', dest='dump_csv', help='Dump CSV path')

    args = parser.parse_args()

    if args.command == "gen-pe":
        # This is a simplified version, ideally we should support full CLI args like pe_sim_gen.py
        if args.config:
            from pathlib import Path
            config_dict = ConfigLoader.load(Path(args.config))
            task = config_dict.get('task')
            if task == 'conv' or task == 'conv1d':
                config = ConvConfig(**{k: v for k, v in config_dict.items() if k != 'task'})
                generator = ConvGenerator(config)
            elif task == 'gemm':
                config = GemmConfig(**{k: v for k, v in config_dict.items() if k != 'task'})
                generator = GemmGenerator(config)
            else:
                print(f"Unknown task: {task}")
                return
            generator.generate()
        else:
            print("Please provide --config for gen-pe (CLI args support pending)")

    elif args.command == "gen-noc":
        from pathlib import Path
        output_path = Path(args.output_dir)

        # Generate Conv2d
        conv_test = gen_noc_conv(args.num_pes, args.num_bus)
        conv_test.save(output_path / "conv2d")

        # Generate GEMM
        gemm_test = gen_noc_gemm(args.num_pes)
        gemm_test.save(output_path / "gemm")

    elif args.command == "check":
        # Reconstruct argv for check_main
        check_argv = ['--sim', args.sim, '--expected', args.expected,
                      '--rtol', str(args.rtol), '--atol', str(args.atol),
                      '--show', str(args.show)]
        if args.quiet:
            check_argv.append('--quiet')
        if args.dump_csv:
            check_argv.extend(['--dump-csv', args.dump_csv])

        sys.exit(check_main(check_argv))

    else:
        parser.print_help()

if __name__ == "__main__":
    main()

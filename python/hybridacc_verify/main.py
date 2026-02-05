import argparse
import sys
from .gen.noc_gen import generate_conv2d_test as gen_noc_conv, generate_gemm_test as gen_noc_gemm
from .gen.pe_gen import ConvGenerator, GemmGenerator
from .utils.config import ConvConfig, GemmConfig, ConfigLoader, NocConvConfig, NocGemmConfig, SUPPORTED_CONV_MODES
from .check.comparator import run_check_with_args

def main():
    parser = argparse.ArgumentParser(description="HybridAcc Verification Tool")
    subparsers = parser.add_subparsers(dest="command", help="Command to run")

    # Gen PE
    pe_parser = subparsers.add_parser("gen-pe", help="Generate PE test data")
    pe_parser.add_argument('--config', type=str, help='Config file path')

    sub = pe_parser.add_subparsers(dest="task")

    # Conv 子命令
    pc = sub.add_parser("conv", help="generate Conv2d test data")
    pc.add_argument('--mode', choices=SUPPORTED_CONV_MODES,
                   required=True, help='mode')
    pc.add_argument('--out-ch', type=int, required=True, help='output channel count (1~16)')
    pc.add_argument('--in-width', type=int, required=True, help='input width (3~800)')
    pc.add_argument('--fmt', choices=['bin','hex'], default='bin', help='output format')
    pc.add_argument('--out-dir', type=str, required=True, help='output directory')
    pc.add_argument('--seed', type=int, default=0, help='random seed')
    pc.add_argument('--no-ps', action='store_true', help='Partial sum set to 0')
    pc.add_argument('--layout', choices=['channels_first','channels_last'],
                   default='channels_last', help='data layout')

    # GEMM 子命令
    pg = sub.add_parser("gemm", help="generate GEMM test data")
    pg.add_argument('--out-width', type=int, required=True, help='output width (1~800)')
    pg.add_argument('--in-width', type=int, required=True, help='input width (3~800)')
    pg.add_argument('--dim', type=int, required=True, help='intermediate dimension')
    pg.add_argument('--fmt', choices=['bin','hex'], default='bin', help='output format')
    pg.add_argument('--out-dir', type=str, required=True, help='output directory')
    pg.add_argument('--seed', type=int, default=0, help='random seed')
    pg.add_argument('--no-ps', action='store_true', help='Partial sum set to 0')


    # Gen NoC
    noc_parser = subparsers.add_parser("gen-noc", help="Generate NoC test data")
    noc_parser.add_argument("--config", type=str, help="Config file path")

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
        output_path = Path("output")

        if args.config:
            config_dict = ConfigLoader.load(Path(args.config))
            mode = config_dict.get('mode', 'conv2d')

            if mode == 'conv2d':
                config = NocConvConfig(**config_dict)
                conv_tests = gen_noc_conv(config)
                for test in conv_tests:
                    save_dir = Path(config.out_dir)
                    if len(conv_tests) > 1:
                        save_dir = save_dir / test.name
                    test.save(save_dir)
            elif mode == 'gemm':
                config = NocGemmConfig(**config_dict)
                gemm_test = gen_noc_gemm(config)
                gemm_test.save(config.out_dir)
            else:
                print(f"Unknown mode for NoC: {mode}")
        else:
            # Default behavior with hardcoded values for backward compatibility
            default_conv_config = NocConvConfig(
                num_pes=args.num_pes,
                num_bus=args.num_bus,
                stride=1,
                input_h=18,
                input_w=200,
                input_c=4,
                out_ch=16,
                kernel_h=3,
                kernel_w=3,
                seed=args.seed,
                padding=0
            )

            # Generate Conv2d
            conv_tests = gen_noc_conv(default_conv_config)
            for test in conv_tests:
                save_dir = output_path / "conv2d_default"
                if len(conv_tests) > 1:
                    save_dir = save_dir / test.name
                test.save(save_dir)

            default_gemm_config = NocGemmConfig(
                num_pes=args.num_pes,
                M=32,
                N=32,
                K=32,
                seed=args.seed
            )

            # Generate GEMM
            gemm_test = gen_noc_gemm(default_gemm_config)
            gemm_test.save(output_path / "gemm_default")

    elif args.command == "check":
        sys.exit(run_check_with_args(args))

    else:
        parser.print_help()

if __name__ == "__main__":
    main()

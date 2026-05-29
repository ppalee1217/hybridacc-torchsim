#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="${SCRIPT_DIR}/install"

usage() {
    cat <<EOF
Usage:
  scripts/setup.sh all [--riscv-prefix PATH] [--no-source]
  scripts/setup.sh install <systemc|riscv|pe|python|all> [tool options]
  scripts/setup.sh env [--riscv-prefix PATH] [--no-source]
    scripts/setup.sh env-check [--strict-eda] [--no-eda]
    scripts/setup.sh fast <cluster-sim|noc-sim|pe-sim|cluster-test|run-test|hybridacc-sim|core-sim> [args...]

Examples:
  scripts/setup.sh all
  scripts/setup.sh install riscv --prefix \$HOME/.local/riscv
  scripts/setup.sh env --riscv-prefix \$HOME/.local/riscv
    scripts/setup.sh env-check
  scripts/setup.sh fast cluster-sim run conv_k3c4
    scripts/setup.sh fast hybridacc-sim build
    scripts/setup.sh fast core-sim run test_dma
EOF
}

run_install() {
    local target="$1"
    shift || true

    case "${target}" in
        systemc)
            "${INSTALL_DIR}/install_systemc.sh" "$@"
            ;;
        riscv)
            "${INSTALL_DIR}/install_riscv_toolchain.sh" "$@"
            ;;
        pe)
            "${INSTALL_DIR}/install_hybridacc_pe_toolchain.sh" "$@"
            ;;
        python)
            "${INSTALL_DIR}/install_python_project.sh" "$@"
            ;;
        all)
            "${INSTALL_DIR}/install_systemc.sh"
            "${INSTALL_DIR}/install_riscv_toolchain.sh"
            "${INSTALL_DIR}/install_hybridacc_pe_toolchain.sh"
            "${INSTALL_DIR}/install_python_project.sh"
            ;;
        *)
            echo "Unknown install target: ${target}"
            usage
            exit 1
            ;;
    esac
}

run_fast_entry() {
    local target="$1"
    shift || true
    case "${target}" in
        cluster-sim)
            exec "${SCRIPT_DIR}/fast_entry/cluster_sim.sh" "$@"
            ;;
        noc-sim)
            exec "${SCRIPT_DIR}/fast_entry/noc_sim.sh" "$@"
            ;;
        pe-sim)
            exec "${SCRIPT_DIR}/fast_entry/pe_sim.sh" "$@"
            ;;
        cluster-test)
            exec "${SCRIPT_DIR}/fast_entry/cluster_test.sh" "$@"
            ;;
        run-test)
            exec "${SCRIPT_DIR}/fast_entry/run_test.sh" "$@"
            ;;
        hybridacc-sim)
            exec "${SCRIPT_DIR}/fast_entry/hybridacc_sim.sh" "$@"
            ;;
        core-sim)
            exec "${SCRIPT_DIR}/fast_entry/core_sim.sh" "$@"
            ;;
        *)
            echo "Unknown fast entry target: ${target}"
            usage
            exit 1
            ;;
    esac
}

if [[ $# -eq 0 ]]; then
    usage
    exit 1
fi

CMD="$1"
shift

case "${CMD}" in
    all)
        RISCV_PREFIX="${HOME}/.local/riscv"
        NO_SOURCE=0
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --riscv-prefix)
                    RISCV_PREFIX="$2"
                    shift 2
                    ;;
                --no-source)
                    NO_SOURCE=1
                    shift
                    ;;
                *)
                    echo "Unknown option for all: $1"
                    usage
                    exit 1
                    ;;
            esac
        done

        run_install all
        if [[ "${NO_SOURCE}" == "1" ]]; then
            "${INSTALL_DIR}/configure_shell_env.sh" --riscv-prefix "${RISCV_PREFIX}" --no-source
        else
            "${INSTALL_DIR}/configure_shell_env.sh" --riscv-prefix "${RISCV_PREFIX}"
        fi
        ;;

    install)
        if [[ $# -lt 1 ]]; then
            echo "Missing install target"
            usage
            exit 1
        fi
        TARGET="$1"
        shift
        run_install "${TARGET}" "$@"
        ;;

    env)
        "${INSTALL_DIR}/configure_shell_env.sh" "$@"
        ;;

    env-check|check)
        "${SCRIPT_DIR}/env_check.sh" "$@"
        ;;

    fast)
        if [[ $# -lt 1 ]]; then
            echo "Missing fast entry target"
            usage
            exit 1
        fi
        TARGET="$1"
        shift
        run_fast_entry "${TARGET}" "$@"
        ;;

    -h|--help|help)
        usage
        ;;

    *)
        echo "Unknown command: ${CMD}"
        usage
        exit 1
        ;;
esac

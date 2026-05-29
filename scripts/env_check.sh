#!/usr/bin/env bash
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
STRICT_EDA=0
CHECK_EDA=1
FAILS=0
WARNS=0

usage() {
    cat <<EOF
Usage: scripts/env_check.sh [--strict-eda] [--no-eda]

Checks the HybridAcc host environment, Python package entrypoints, SystemC,
RISC-V firmware toolchain, and optional EDA tools loaded through tcsh.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --strict-eda)
            STRICT_EDA=1
            shift
            ;;
        --no-eda)
            CHECK_EDA=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -t 1 ]]; then
    C_OK=$'\033[32m'
    C_WARN=$'\033[33m'
    C_FAIL=$'\033[31m'
    C_DIM=$'\033[2m'
    C_RESET=$'\033[0m'
else
    C_OK=""
    C_WARN=""
    C_FAIL=""
    C_DIM=""
    C_RESET=""
fi

ok() {
    printf '%s[OK]%s   %s\n' "${C_OK}" "${C_RESET}" "$1"
}

warn() {
    printf '%s[WARN]%s %s\n' "${C_WARN}" "${C_RESET}" "$1"
    WARNS=$((WARNS + 1))
}

fail() {
    printf '%s[FAIL]%s %s\n' "${C_FAIL}" "${C_RESET}" "$1"
    FAILS=$((FAILS + 1))
}

section() {
    printf '\n%s== %s ==%s\n' "${C_DIM}" "$1" "${C_RESET}"
}

check_cmd() {
    local cmd="$1"
    local required="${2:-required}"
    local path
    if path="$(command -v "${cmd}" 2>/dev/null)"; then
        ok "${cmd}: ${path}"
    elif [[ "${required}" == "required" ]]; then
        fail "${cmd}: not found in PATH"
    else
        warn "${cmd}: not found in PATH"
    fi
}

check_path() {
    local label="$1"
    local path="$2"
    local required="${3:-required}"
    if [[ -e "${path}" ]]; then
        ok "${label}: ${path}"
    elif [[ "${required}" == "required" ]]; then
        fail "${label}: missing ${path}"
    else
        warn "${label}: missing ${path}"
    fi
}

check_env_path() {
    local name="$1"
    local required="${2:-optional}"
    local value="${!name:-}"
    if [[ -z "${value}" ]]; then
        if [[ "${required}" == "required" ]]; then
            fail "${name}: not set"
        else
            warn "${name}: not set"
        fi
    elif [[ -e "${value}" ]]; then
        ok "${name}: ${value}"
    else
        if [[ "${required}" == "required" ]]; then
            fail "${name}: set to missing path ${value}"
        else
            warn "${name}: set to missing path ${value}"
        fi
    fi
}

section "Repository"
check_path "repo root" "${REPO_ROOT}" required
check_path "pyproject.toml" "${REPO_ROOT}/pyproject.toml" required
check_path "RTL Makefile" "${REPO_ROOT}/design/hybridacc-RTL/Makefile" required
check_path "SystemC vendored tree" "${REPO_ROOT}/libs/systemc-2.3.3" required

section "Host tools"
for cmd in bash make cmake g++ git uv; do
    check_cmd "${cmd}" required
done
check_cmd tcsh optional

section "Python package"
if command -v uv >/dev/null 2>&1; then
    if (cd "${REPO_ROOT}" && uv run python -c "import hybridacc_cc, hybridacc_verify, hybridacc_tools, log_parser, trace_parser, syn_report_parser" >/dev/null 2>&1); then
        ok "uv package imports: hybridacc_cc, hybridacc_verify, hybridacc_tools, log_parser, trace_parser, syn_report_parser"
    else
        fail "uv package imports failed"
    fi

    for cli in hacc-compile hacc-setup hacc-sweep hacc-e2e-monitor hacc-flat-fw-mem hacc-wave-gap-summary syn-report; do
        if (cd "${REPO_ROOT}" && uv run "${cli}" --help >/dev/null 2>&1); then
            ok "uv run ${cli} --help"
        else
            fail "uv run ${cli} --help failed"
        fi
    done
else
    fail "uv is required before Python package checks can run"
fi

section "SystemC"
check_env_path SYSTEMC_LIB optional
check_env_path SYSTEMC_HOME optional
if [[ -f "${REPO_ROOT}/libs/systemc-2.3.3/include/systemc" || -f "${REPO_ROOT}/libs/systemc-2.3.3/include/systemc.h" ]]; then
    ok "SystemC headers: ${REPO_ROOT}/libs/systemc-2.3.3/include"
else
    fail "SystemC headers not found under libs/systemc-2.3.3/include"
fi

section "Firmware toolchain"
for cmd in riscv32-unknown-elf-gcc riscv32-unknown-elf-objcopy riscv32-unknown-elf-objdump riscv32-unknown-elf-size; do
    check_cmd "${cmd}" required
done
check_cmd ha-asm optional
check_cmd ha-package optional

section "EDA tools"
if [[ "${CHECK_EDA}" == "0" ]]; then
    warn "EDA checks skipped by --no-eda"
elif ! command -v tcsh >/dev/null 2>&1; then
    if [[ "${STRICT_EDA}" == "1" ]]; then
        fail "tcsh is required for EDA checks"
    else
        warn "tcsh not found; skipped EDA checks"
    fi
else
    for cmd in vcs dc_shell pt_shell fsdb2vcd jg; do
        tool_output="$(tcsh -ic "source ~/.tcshrc; which ${cmd}" 2>&1 || true)"
        tool_path="$(printf '%s\n' "${tool_output}" | grep -E "/${cmd}$" | tail -n 1 || true)"
        if [[ -n "${tool_path}" ]]; then
            ok "tcsh -ic ${cmd}: ${tool_path}"
        elif [[ "${STRICT_EDA}" == "1" ]]; then
            fail "tcsh -ic ${cmd}: not found after sourcing ~/.tcshrc"
        else
            warn "tcsh -ic ${cmd}: not found after sourcing ~/.tcshrc"
        fi
    done
fi

section "Summary"
if [[ "${FAILS}" -eq 0 ]]; then
    ok "environment check completed with ${WARNS} warning(s)"
    exit 0
fi

fail "environment check completed with ${FAILS} failure(s) and ${WARNS} warning(s)"
exit 1

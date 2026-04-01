#!/usr/bin/env python3
"""Parse VCS simulation logs and generate a Markdown summary report.

Reads run log files from the sim/log/ directory and outputs a consolidated
Markdown table with pass/fail counts per testbench.

Usage:
    python3 sim_report.py [--log-dir LOG_DIR] [--output OUTPUT_FILE]
                          [--filter PATTERN] [--mode MODE]

Examples:
    python3 sim_report.py --all
    python3 sim_report.py --log-dir ./sim/log --output report/pre_sim_report.md
    python3 sim_report.py --filter 'tb_vaddu|tb_vmulu'
    python3 sim_report.py --mode post-sim --output report/post_sim_report.md
"""
import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from datetime import datetime


@dataclass
class SimResult:
    testbench: str = ""
    compile_ok: bool = False
    run_ok: bool = False
    pass_count: int = 0
    fail_count: int = 0
    total_assertions: int = 0
    verdict: str = "UNKNOWN"  # PASS, FAIL, TIMEOUT, COMPILE_ERROR, NOT_RUN
    pass_tests: list = field(default_factory=list)
    fail_tests: list = field(default_factory=list)
    errors: list = field(default_factory=list)
    # X/timing vs logic failure breakdown
    x_fail_count: int = 0
    logic_fail_count: int = 0
    x_fail_tests: list = field(default_factory=list)
    logic_fail_tests: list = field(default_factory=list)
    # For system-level TBs (tb_pe_sim)
    mismatches: int = -1
    cosine_similarity: float = -1.0


def parse_compile_log(filepath: str, result: SimResult):
    """Parse VCS compilation log to check for errors."""
    if not os.path.isfile(filepath):
        result.errors.append(f"Missing compile log: {os.path.basename(filepath)}")
        return

    text = Path(filepath).read_text(errors="replace")

    # Check for compile errors (excluding lines that just print "ERROR" as part of puts)
    error_lines = []
    for line in text.splitlines():
        # Skip lines that are TCL puts or quoted strings
        if re.match(r'\s*puts\s+"', line):
            continue
        if re.search(r'Error[:\s]', line, re.IGNORECASE) and not re.match(r'\s*"', line):
            error_lines.append(line.strip())

    if error_lines:
        # Check if errors are real compilation errors vs just informational
        real_errors = [e for e in error_lines if not e.startswith('puts ')]
        if real_errors:
            result.errors.extend(real_errors[:5])
            return

    result.compile_ok = True


def parse_run_log(filepath: str, result: SimResult):
    """Parse VCS run log to extract test results."""
    if not os.path.isfile(filepath):
        result.errors.append(f"Missing run log: {os.path.basename(filepath)}")
        return

    text = Path(filepath).read_text(errors="replace")
    lines = text.splitlines()

    result.run_ok = True

    # Count [PASS], [FAIL], [FAIL-X], [FAIL-LOGIC] lines
    for line in lines:
        m_pass = re.match(r'\[PASS\]\s+(.*)', line)
        m_fail_x = re.match(r'\[FAIL-X\]\s+(.*)', line)
        m_fail_logic = re.match(r'\[FAIL-LOGIC\]\s+(.*)', line)
        m_fail = re.match(r'\[FAIL\]\s+(.*)', line)
        if m_pass:
            result.pass_count += 1
            result.pass_tests.append(m_pass.group(1).strip())
        elif m_fail_x:
            result.fail_count += 1
            result.x_fail_count += 1
            result.x_fail_tests.append(m_fail_x.group(1).strip())
        elif m_fail_logic:
            result.fail_count += 1
            result.logic_fail_count += 1
            result.logic_fail_tests.append(m_fail_logic.group(1).strip())
        elif m_fail:
            result.fail_count += 1
            result.fail_tests.append(m_fail.group(1).strip())

    result.total_assertions = result.pass_count + result.fail_count

    # Check for timeout
    if any(re.search(r'\[TIMEOUT\]', line) for line in lines):
        result.verdict = "TIMEOUT"
        return

    # Check for summary line (enhanced or legacy format)
    for line in lines:
        # Enhanced: === tb_xxx Summary: N PASSED, M FAILED (X X/timing, Y logic) ===
        m = re.search(r'===\s+(\S+)\s+Summary:\s+(\d+)\s+PASSED,\s+(\d+)\s+FAILED\s+\((\d+)\s+X/timing,\s+(\d+)\s+logic\)\s+===', line)
        if m:
            result.pass_count = int(m.group(2))
            result.fail_count = int(m.group(3))
            result.x_fail_count = int(m.group(4))
            result.logic_fail_count = int(m.group(5))
            result.total_assertions = result.pass_count + result.fail_count
            continue
        # Legacy: === tb_xxx Summary: N PASSED, M FAILED ===
        m = re.search(r'===\s+(\S+)\s+Summary:\s+(\d+)\s+PASSED,\s+(\d+)\s+FAILED\s+===', line)
        if m:
            result.pass_count = int(m.group(2))
            result.fail_count = int(m.group(3))
            result.total_assertions = result.pass_count + result.fail_count

    # Check for system-level TB metrics (tb_pe_sim style)
    for line in lines:
        m = re.search(r'Mismatches:\s+(\d+)', line)
        if m:
            result.mismatches = int(m.group(1))
        m = re.search(r'Cosine Similarity:\s+([\d.]+)', line)
        if m:
            result.cosine_similarity = float(m.group(1))

    # Determine verdict from final line patterns
    verdict_found = False
    for line in reversed(lines):
        line_stripped = line.strip()
        # Pattern: "tb_xxx PASS" or "PASS: tb_xxx"
        if re.search(r'\bPASS\b', line_stripped) and re.search(r'tb_\w+', line_stripped):
            result.verdict = "PASS"
            verdict_found = True
            break
        elif re.search(r'\bFAIL\b', line_stripped) and re.search(r'tb_\w+', line_stripped):
            result.verdict = "FAIL"
            verdict_found = True
            break

    if not verdict_found:
        if result.fail_count > 0:
            result.verdict = "FAIL"
        elif result.pass_count > 0:
            result.verdict = "PASS"
        else:
            result.verdict = "UNKNOWN"


def parse_testbench(log_dir: str, tb_name: str) -> SimResult:
    """Parse all logs for a given testbench."""
    result = SimResult(testbench=tb_name)

    compile_log = os.path.join(log_dir, f"{tb_name}.compile.log")
    run_log = os.path.join(log_dir, f"{tb_name}.run.log")

    parse_compile_log(compile_log, result)
    if result.compile_ok:
        parse_run_log(run_log, result)
    else:
        result.verdict = "COMPILE_ERROR"

    return result


def generate_markdown(results: list, mode: str = "pre-sim", output_path: str = None) -> str:
    """Generate consolidated Markdown report."""
    mode_label = "Pre-Simulation" if mode == "pre-sim" else "Post-Simulation (Gate-Level)"
    lines = []
    lines.append(f"# {mode_label} Report")
    lines.append("")
    lines.append(f"**Generated**: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f"**Tool**: Synopsys VCS W-2024.09-SP1")
    lines.append(f"**Mode**: {mode_label}")
    lines.append("")

    # === Summary Table ===
    lines.append("## Summary")
    lines.append("")
    lines.append(
        "| # | Testbench | Compile | Run | PASS | FAIL-X | FAIL-Logic | Verdict |"
    )
    lines.append(
        "|---|-----------|---------|-----|------|--------|------------|---------|"
    )

    total_pass = 0
    total_fail = 0
    total_tb_pass = 0
    total_tb_fail = 0

    for i, r in enumerate(results, 1):
        compile_icon = "✅" if r.compile_ok else "❌"
        run_icon = "✅" if r.run_ok else "❌"
        verdict_icon = {
            "PASS": "✅ **PASS**",
            "FAIL": "❌ **FAIL**",
            "TIMEOUT": "⏱️ **TIMEOUT**",
            "COMPILE_ERROR": "❌ **COMPILE_ERROR**",
            "UNKNOWN": "❓ **UNKNOWN**",
            "NOT_RUN": "⏸️ **NOT_RUN**",
        }.get(r.verdict, r.verdict)

        # Fail breakdown
        pass_str = str(r.pass_count)
        fail_x_str = str(r.x_fail_count)
        fail_logic_str = str(r.logic_fail_count)
        unclassified = r.fail_count - r.x_fail_count - r.logic_fail_count
        if unclassified > 0:
            fail_logic_str += f" (+{unclassified})"
        if r.mismatches >= 0:
            verdict_icon += f" (Mismatches={r.mismatches}"
            if r.cosine_similarity >= 0:
                verdict_icon += f", Cosine={r.cosine_similarity:.6f}"
            verdict_icon += ")"

        lines.append(
            f"| {i} | {r.testbench} | {compile_icon} | {run_icon} | "
            f"{pass_str} | {fail_x_str} | {fail_logic_str} | {verdict_icon} |"
        )

        total_pass += r.pass_count
        total_fail += r.fail_count
        if r.verdict == "PASS":
            total_tb_pass += 1
        else:
            total_tb_fail += 1

    lines.append("")
    lines.append("## Statistics")
    lines.append("")
    lines.append(f"- **Testbenches Passed**: {total_tb_pass} / {len(results)}")
    lines.append(f"- **Testbenches Failed**: {total_tb_fail} / {len(results)}")
    total_x_fail = sum(r.x_fail_count for r in results)
    total_logic_fail = sum(r.logic_fail_count for r in results)
    total_unclassified = total_fail - total_x_fail - total_logic_fail
    fail_parts = []
    if total_x_fail > 0:
        fail_parts.append(f"{total_x_fail} FAIL-X")
    if total_logic_fail > 0:
        fail_parts.append(f"{total_logic_fail} FAIL-Logic")
    if total_unclassified > 0:
        fail_parts.append(f"{total_unclassified} FAIL")
    fail_detail = ", ".join(fail_parts) if fail_parts else "0 FAIL"
    lines.append(f"- **Total Assertions**: {total_pass + total_fail} ({total_pass} PASS, {fail_detail})")
    lines.append(f"- **All Passed**: {'✅ Yes' if total_tb_fail == 0 else '❌ No'}")
    lines.append("")

    # === Failure Details ===
    failed = [r for r in results if r.verdict != "PASS"]
    if failed:
        lines.append("## Failure Details")
        lines.append("")
        for r in failed:
            lines.append(f"### {r.testbench} — {r.verdict}")
            lines.append("")
            if r.errors:
                lines.append("**Errors:**")
                for e in r.errors:
                    lines.append(f"- {e}")
                lines.append("")
            if r.x_fail_tests:
                lines.append(f"**X/Timing Failures ({len(r.x_fail_tests)}):**")
                for t in r.x_fail_tests[:10]:
                    lines.append(f"- {t}")
                if len(r.x_fail_tests) > 10:
                    lines.append(f"- ... and {len(r.x_fail_tests) - 10} more")
                lines.append("")
            if r.logic_fail_tests:
                lines.append(f"**Logic Failures ({len(r.logic_fail_tests)}):**")
                for t in r.logic_fail_tests[:10]:
                    lines.append(f"- {t}")
                if len(r.logic_fail_tests) > 10:
                    lines.append(f"- ... and {len(r.logic_fail_tests) - 10} more")
                lines.append("")
            if r.fail_tests:
                lines.append(f"**Unclassified Failures ({len(r.fail_tests)}):**")
                for t in r.fail_tests[:10]:
                    lines.append(f"- {t}")
                if len(r.fail_tests) > 10:
                    lines.append(f"- ... and {len(r.fail_tests) - 10} more")
                lines.append("")

    md = "\n".join(lines) + "\n"

    if output_path:
        Path(output_path).parent.mkdir(parents=True, exist_ok=True)
        Path(output_path).write_text(md)
        print(f"Report written to {output_path}")

    return md


def main():
    parser = argparse.ArgumentParser(
        description="Parse VCS simulation logs and generate Markdown summary."
    )
    parser.add_argument(
        "--log-dir", default="./sim/log",
        help="Path to simulation log directory (default: ./sim/log)"
    )
    parser.add_argument(
        "--output", "-o", default=None,
        help="Output Markdown file (default: report/<mode>_sim_report.md)"
    )
    parser.add_argument(
        "--filter", default=None,
        help="Regex filter for testbench names (e.g. 'tb_vaddu|tb_vmulu')"
    )
    parser.add_argument(
        "--mode", choices=["pre-sim", "post-sim"], default="pre-sim",
        help="Simulation mode label (default: pre-sim)"
    )
    args = parser.parse_args()

    log_dir = args.log_dir

    if not os.path.isdir(log_dir):
        print(f"Log directory not found: {log_dir}", file=sys.stderr)
        sys.exit(1)

    # Discover testbenches from run logs
    tb_names = []
    for f in sorted(os.listdir(log_dir)):
        m = re.match(r'(tb_\w+)\.run\.log$', f)
        if m:
            tb_names.append(m.group(1))

    if args.filter:
        pattern = re.compile(args.filter)
        tb_names = [t for t in tb_names if pattern.search(t)]

    if not tb_names:
        print("No simulation logs found.", file=sys.stderr)
        sys.exit(1)

    print(f"Parsing {len(tb_names)} testbench(es): {', '.join(tb_names)}")
    results = [parse_testbench(log_dir, tb) for tb in tb_names]

    output_path = args.output
    if output_path is None:
        output_path = f"report/{args.mode.replace('-', '_')}_sim_report.md"

    md = generate_markdown(results, mode=args.mode, output_path=output_path)
    print(md)


if __name__ == "__main__":
    main()

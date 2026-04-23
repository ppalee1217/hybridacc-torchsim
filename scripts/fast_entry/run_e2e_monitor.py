#!/usr/bin/env python3

from __future__ import annotations

import argparse
import curses
import signal
import sys
import time
from dataclasses import dataclass
from pathlib import Path

FIELD_SEP = "\x1f"
PHASE_ORDER = {
    "queued": 0,
    "compile": 1,
    "gendram": 2,
    "sim": 3,
    "verify": 4,
    "trace": 5,
    "done": 6,
    "failed": 6,
    "skipped": 6,
}


@dataclass(slots=True)
class StatusRecord:
    index: int
    phase: str
    name: str
    message: str


@dataclass(slots=True)
class ResultRecord:
    index: int
    result: str
    name: str
    detail: str
    log_path: str
    updated_at: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Render run_e2e batch progress.")
    parser.add_argument("--status-dir", required=True)
    parser.add_argument("--total", type=int, required=True)
    parser.add_argument("--jobs", type=int, required=True)
    parser.add_argument("--interval", type=float, default=0.2)
    parser.add_argument("--max-visible", type=int, default=12)
    parser.add_argument("--max-recent", type=int, default=6)
    return parser.parse_args()


def read_status_file(path: Path) -> StatusRecord | None:
    try:
        payload = path.read_text(encoding="utf-8", errors="replace").rstrip("\n")
    except FileNotFoundError:
        return None

    parts = payload.split(FIELD_SEP)
    if len(parts) != 3:
        return None

    try:
        index = int(path.stem)
    except ValueError:
        index = 0

    phase, name, message = parts
    return StatusRecord(index=index, phase=phase, name=name, message=message)


def load_statuses(status_dir: Path) -> list[StatusRecord]:
    statuses: list[StatusRecord] = []
    for path in sorted(status_dir.glob("*.status"), key=lambda item: int(item.stem)):
        status = read_status_file(path)
        if status is not None:
            statuses.append(status)
    return statuses


def read_result_file(path: Path) -> ResultRecord | None:
    try:
        payload = path.read_text(encoding="utf-8", errors="replace").rstrip("\n")
        updated_at = path.stat().st_mtime
    except FileNotFoundError:
        return None

    parts = payload.split(FIELD_SEP)
    if len(parts) != 4:
        return None

    try:
        index = int(path.stem)
    except ValueError:
        index = 0

    result, name, detail, log_path = parts
    return ResultRecord(
        index=index,
        result=result,
        name=name,
        detail=detail,
        log_path=log_path,
        updated_at=updated_at,
    )


def load_results(status_dir: Path) -> list[ResultRecord]:
    results: list[ResultRecord] = []
    for path in status_dir.glob("*.result"):
        result = read_result_file(path)
        if result is not None:
            results.append(result)
    results.sort(key=lambda item: item.updated_at, reverse=True)
    return results


def render_progress_bar(done: int, total: int, width: int = 28) -> str:
    if total <= 0:
        return "[" + ("-" * width) + "]"
    filled = min(width, done * width // total)
    return "[" + ("#" * filled) + ("-" * (width - filled)) + "]"


def phase_index(phase: str) -> int:
    return PHASE_ORDER.get(phase, 0)


def format_duration(seconds: float) -> str:
    total_seconds = max(0, int(seconds))
    hours, remainder = divmod(total_seconds, 3600)
    minutes, secs = divmod(remainder, 60)
    if hours > 0:
        return f"{hours:02d}:{minutes:02d}:{secs:02d}"
    return f"{minutes:02d}:{secs:02d}"


def estimate_eta(done: int, total: int, elapsed: float) -> str:
    if done <= 0 or elapsed <= 0:
        return "--:--"
    remaining = max(0, total - done)
    if remaining == 0:
        return "00:00"
    eta_seconds = elapsed * remaining / done
    return format_duration(eta_seconds)


def build_lines(
    statuses: list[StatusRecord],
    results: list[ResultRecord],
    total: int,
    jobs: int,
    max_visible: int,
    max_recent: int,
    elapsed: float,
) -> list[str]:
    done = 0
    passed = 0
    failed = 0
    queued = 0
    running = 0

    active: list[StatusRecord] = []
    for status in statuses:
        if status.phase == "done":
            done += 1
            passed += 1
        elif status.phase == "failed":
            done += 1
            failed += 1
        elif status.phase == "queued":
            queued += 1
            active.append(status)
        else:
            running += 1
            active.append(status)

    lines = [
        f"Parallel E2E Progress  jobs={jobs}",
        f"{render_progress_bar(done, total)}  {done}/{total} complete  pass={passed} fail={failed} running={running} queued={queued}",
        f"elapsed={format_duration(elapsed)}  eta={estimate_eta(done, total, elapsed)}",
        "",
    ]

    shown = 0
    for status in active[:max_visible]:
        lines.append(f"  [{phase_index(status.phase)}/6] {status.name:<36} {status.message}")
        shown += 1

    if shown == 0:
        lines.append("  Waiting for workers to finish...")

    lines.append("")
    lines.append("Recent completions:")
    if results:
        for result in results[:max_recent]:
            detail = result.detail if result.detail else "completed"
            lines.append(f"  [{result.result:<4}] {result.name:<36} {detail}")
    else:
        lines.append("  No completed workloads yet.")

    lines.extend(["", "Each task writes full logs to <build_dir>/e2e_run.log"])
    return lines


def truncate(line: str, width: int) -> str:
    if width <= 0:
        return ""
    return line[: max(0, width - 1)]


def is_complete(statuses: list[StatusRecord], total: int) -> bool:
    finished = sum(1 for status in statuses if status.phase in {"done", "failed"})
    return finished >= total


def wait_without_tui(status_dir: Path, total: int, interval: float) -> None:
    while True:
        if is_complete(load_statuses(status_dir), total):
            return
        time.sleep(interval)


def run_curses(
    stdscr: curses.window,
    status_dir: Path,
    total: int,
    jobs: int,
    interval: float,
    max_visible: int,
    max_recent: int,
    started_at: float,
) -> None:
    try:
        curses.curs_set(0)
    except curses.error:
        pass

    stdscr.nodelay(True)
    stdscr.timeout(0)
    previous_line_count = 0

    while True:
        statuses = load_statuses(status_dir)
        results = load_results(status_dir)
        elapsed = time.monotonic() - started_at
        lines = build_lines(statuses, results, total, jobs, max_visible, max_recent, elapsed)
        height, width = stdscr.getmaxyx()

        for row, line in enumerate(lines[:height]):
            stdscr.move(row, 0)
            stdscr.clrtoeol()
            stdscr.addnstr(row, 0, truncate(line, width), max(0, width - 1))

        for row in range(len(lines), min(previous_line_count, height)):
            stdscr.move(row, 0)
            stdscr.clrtoeol()

        previous_line_count = len(lines)
        stdscr.refresh()

        if is_complete(statuses, total):
            return

        time.sleep(interval)


def main() -> int:
    args = parse_args()
    status_dir = Path(args.status_dir)
    started_at = time.monotonic()

    signal.signal(signal.SIGTERM, lambda *_args: sys.exit(0))
    signal.signal(signal.SIGINT, lambda *_args: sys.exit(0))

    if not sys.stdout.isatty() or not status_dir.exists():
        wait_without_tui(status_dir, args.total, args.interval)
        return 0

    try:
        curses.wrapper(
            run_curses,
            status_dir,
            args.total,
            args.jobs,
            args.interval,
            args.max_visible,
            args.max_recent,
            started_at,
        )
    except curses.error:
        wait_without_tui(status_dir, args.total, args.interval)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
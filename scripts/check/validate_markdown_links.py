#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from urllib.parse import unquote


DEFAULT_PATTERNS = [
    "README.md",
    "doc/**/*.md",
    "design/**/README.md",
    "design/**/doc/**/*.md",
    "python/**/README*.md",
]
LINK_PATTERN = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate that local markdown links resolve to existing files."
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="Repository root used to expand glob patterns.",
    )
    parser.add_argument(
        "patterns",
        nargs="*",
        default=DEFAULT_PATTERNS,
        help="Glob patterns relative to the repository root.",
    )
    return parser.parse_args()


def iter_markdown_files(repo_root: Path, patterns: list[str]) -> list[Path]:
    files: set[Path] = set()
    for pattern in patterns:
        files.update(path for path in repo_root.glob(pattern) if path.is_file())
    return sorted(files)


def normalize_target(target: str) -> str | None:
    candidate = target.strip()
    if not candidate or candidate.startswith("#"):
        return None
    if "://" in candidate or candidate.startswith("mailto:"):
        return None
    if candidate.startswith("<") and candidate.endswith(">"):
        candidate = candidate[1:-1].strip()
    path_part = candidate.split("#", 1)[0].strip()
    return path_part or None


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    files = iter_markdown_files(repo_root, args.patterns)
    if not files:
        print("No markdown files matched the requested patterns.", file=sys.stderr)
        return 2

    errors: list[str] = []
    for file_path in files:
        for line_no, line in enumerate(file_path.read_text(encoding="utf-8").splitlines(), start=1):
            for match in LINK_PATTERN.finditer(line):
                target = normalize_target(match.group(1))
                if target is None:
                    continue
                resolved = (file_path.parent / unquote(target)).resolve()
                if resolved.exists():
                    continue
                rel_file = file_path.relative_to(repo_root)
                errors.append(f"{rel_file}:{line_no}: {match.group(1)} -> missing")

    if errors:
        print("\n".join(errors))
        return 1

    print(f"OK: checked {len(files)} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
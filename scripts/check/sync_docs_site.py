#!/usr/bin/env python3

from __future__ import annotations

import shutil
from collections import deque
from pathlib import Path
from urllib.parse import unquote

from validate_markdown_links import DEFAULT_PATTERNS, LINK_PATTERN, normalize_target


BLOCKED_SUFFIXES = {
    ".a",
    ".bin",
    ".fsdb",
    ".o",
    ".so",
    ".vcd",
    ".vpd",
}


def should_copy_asset(path: Path) -> bool:
    return path.is_file() and path.suffix.lower() not in BLOCKED_SUFFIXES


def copy_into_stage(repo_root: Path, stage_root: Path, source: Path) -> None:
    destination = stage_root / source.relative_to(repo_root)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def seed_markdown_files(repo_root: Path) -> set[Path]:
    files: set[Path] = set()
    for pattern in DEFAULT_PATTERNS:
        files.update(path for path in repo_root.glob(pattern) if path.is_file())
    return files


def main() -> int:
    repo_root = Path(__file__).resolve().parents[2]
    stage_root = repo_root / ".docs-site-src"

    if stage_root.exists():
        shutil.rmtree(stage_root)
    stage_root.mkdir(parents=True)

    queued = seed_markdown_files(repo_root)
    seen_markdown: set[Path] = set()
    asset_files: set[Path] = set()
    queue = deque(sorted(queued))

    while queue:
        file_path = queue.popleft()
        if file_path in seen_markdown:
            continue
        seen_markdown.add(file_path)

        for line in file_path.read_text(encoding="utf-8").splitlines():
            for match in LINK_PATTERN.finditer(line):
                target = normalize_target(match.group(1))
                if target is None:
                    continue
                resolved = (file_path.parent / unquote(target)).resolve()
                if not resolved.exists() or resolved.is_dir() or repo_root not in resolved.parents and resolved != repo_root:
                    continue
                if resolved.suffix.lower() == ".md":
                    if resolved not in seen_markdown:
                        queue.append(resolved)
                    continue
                if should_copy_asset(resolved):
                    asset_files.add(resolved)

    for source in sorted(seen_markdown):
        copy_into_stage(repo_root, stage_root, source)
    for source in sorted(asset_files):
        copy_into_stage(repo_root, stage_root, source)

    print(
        f"Staged {len(seen_markdown)} markdown files and {len(asset_files)} linked assets into {stage_root}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
"""Emit exact, machine-readable provenance for a ``hacc-compile`` invocation."""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Iterable


SCHEMA = "hybridacc-cc-toolchain-provenance/v1"
SIDECAR_NAME = "toolchain_provenance.json"
_COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
_PACKAGE_DIR = Path(__file__).resolve().parent
_DEFAULT_TEMPLATE_DIR = _PACKAGE_DIR / "templates"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _file_hashes(root: Path, files: Iterable[Path]) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): _sha256(path)
        for path in sorted(files)
    }


def _tree_hashes(root: Path) -> dict[str, str]:
    files = (
        path
        for path in root.rglob("*")
        if path.is_file()
        and "__pycache__" not in path.parts
        and path.suffix not in {".pyc", ".pyo"}
    )
    return _file_hashes(root, files)


def _mapping_sha256(files_sha256: dict[str, str]) -> str:
    canonical = json.dumps(
        files_sha256, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def _git_value(*args: str) -> str | None:
    repository = _PACKAGE_DIR.parents[1]
    try:
        completed = subprocess.run(
            ["git", "-C", str(repository), *args],
            capture_output=True,
            text=True,
            check=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None
    return completed.stdout.strip()


def _git_identity() -> dict[str, Any]:
    supplied = os.environ.get("HYBRIDACC_CC_GIT_COMMIT", "").strip().lower()
    if supplied and not _COMMIT_RE.fullmatch(supplied):
        raise ValueError("HYBRIDACC_CC_GIT_COMMIT must be a 40-digit lowercase hex commit")
    discovered = _git_value("rev-parse", "HEAD")
    if discovered:
        discovered = discovered.lower()
        if not _COMMIT_RE.fullmatch(discovered):
            discovered = None
    if supplied and discovered and supplied != discovered:
        raise ValueError(
            "HYBRIDACC_CC_GIT_COMMIT disagrees with the mounted source repository"
        )
    commit = supplied or discovered
    dirty_text = os.environ.get("HYBRIDACC_CC_GIT_DIRTY")
    if dirty_text not in (None, "0", "1"):
        raise ValueError("HYBRIDACC_CC_GIT_DIRTY must be 0 or 1")
    if dirty_text is not None:
        dirty: bool | None = dirty_text == "1"
        dirty_source = "HYBRIDACC_CC_GIT_DIRTY"
    else:
        status = _git_value("status", "--short", "--untracked-files=all")
        dirty = None if status is None else bool(status)
        dirty_source = "git" if status is not None else "unavailable"
    return {
        "git_commit": commit,
        "git_commit_source": (
            "HYBRIDACC_CC_GIT_COMMIT" if supplied else ("git" if discovered else "unavailable")
        ),
        "worktree_dirty": dirty,
        "worktree_dirty_source": dirty_source,
    }


def _atomic_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, prefix=f".{path.name}.", delete=False
    ) as handle:
        json.dump(value, handle, indent=2, sort_keys=True)
        handle.write("\n")
        temporary = Path(handle.name)
    os.replace(temporary, path)


def write_toolchain_provenance(
    output_dir: Path,
    *,
    package_version: str,
    template_dir: Path | None,
    march: str,
    gcc: str,
    opt_level: str,
    mmio_opt_level: str | None,
    stack_size: int,
    compilation_requested: bool,
    gcc_command: list[str],
) -> Path:
    """Write a sidecar after source generation and before optional GCC execution."""
    output_dir = output_dir.resolve()
    templates = (template_dir or _DEFAULT_TEMPLATE_DIR).resolve()
    template_files = _tree_hashes(templates)
    package_files = _tree_hashes(_PACKAGE_DIR)
    generated_files = {
        name: _sha256(output_dir / name)
        for name in (
            "firmware_hw.h",
            "firmware_payload.h",
            "firmware_data.c",
            "firmware_ops.c",
            "firmware_main.c",
            "linker.ld",
        )
        if (output_dir / name).is_file()
    }
    git_identity = _git_identity()
    errors = []
    if git_identity["git_commit"] is None:
        errors.append("cc_git_commit_unavailable")
    firmware_template_hash = template_files.get("firmware_ops.c.j2")
    if firmware_template_hash is None:
        errors.append("firmware_ops.c.j2_missing")
    record = {
        "schema": SCHEMA,
        "status": "COMPLETE" if not errors else "INCOMPLETE",
        "errors": errors,
        "cc": {
            **git_identity,
            "package_version": package_version,
            "package_directory_sha256": _mapping_sha256(package_files),
            "package_files_sha256": package_files,
        },
        "templates": {
            "directory_sha256": _mapping_sha256(template_files),
            "files_sha256": template_files,
            "firmware_ops_c_j2_sha256": firmware_template_hash,
        },
        "compile": {
            "march": march,
            "gcc": gcc,
            "opt_level": opt_level,
            "mmio_opt_level": mmio_opt_level,
            "stack_size": stack_size,
            "compilation_requested": compilation_requested,
            "gcc_command": gcc_command,
        },
        "generated_sources_sha256": generated_files,
        "digest_definition": (
            "SHA-256 over file bytes; directory digest is SHA-256 of compact, "
            "key-sorted JSON mapping relative POSIX path to file SHA-256"
        ),
    }
    path = output_dir / SIDECAR_NAME
    _atomic_json(path, record)
    return path

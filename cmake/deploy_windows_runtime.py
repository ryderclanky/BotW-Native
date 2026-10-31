#!/usr/bin/env python3
"""Copy the non-system DLL closure of a MinGW executable beside it."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


def imported_dlls(binary: Path, objdump: str) -> list[str]:
    result = subprocess.run(
        [objdump, "-p", str(binary)], capture_output=True, text=True, errors="replace", check=False
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"objdump could not inspect {binary}: {detail}")
    names = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if line.startswith("DLL Name:"):
            name = line.split(":", 1)[1].strip()
            if name:
                names.append(name)
    return names


def is_system_dll(name: str, system_dir: Path) -> bool:
    folded = name.casefold()
    if folded.startswith(("api-ms-win-", "ext-ms-win-")):
        return True
    # The Vulkan loader is supplied by the graphics driver, not by this app.
    if folded in {"vulkan-1.dll", "libvulkan-1.dll"}:
        return True
    return (system_dir / name).is_file()


def same_file_contents(first: Path, second: Path) -> bool:
    try:
        if first.stat().st_size != second.stat().st_size:
            return False
        with first.open("rb") as a, second.open("rb") as b:
            while True:
                a_chunk = a.read(1024 * 1024)
                b_chunk = b.read(1024 * 1024)
                if a_chunk != b_chunk:
                    return False
                if not a_chunk:
                    return True
    except OSError:
        return False


def copy_runtime_file(source: Path, target: Path) -> bool:
    if source.resolve() == target.resolve() or same_file_contents(source, target):
        return False
    target.parent.mkdir(parents=True, exist_ok=True)
    staged = target.with_name(target.name + ".new")
    try:
        shutil.copy2(source, staged)
        try:
            os.replace(staged, target)
        except OSError:
            # A launcher may have an older DLL loaded. Windows allows its file
            # to be renamed in place; keep it aside and install the new version.
            old = None
            for suffix in [".old", *(f".old{i}" for i in range(1, 100))]:
                candidate = target.with_name(target.name + suffix)
                if not candidate.exists():
                    try:
                        target.rename(candidate)
                        old = candidate
                        break
                    except OSError:
                        continue
            if old is None:
                raise
            try:
                staged.rename(target)
            except OSError:
                if not target.exists():
                    try:
                        old.rename(target)
                    except OSError:
                        pass
                raise
    except OSError as error:
        staged.unlink(missing_ok=True)
        raise RuntimeError(f"cannot deploy {source} beside {target}: {error}") from error
    return True


def search_directories(binary_dir: Path, explicit: list[str]) -> list[Path]:
    result: list[Path] = []
    seen: set[str] = set()
    for value in [*explicit, *os.environ.get("PATH", "").split(os.pathsep), str(binary_dir)]:
        if not value:
            continue
        path = Path(value)
        try:
            key = str(path.resolve()).casefold()
        except OSError:
            key = str(path).casefold()
        if key not in seen:
            seen.add(key)
            result.append(path)
    return result


def deploy(binary: Path, objdump: str, explicit_dirs: list[str]) -> list[str]:
    binary = binary.resolve()
    destination = binary.parent
    destination.mkdir(parents=True, exist_ok=True)
    system_root = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    system_dir = system_root / "System32"
    search_dirs = search_directories(destination, explicit_dirs)

    pending = [binary]
    inspected: set[str] = set()
    copied: list[str] = []
    while pending:
        current = pending.pop()
        current_key = str(current.resolve()).casefold()
        if current_key in inspected:
            continue
        inspected.add(current_key)

        for name in imported_dlls(current, objdump):
            folded = name.casefold()
            if folded in inspected or is_system_dll(name, system_dir):
                continue

            source = next((directory / name for directory in search_dirs if (directory / name).is_file()), None)
            target = destination / name
            if source is None and target.is_file():
                source = target
            if source is None:
                raise RuntimeError(
                    f"runtime DLL {name} required by {current.name} was not found in the compiler/runtime paths"
                )

            if copy_runtime_file(source, target):
                copied.append(name)
                current_dependency = target
            else:
                current_dependency = source
            inspected.add(folded)
            pending.append(current_dependency)

    return copied


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--search-dir", action="append", default=[])
    args = parser.parse_args()
    try:
        copied = deploy(args.binary, args.objdump, args.search_dir)
    except (OSError, RuntimeError) as error:
        print(f"runtime deployment failed: {error}", file=sys.stderr)
        return 1
    if copied:
        print(f"runtime dependencies beside {args.binary.name}: {', '.join(sorted(set(copied)))}")
    else:
        print(f"runtime dependencies beside {args.binary.name} are current")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

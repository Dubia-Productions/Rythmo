#!/usr/bin/env python3
"""Bundle the non-glibc shared libraries needed by Rythmo and Qt plugins."""
from __future__ import annotations

import pathlib
import re
import shutil
import subprocess
import sys

root = pathlib.Path(sys.argv[1]).resolve()
lib_dir = root / "lib"
lib_dir.mkdir(exist_ok=True)
queue = [root / "Rythmo.bin", root / "ffmpeg", root / "ffprobe"]
queue.extend((root / "plugins").rglob("*.so"))
seen: set[pathlib.Path] = set()
system_runtime = re.compile(
    r"^(?:libc\.so|libm\.so|libpthread\.so|libdl\.so|librt\.so|ld-linux)"
)
dependency_re = re.compile(r"=>\s+(/[^\s]+)|^\s*(/[^\s]+)")

while queue:
    binary = queue.pop()
    try:
        resolved_binary = binary.resolve(strict=True)
    except OSError:
        continue
    if resolved_binary in seen or not resolved_binary.is_file():
        continue
    seen.add(resolved_binary)
    result = subprocess.run(
        ["ldd", str(resolved_binary)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode:
        raise SystemExit(f"ldd failed for {resolved_binary}:\n{result.stdout}")
    if "not found" in result.stdout:
        raise SystemExit(f"Unresolved runtime dependency for {resolved_binary}:\n{result.stdout}")

    for line in result.stdout.splitlines():
        match = dependency_re.search(line)
        if not match:
            continue
        dependency = pathlib.Path(match.group(1) or match.group(2))
        if system_runtime.match(dependency.name):
            continue
        resolved = dependency.resolve(strict=True)
        metadata = subprocess.run(
            ["readelf", "-d", str(resolved)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        soname = re.search(r"\(SONAME\).*?\[(.*?)\]", metadata.stdout)
        name = soname.group(1) if soname else resolved.name
        destination = lib_dir / name
        if destination.exists():
            if destination.resolve() != resolved:
                print(
                    f"Using first compatible SONAME {name}: {destination.resolve()} "
                    f"(also required by {resolved})",
                    file=sys.stderr,
                )
        else:
            shutil.copy2(resolved, destination)
        queue.append(resolved)

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Pack Blender's installed run-time files (``bin/<version>/``: data-files, scripts, Python)
into one binary file and a JSON index, loaded by ``web/shell/blender_loader.js``.

Usage: package_data.py <build>/bin/<version> <output dir>

Writes ``blender-data.bin`` and ``blender-data.json`` (``{"files": [[path, offset, size], ...]}``).
Paths are absolute in the virtual file-system, e.g. ``/5.4/scripts/startup/...``.
"""

import json
import os
import sys

# Parts of the Python standard library that can't work in a browser or aren't needed.
PYTHON_EXCLUDE_DIRS = {
    "test", "tests", "idlelib", "tkinter", "turtledemo", "ensurepip", "lib2to3",
    "venv", "__pycache__", "pydoc_data", "unittest", "config-3.13-wasm32-emscripten",
}
EXCLUDE_SUFFIXES = (".pyc", ".a")
# Top level directories of the version directory to skip (the stand-alone Python executable).
EXCLUDE_PATHS = {"python/bin"}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    version_dir = os.path.abspath(sys.argv[1])
    out_dir = sys.argv[2]
    version = os.path.basename(version_dir)
    os.makedirs(out_dir, exist_ok=True)

    files = []
    total = 0
    with open(os.path.join(out_dir, "blender-data.bin"), "wb") as fh_out:
        for root, dirs, filenames in os.walk(version_dir):
            in_python = os.sep + "python" + os.sep in root + os.sep
            rel_root = os.path.relpath(root, version_dir).replace(os.sep, "/")
            dirs[:] = sorted(
                d for d in dirs
                if not (in_python and d in PYTHON_EXCLUDE_DIRS) and
                (rel_root + "/" + d).lstrip("./") not in EXCLUDE_PATHS
            )
            for name in sorted(filenames):
                if name.endswith(EXCLUDE_SUFFIXES):
                    continue
                path = os.path.join(root, name)
                with open(path, "rb") as fh_in:
                    data = fh_in.read()
                virtual_path = "/" + version + "/" + os.path.relpath(path, version_dir).replace(os.sep, "/")
                files.append([virtual_path, total, len(data)])
                fh_out.write(data)
                total += len(data)

    with open(os.path.join(out_dir, "blender-data.json"), "w", encoding="utf-8") as fh:
        json.dump({"files": files, "size": total}, fh, separators=(",", ":"))
    print("Packed {:d} files, {:.1f} MB".format(len(files), total / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())

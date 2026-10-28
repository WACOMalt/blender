#!/usr/bin/env bash
# Fetch the Git LFS files needed to build (startup.blend, fonts, icons, assets...).
# Forks on GitHub do not have Blender's LFS objects, they are served by projects.blender.org.
# Test files are skipped, they are large and not needed to build.
set -euo pipefail
cd "$(dirname "$0")/.."
git -c lfs.url=https://projects.blender.org/blender/blender.git/info/lfs \
  lfs pull --exclude="tests/**,release/windows/**,release/darwin/**"

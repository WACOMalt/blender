# Blender Xe-LP fork — build & install guide (Arch Linux)

Fork of Blender enabling **Cycles GPU rendering on Intel Xe-LP integrated
GPUs** (Tiger Lake Iris Xe, e.g. i7-1165G7) via oneAPI/Level-Zero — hardware
Blender upstream excludes. See the `xelp-fork-5.2` branch commits for the
actual changes (software texture sampling replacing bindless images, device
filter relaxation, plus Arch build fixes).

## Dependencies (one-time)

```sh
sudo pacman -S --needed intel-compute-runtime level-zero-loader level-zero-headers \
  intel-oneapi-dpcpp-cpp intel-oneapi-compiler-shared-runtime \
  intel-oneapi-compiler-dpcpp-cpp-runtime-libs intel-oneapi-compiler-shared-runtime-libs \
  boost eigen cmake ninja mold llvm mesa git-lfs subversion wayland-protocols \
  vulkan-headers libdecor alembic boost-libs ceres-solver draco embree ffmpeg fftw fmt \
  glew gmp imath jack jemalloc libharu libspnav manifold materialx onetbb openal \
  opencolorio openexr openimagedenoise openimageio openjpeg2 openpgl \
  openshadinglanguage opensubdiv openvdb openxr potrace pugixml pystring \
  python-numpy python-requests python-zstandard cython sdl2 usd yaml-cpp \
  pacman-contrib patchelf
```

## Configure & build

Needs ~15 GB free disk and ~3–4 h on a 4-core TGL laptop. The
`libcycles_kernel_oneapi_aot.so` target is silent for ~1 h (AOT kernel
compile for `tgllp`) and is RAM-hungry — avoid running renders meanwhile.

```sh
cmake -G Ninja -B build -C src/build_files/cmake/config/blender_release.cmake -S src \
  -D CMAKE_BUILD_TYPE=Release \
  -D WITH_INSTALL_PORTABLE=ON \
  -D WITH_PYTHON_INSTALL=ON \
  -D PYTHON_VERSION=3.14 \
  -D CMAKE_INSTALL_PREFIX="$HOME/Blender/custom/blender-5.2.0-xelp" \
  -D WITH_CYCLES_DEVICE_ONEAPI=ON \
  -D WITH_CYCLES_ONEAPI_BINARIES=ON \
  -D CYCLES_ONEAPI_INTEL_BINARIES_ARCH=tgllp \
  -D SYCL_ROOT_DIR=/opt/intel/oneapi/compiler/latest \
  -D OCLOC_INSTALL_DIR=/usr \
  -D SYCL_OFFLINE_COMPILER_PARALLEL_JOBS=2 \
  -D WITH_SYSTEM_GLOG=ON -D WITH_SYSTEM_GFLAGS=ON \
  -D WITH_CYCLES_CUDA_BINARIES=OFF -D WITH_CYCLES_DEVICE_CUDA=OFF \
  -D WITH_CYCLES_DEVICE_OPTIX=OFF -D WITH_CYCLES_HIP_BINARIES=OFF \
  -D WITH_CYCLES_DEVICE_HIP=OFF -D WITH_CYCLES_DEVICE_HIPRT=OFF
cd build && nice -n 10 ninja -j6 && ninja install
```

Notes:
- `PYTHON_VERSION=3.14`: Blender 5.2 officially wants 3.13; Arch ships newer.
- `WITH_SYSTEM_GLOG/GFLAGS=ON`: required — the bundled glog collides with
  system ceres-solver's glog (duplicate `logtostderr` flag → abort at start).
- `SYCL_OFFLINE_COMPILER_PARALLEL_JOBS=2` keeps AOT peak RAM inside 16 GB.
- The install's man-page step fails harmlessly (binary lacks its rpath until
  the post-install step below).

## Post-install (required)

The installed binary needs an rpath for the Cycles kernel lib, and a launcher
shim (`xelp_shim.c`, repo root) that adds Intel UMF to the library path —
Arch splits oneAPI into components and the UR Level-Zero adapter dlopens
`libumf.so.1` from a non-default path. The shim must be a compiled binary,
not a shell script: Blender Launcher's build probe fails on interpreter
scripts with exit 127.

```sh
D="$HOME/Blender/custom/blender-5.2.0-xelp"
mv "$D/blender" "$D/blender-bin"
patchelf --set-rpath '$ORIGIN/lib' "$D/blender-bin"
gcc -O2 -o "$D/blender" xelp_shim.c
```

The build then appears in Blender Launcher's Custom tab, and the Iris Xe
shows up under Preferences → System → Cycles Render Devices → oneAPI with no
environment variables needed.

## Maintenance

- **After `pacman -Syu` upgrades of USD / embree / OpenVDB / OpenImageIO
  etc.** the binary may fail with a `symbol lookup error` — rebuild
  (incremental if `build/` was kept: `cmake -S src -B build && ninja &&
  ninja install` + post-install steps).
- **Rebasing onto a new Blender release**: cherry-pick the fork commits onto
  the new tag; the texture-sampling commit is the one that may conflict.

## Known limitation (under investigation)

Image textures loaded from saved .blend files can render as flat
average-color/black on the GPU when they route through the 5.2 texture-cache
tile-streaming path; freshly created in-session images are unaffected.
Diagnosis in progress (tile loading appears to never complete on this
device); workaround: none yet on GPU — CPU renders are correct.

#!/usr/bin/env bash
# Build the dependencies Blender needs for wasm32-emscripten into one prefix.
#
# Usage:
#   source web/setup_emsdk.sh
#   web/build_deps.sh [package ...]     # no arguments: build all packages in order
#
# Environment:
#   BLENDER_WEB_DEPS  root for sources, build trees and the install prefix
#                     (default: $HOME/blender-web-deps). The prefix is $BLENDER_WEB_DEPS/install.
#   JOBS              parallel jobs (default: nproc)
#
# Versions follow build_files/build_environment/cmake/versions.cmake.
# GitHub sources are fetched with `git clone` because some sandboxed
# environments block GitHub archive downloads but allow git.
#
# Every package is compiled with the same flags as Blender itself
# (see build_files/cmake/platform/platform_emscripten.cmake): threads,
# native wasm exceptions and wasm SIMD. Objects built with different
# -pthread/-fwasm-exceptions settings cannot be linked together.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="${BLENDER_WEB_DEPS:-$HOME/blender-web-deps}"
SRC="$DEPS/src"
BUILD="$DEPS/build"
PREFIX="$DEPS/install"
JOBS="${JOBS:-$(nproc)}"
WEB_FLAGS="-pthread -fwasm-exceptions -msimd128"

command -v emcmake >/dev/null || { echo "emcmake not found; run: source web/setup_emsdk.sh" >&2; exit 1; }
mkdir -p "$SRC" "$BUILD" "$PREFIX"

# fetch_git <name> <url> <tag>
fetch_git() {
  local dir="$SRC/$1"
  [ -d "$dir" ] || git -c advice.detachedHead=false clone -q --depth 1 --branch "$3" "$2" "$dir"
}

# fetch_url <name> <url>: download a tarball and extract it with the top directory stripped.
fetch_url() {
  local dir="$SRC/$1"
  [ -d "$dir" ] && return 0
  local tmp="$SRC/$1.download"
  curl -fsSL --retry 4 -o "$tmp" "$2"
  mkdir -p "$dir"
  tar -xf "$tmp" -C "$dir" --strip-components=1
  rm -f "$tmp"
}

# cmake_build <name> <source dir> [cmake args ...]
cmake_build() {
  local name="$1" src="$2"
  shift 2
  emcmake cmake -S "$src" -B "$BUILD/$name" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
    -DCMAKE_C_FLAGS="$WEB_FLAGS" \
    -DCMAKE_CXX_FLAGS="$WEB_FLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$WEB_FLAGS" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DEMSCRIPTEN_SYSTEM_PROCESSOR=wasm32 \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF \
    "$@"
  cmake --build "$BUILD/$name" -j "$JOBS"
  cmake --install "$BUILD/$name"
}

build_zlib() {
  fetch_url zlib https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
  cmake_build zlib "$SRC/zlib" -DZLIB_BUILD_EXAMPLES=OFF
  # zlib always builds a shared library, remove it so everything links the static one.
  rm -f "$PREFIX"/lib/libz.so*
}

build_zstd() {
  fetch_url zstd https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
  cmake_build zstd "$SRC/zstd/build/cmake" \
    -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_TESTS=OFF
}

build_png() {
  fetch_url png http://prdownloads.sourceforge.net/libpng/libpng-1.6.58.tar.xz
  cmake_build png "$SRC/png" \
    -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF \
    -DPNG_HARDWARE_OPTIMIZATIONS=OFF
}

build_jpeg() {
  fetch_git jpeg https://github.com/libjpeg-turbo/libjpeg-turbo 2.1.3
  cmake_build jpeg "$SRC/jpeg" \
    -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DWITH_JPEG8=ON -DWITH_TURBOJPEG=OFF
}

build_brotli() {
  fetch_git brotli https://github.com/google/brotli v1.0.9
  cmake_build brotli "$SRC/brotli" -DBROTLI_DISABLE_TESTS=ON
  # Brotli's CMake installs nothing when targeting Emscripten.
  cp "$BUILD"/brotli/libbrotli{common,dec,enc}-static.a "$PREFIX/lib/"
  cp -r "$SRC/brotli/c/include/brotli" "$PREFIX/include/"
}

build_freetype() {
  fetch_url freetype http://prdownloads.sourceforge.net/freetype/freetype-2.13.3.tar.gz
  # Brotli is required for the WOFF2 fonts used by Blender's UI.
  cmake_build freetype "$SRC/freetype" \
    -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_HARFBUZZ=ON \
    -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON -DFT_REQUIRE_BROTLI=ON \
    -DBROTLIDEC_INCLUDE_DIRS="$PREFIX/include" \
    -DBROTLIDEC_LIBRARIES="$PREFIX/lib/libbrotlidec-static.a;$PREFIX/lib/libbrotlicommon-static.a"
}

build_fmt() {
  fetch_git fmt https://github.com/fmtlib/fmt 12.1.0
  cmake_build fmt "$SRC/fmt" -DFMT_DOC=OFF -DFMT_TEST=OFF
}

build_eigen() {
  local ver=8a1083e9bf41b91fdea6546681f806154efdc25a
  fetch_url eigen "https://gitlab.com/libeigen/eigen/-/archive/$ver/eigen-$ver.tar.gz"
  cmake_build eigen "$SRC/eigen" -DEIGEN_BUILD_DOC=OFF -DEIGEN_BUILD_TESTING=OFF -DEIGEN_BUILD_BLAS=OFF \
    -DEIGEN_BUILD_LAPACK=OFF -DEIGEN_BUILD_PKGCONFIG=ON
}

build_imath() {
  fetch_git imath https://github.com/AcademySoftwareFoundation/Imath v3.2.2
  cmake_build imath "$SRC/imath" -DPYTHON=OFF -DIMATH_INSTALL_PKG_CONFIG=ON
}

build_deflate() {
  fetch_git deflate https://github.com/ebiggers/libdeflate v1.18
  cmake_build deflate "$SRC/deflate" \
    -DLIBDEFLATE_BUILD_SHARED_LIB=OFF -DLIBDEFLATE_BUILD_STATIC_LIB=ON -DLIBDEFLATE_BUILD_GZIP=OFF
}

build_openjph() {
  fetch_git openjph https://github.com/aous72/OpenJPH 0.25.2
  cmake_build openjph "$SRC/openjph" \
    -DOJPH_BUILD_EXECUTABLES=OFF -DOJPH_ENABLE_TIFF_SUPPORT=OFF -DOJPH_BUILD_TESTS=OFF -DOJPH_DISABLE_SIMD=ON
}

build_openexr() {
  fetch_git openexr https://github.com/AcademySoftwareFoundation/openexr v3.4.10
  cmake_build openexr "$SRC/openexr" \
    -DOPENEXR_BUILD_TOOLS=OFF -DOPENEXR_INSTALL_TOOLS=OFF -DOPENEXR_BUILD_EXAMPLES=OFF \
    -DOPENEXR_BUILD_PYTHON=OFF -DOPENEXR_FORCE_INTERNAL_DEFLATE=OFF -DOPENEXR_FORCE_INTERNAL_IMATH=OFF \
    -DOPENEXR_FORCE_INTERNAL_OPENJPH=OFF -DOPENEXR_ENABLE_THREADING=ON -DOPENEXR_BUILD_DOCS=OFF \
    -DOPENEXR_TEST_LIBRARIES=OFF -DOPENEXR_TEST_TOOLS=OFF -DOPENEXR_TEST_PYTHON=OFF
}

build_tbb() {
  fetch_git tbb https://github.com/uxlfoundation/oneTBB v2022.3.0
  cmake_build tbb "$SRC/tbb" \
    -DTBB_TEST=OFF -DTBB_EXAMPLES=OFF -DTBB_STRICT=OFF -DTBBMALLOC_BUILD=OFF -DTBBMALLOC_PROXY_BUILD=OFF \
    -DTBB_DISABLE_HWLOC_AUTOMATIC_SEARCH=ON
}

build_tiff() {
  fetch_url tiff https://gitlab.com/libtiff/libtiff/-/archive/v4.7.1/libtiff-v4.7.1.tar.gz
  cmake_build tiff "$SRC/tiff" \
    -Dlzma=OFF -Djbig=OFF -Dzstd=OFF -Dwebp=OFF -Dlerc=OFF -Dlibdeflate=ON -Dcxx=OFF \
    -Dtiff-tests=OFF -Dtiff-tools=OFF -Dtiff-docs=OFF -Dtiff-contrib=OFF -Dsphinx=OFF
  # The exported TiffConfig.cmake references Deflate::Deflate but does not find
  # it ("TODO: import dependencies" upstream); define the target for consumers.
  local cfg="$PREFIX/lib/cmake/tiff/TiffConfig.cmake"
  if ! grep -q 'Deflate::Deflate' "$cfg"; then
    local shim='if(NOT TARGET Deflate::Deflate)
  add_library(Deflate::Deflate UNKNOWN IMPORTED)
  set_target_properties(Deflate::Deflate PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_LIST_DIR}/../../libdeflate.a"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../../include")
endif()'
    printf '%s\n%s' "$shim" "$(cat "$cfg")" > "$cfg"
  fi
}

build_robinmap() {
  fetch_git robinmap https://github.com/Tessil/robin-map v1.3.0
  cmake_build robinmap "$SRC/robinmap"
}

build_expat() {
  fetch_git expat https://github.com/libexpat/libexpat R_2_7_5
  cmake_build expat "$SRC/expat/expat" \
    -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF \
    -DEXPAT_SHARED_LIBS=OFF -DEXPAT_BUILD_PKGCONFIG=ON
}

build_yamlcpp() {
  fetch_git yamlcpp https://github.com/jbeder/yaml-cpp 0.8.0
  cmake_build yamlcpp "$SRC/yamlcpp" \
    -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF -DYAML_CPP_BUILD_CONTRIB=OFF \
    -DYAML_BUILD_SHARED_LIBS=OFF
}

build_pystring() {
  fetch_git pystring https://github.com/imageworks/pystring v1.1.3
  cp "$ROOT/build_files/build_environment/patches/cmakelists_pystring.txt" "$SRC/pystring/CMakeLists.txt"
  cmake_build pystring "$SRC/pystring"
}

build_minizipng() {
  fetch_git minizipng https://github.com/zlib-ng/minizip-ng 4.2.2
  cmake_build minizipng "$SRC/minizipng" \
    -DMZ_FETCH_LIBS=OFF -DMZ_LIBCOMP=OFF -DMZ_PKCRYPT=OFF -DMZ_WZAES=OFF -DMZ_OPENSSL=OFF \
    -DMZ_SIGNING=OFF -DMZ_PPMD=OFF -DMZ_LZMA=OFF -DMZ_ZSTD=OFF -DMZ_BZIP2=OFF -DMZ_ICONV=OFF \
    -DMZ_ZLIB=ON -DMZ_ZLIB_FLAVOR=zlib -DMZ_COMPAT=OFF \
    -DCMAKE_INSTALL_INCLUDEDIR="$PREFIX/include/minizip-ng"
}

build_opencolorio() {
  fetch_git opencolorio https://github.com/AcademySoftwareFoundation/OpenColorIO v2.5.0
  cmake_build opencolorio "$SRC/opencolorio" \
    -DOCIO_BUILD_APPS=OFF -DOCIO_BUILD_PYTHON=OFF -DOCIO_BUILD_NUKE=OFF -DOCIO_BUILD_JAVA=OFF \
    -DOCIO_BUILD_DOCS=OFF -DOCIO_BUILD_TESTS=OFF -DOCIO_BUILD_GPU_TESTS=OFF -DOCIO_BUILD_OPENFX=OFF \
    -DOCIO_USE_SIMD=OFF -DOCIO_USE_SSE2NEON=OFF -DOCIO_INSTALL_EXT_PACKAGES=NONE \
    -Dminizip-ng_INCLUDE_DIR="$PREFIX/include/minizip-ng/minizip" \
    -Dminizip-ng_LIBRARY="$PREFIX/lib/libminizip.a"
}

build_openimageio() {
  fetch_git openimageio https://github.com/AcademySoftwareFoundation/OpenImageIO v3.1.16.0
  # ustring makes its std::string point to the string table using knowledge of libc++ internals,
  # which don't match the libc++ of Emscripten: strings longer than 10 characters became empty
  # (breaking e.g. PNG writing). Use plain std::string copies.
  sed -i 's/^#elif defined(_LIBCPP_VERSION) && !defined(__aarch64__)$/& \&\& !defined(__EMSCRIPTEN__)/' \
    "$SRC/openimageio/src/libutil/ustring.cpp"
  cmake_build openimageio "$SRC/openimageio" -DZLIB_LIBRARY="$PREFIX/lib/libz.a" \
    -DOIIO_BUILD_TOOLS=OFF -DOIIO_BUILD_TESTS=OFF -DOIIO_BUILD_DOCS=OFF -DINSTALL_DOCS=OFF \
    -DOpenImageIO_BUILD_MISSING_DEPS="" -DSTOP_ON_WARNING=OFF -DUSE_SIMD=0 -DEMBEDPLUGINS=ON \
    -DUSE_PYTHON=OFF -DUSE_QT=OFF -DUSE_NUKE=OFF -DUSE_OPENVDB=OFF -DUSE_FREETYPE=OFF -DUSE_DCMTK=OFF \
    -DUSE_TBB=OFF -DUSE_GIF=OFF -DUSE_OPENCV=OFF -DUSE_OPENJPEG=OFF -DUSE_FFMPEG=OFF -DUSE_PTEX=OFF \
    -DUSE_LIBHEIF=OFF -DUSE_LIBRAW=OFF -DUSE_JXL=OFF -DUSE_WEBP=OFF -DUSE_OPENCOLORIO=ON \
    -DUSE_LIBUHDR=OFF -DUSE_R3DSDK=OFF -DUSE_EXTERNAL_PUGIXML=OFF -DUSE_BZIP2=OFF \
    -DOpenImageIO_REQUIRED_DEPS="libjpeg-turbo;TIFF;OpenEXR;PNG;fmt;Robinmap;ZLIB"
}

build_python() {
  fetch_git python https://github.com/python/cpython v3.13.13
  local src="$SRC/python"
  # Cross-compiling needs a native Python of the same version.
  local native="$BUILD/python-native"
  if [ ! -x "$native/python" ]; then
    mkdir -p "$native"
    (cd "$native" && "$src/configure" --without-ensurepip -q && make -j "$JOBS" -s)
  fi
  local host="$BUILD/python"
  mkdir -p "$host"
  (
    cd "$host"
    # PY_CALL_TRAMPOLINE: WebAssembly traps when a function is called through a pointer of
    # another type. Many C extensions (Blender's too) define e.g. METH_NOARGS functions with one
    # argument, CPython then calls them through a JavaScript trampoline which tolerates this.
    # The stdlib extension modules are linked statically into libpython,
    # there is no dynamic loading in the Blender WebAssembly build.
    # bz2 and sqlite3 would use Emscripten ports, downloaded from GitHub archives at build time.
    CONFIG_SITE="$src/Tools/wasm/config.site-wasm32-emscripten" \
      py_cv_module__bz2=n/a py_cv_module__sqlite3=n/a \
      emconfigure "$src/configure" \
        --host=wasm32-unknown-emscripten --build="$("$src/config.guess")" \
        --with-emscripten-target=browser --enable-wasm-pthreads --disable-wasm-dynamic-linking \
        --with-build-python="$native/python" --without-ensurepip --disable-ipv6 --disable-test-modules \
        --disable-shared --prefix="$PREFIX" \
        CFLAGS="$WEB_FLAGS -DPY_CALL_TRAMPOLINE" LDFLAGS="$WEB_FLAGS" \
        ZLIB_CFLAGS="-I$PREFIX/include" ZLIB_LIBS="-L$PREFIX/lib -lz" \
        --with-system-expat LIBEXPAT_CFLAGS="-I$PREFIX/include" LIBEXPAT_LDFLAGS="-L$PREFIX/lib -lexpat"
    emmake make -j "$JOBS"
    emmake make install
    # Some modules link helper libraries that are not part of libpython, add them so
    # embedding Python only needs libpython (and expat, which Blender links already:
    # CPython's bundled copy would clash with it).
    local objs
    objs="$(mktemp -d)"
    for lib in Modules/_decimal/libmpdec/libmpdec.a Modules/_hacl/libHacl_Hash_SHA2.a; do
      (cd "$objs" && emar x "$host/$lib")
    done
    # Append (q), never replace (r): libmpdec has a `context.o` like CPython itself.
    emar qs "$PREFIX/lib/libpython3.13.a" "$objs"/*.o
    rm -rf "$objs"
  )
}

# Dawn (for Tint) at the version of Emscripten's emdawnwebgpu port. Its dependencies are fetched
# from GitHub at the commits in Dawn's DEPS file (abseil: an upstream release, Dawn uses
# Chromium's mirror).
fetch_dawn() {
  fetch_git dawn https://github.com/google/dawn v20260423.175430
  local third_party="$SRC/dawn/third_party"
  fetch_commit() {
    [ -f "$1/.fetched" ] && return 0
    mkdir -p "$1"
    (cd "$1" && git init -q && git fetch -q --depth 1 "$2" "$3" && \
      git -c advice.detachedHead=false checkout -q FETCH_HEAD && touch .fetched)
  }
  fetch_commit "$third_party/glslang/src" https://github.com/KhronosGroup/glslang \
    aa8e19e05f8b30d14b894fb2d94875bb6c8be3c6
  fetch_commit "$third_party/spirv-headers/src" https://github.com/KhronosGroup/SPIRV-Headers \
    ad9184e76a66b1001c29db9b0a3e87f646c64de0
  fetch_commit "$third_party/spirv-tools/src" https://github.com/KhronosGroup/SPIRV-Tools \
    ff5c50339cc1e9f34f04cb440a3e5fe89db0161d
  # Dawn's code generator imports these; an empty third_party/jinja2 shadows any
  # system copy as a namespace package, so fetch them at the DEPS commits.
  fetch_commit "$third_party/jinja2" https://chromium.googlesource.com/chromium/src/third_party/jinja2 \
    c3027d884967773057bf74b957e3fea87e5df4d7
  fetch_commit "$third_party/markupsafe" https://chromium.googlesource.com/chromium/src/third_party/markupsafe \
    4256084ae14175d38a3ff7d739dca83ae49ccec6
  # Not a Dawn dependency: GLSL ES output for WebGL2 (the OpenGL backend).
  fetch_commit "$third_party/spirv-cross/src" https://github.com/KhronosGroup/SPIRV-Cross \
    vulkan-sdk-1.4.363.0
  if [ ! -f "$third_party/abseil-cpp/CMakeLists.txt" ]; then
    rm -rf "$third_party/abseil-cpp"
    git -c advice.detachedHead=false clone -q --depth 1 --branch 20260107.0 \
      https://github.com/abseil/abseil-cpp "$third_party/abseil-cpp"
  fi
  python3 "$ROOT/web/shader_compiler/patch_tint.py" "$SRC/dawn"
}

# GLSL → WGSL compiler (glslang + Tint) used by the WebGPU backend, see web/shader_compiler/.
# All its static libraries are merged into libweb_shader_compiler.a.
build_shader_compiler() {
  fetch_dawn
  cmake_build shader_compiler "$ROOT/web/shader_compiler" -DDAWN_DIR="$SRC/dawn" \
    -DCMAKE_EXE_LINKER_FLAGS="$WEB_FLAGS -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=4MB"
  cmake --build "$BUILD/shader_compiler" -j "$JOBS" --target wsc_test
  local script="$BUILD/shader_compiler/merge.mri"
  {
    echo "create $PREFIX/lib/libweb_shader_compiler.a"
    find "$BUILD/shader_compiler" -name '*.a' | sort | sed 's/^/addlib /'
    echo "save"
    echo "end"
  } > "$script"
  rm -f "$PREFIX/lib/libweb_shader_compiler.a"
  emar -M < "$script"
  cp "$ROOT/web/shader_compiler/web_shader_compiler.h" "$PREFIX/include/"
}

ALL_PACKAGES=(zlib zstd png jpeg brotli freetype fmt eigen imath deflate openjph openexr tbb tiff robinmap
              expat yamlcpp pystring minizipng opencolorio openimageio python shader_compiler)

main() {
  local packages=("$@")
  [ ${#packages[@]} -eq 0 ] && packages=("${ALL_PACKAGES[@]}")
  local pkg stamp
  for pkg in "${packages[@]}"; do
    stamp="$PREFIX/.built-$pkg"
    if [ -f "$stamp" ] && [ $# -eq 0 ]; then
      echo "== $pkg: already built"
      continue
    fi
    echo "== $pkg: building"
    "build_$pkg"
    touch "$stamp"
  done
  echo "Dependencies installed in $PREFIX"
}

# Parsed in full before running, so editing this file during a build is safe.
main "$@"
exit

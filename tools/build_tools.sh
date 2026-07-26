#!/usr/bin/env bash
# Builds XenonAnalyse, XenonRecomp, and XenosRecomp from the submodules in
# tools/. Requires CMake 3.20+ and Clang 18+ (see docs/01-getting-started.md).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Core count: nproc on Linux, NUMBER_OF_PROCESSORS on Windows/Git Bash
# (which has no nproc), sysctl on macOS.
detect_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif [ -n "${NUMBER_OF_PROCESSORS:-}" ]; then
        echo "$NUMBER_OF_PROCESSORS"
    elif command -v sysctl >/dev/null 2>&1 && sysctl -n hw.ncpu >/dev/null 2>&1; then
        sysctl -n hw.ncpu
    else
        echo 4
    fi
}
JOBS="${JOBS:-$(detect_jobs)}"

case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;;
    *)                    IS_WINDOWS=0 ;;
esac

# Compiler selection:
#   - explicit CC/CXX always wins
#   - on Windows prefer clang-cl (the MSVC-style driver). XenonRecomp's
#     CMakeLists sets CMAKE_MSVC_RUNTIME_LIBRARY and, under CMP0141,
#     CMAKE_MSVC_DEBUG_INFORMATION_FORMAT. Those are MSVC-ABI abstractions
#     that the GNU-style clang.exe driver does not implement, so configuring
#     with plain clang.exe fails with:
#       MSVC_DEBUG_INFORMATION_FORMAT value 'ProgramDatabase' not known
#     clang-cl is the same compiler with an MSVC-compatible driver, and is
#     what this codebase expects on Windows.
#   - clang-18/clang++-18 is the Debian/Ubuntu naming convention
#   - plain clang/clang++ otherwise (macOS, other distros)
if [ -n "${CC:-}" ] || [ -n "${CXX:-}" ]; then
    CC_BIN="${CC:-${CXX:-clang}}"
    CXX_BIN="${CXX:-${CC:-clang++}}"
elif [ "$IS_WINDOWS" -eq 1 ] && command -v clang-cl >/dev/null 2>&1; then
    # clang-cl drives both C and C++; CMake distinguishes by source language.
    CC_BIN="clang-cl"
    CXX_BIN="clang-cl"
elif command -v clang-18 >/dev/null 2>&1; then
    CC_BIN="clang-18"
    CXX_BIN="clang++-18"
else
    CC_BIN="clang"
    CXX_BIN="clang++"
fi

if ! command -v "$CXX_BIN" >/dev/null 2>&1; then
    echo "error: no Clang found on PATH (tried clang++-18 and clang++)." >&2
    echo "       XenonRecomp requires Clang 18+. Install it, or set CC/CXX." >&2
    exit 1
fi

# Verify the version, rather than silently building with whatever was found.
# An old Clang produces confusing failures much later, so fail loudly here.
CLANG_MAJOR="$("$CXX_BIN" --version 2>/dev/null \
    | grep -oiE 'clang version [0-9]+' | grep -oE '[0-9]+' | head -1 || true)"

if [ -z "$CLANG_MAJOR" ]; then
    echo "warning: could not parse a version from '$CXX_BIN --version'." >&2
    echo "         Proceeding, but XenonRecomp requires Clang 18+." >&2
elif [ "$CLANG_MAJOR" -lt 18 ]; then
    echo "error: '$CXX_BIN' is Clang $CLANG_MAJOR, but Clang 18+ is required." >&2
    echo "       XenonRecomp relies on Clang-specific behaviour; older versions" >&2
    echo "       fail in confusing ways. Install Clang 18+, or set CC/CXX to it." >&2
    exit 1
fi

echo "==> Using $CXX_BIN (Clang ${CLANG_MAJOR:-unknown}), $JOBS parallel jobs"

# When compiling with clang-cl, CMake emits the MSVC lib.exe-style archive
# rule:
#     <CMAKE_AR> /nologo /out:<TARGET> <OBJECTS>
# but its CMAKE_AR autodetection can land on llvm-ar.exe, the GNU-style
# archiver, which only understands '-'-prefixed flags and dies with:
#     llvm-ar.exe: error: unknown option /
# llvm-lib.exe is the lib.exe-compatible archiver and ships in the same
# LLVM bin directory, so pin CMAKE_AR to it explicitly.
TOOLCHAIN_ARGS=()
WANT_AR=""
if [ "$IS_WINDOWS" -eq 1 ] && [ "$(basename "$CC_BIN")" = "clang-cl" ]; then
    _clang_cl="$(command -v clang-cl 2>/dev/null || true)"
    if [ -n "$_clang_cl" ]; then
        _llvm_bin="$(dirname "$_clang_cl")"
        for _ar in "$_llvm_bin/llvm-lib.exe" "$_llvm_bin/llvm-lib"; do
            [ -x "$_ar" ] || continue
            # CMake wants a native path; cygpath -m yields C:/... form.
            if command -v cygpath >/dev/null 2>&1; then
                _ar="$(cygpath -m "$_ar")"
            fi
            WANT_AR="$_ar"
            TOOLCHAIN_ARGS+=(-DCMAKE_AR="$_ar")
            echo "==> Archiver: $_ar"
            break
        done
        if [ "${#TOOLCHAIN_ARGS[@]}" -eq 0 ]; then
            echo "warning: clang-cl in use but llvm-lib not found next to it." >&2
            echo "         If archiving fails with \"unknown option /\", pass" >&2
            echo "         CMAKE_ARGS=\"-DCMAKE_AR=<path to llvm-lib.exe or lib.exe>\"." >&2
        fi
    fi
fi

# --------------------------------------------------------------------------
# Apply our patches to the vendored submodules.
#
# The submodules point at upstream repositories we cannot push to, so any
# change we make inside them exists only in the local checkout and would be
# lost on a fresh clone. Patches under patches/<Submodule>/*.patch are the
# version-controlled source of truth; this applies them on every build.
#
# Idempotent: a patch that is already applied reverses cleanly, which is how
# we detect it and skip. A patch that neither applies nor reverses means the
# submodule has moved under us — that is a hard error, not something to
# paper over, because silently building without our instruction
# implementations would produce subtly wrong game code.
# --------------------------------------------------------------------------
apply_patches() {
    local name="$1"
    local repo="$SCRIPT_DIR/$name"
    local dir="$REPO_ROOT/patches/$name"

    [ -d "$dir" ] || return 0

    local patch
    for patch in "$dir"/*.patch; do
        [ -e "$patch" ] || continue
        local label="${patch##*/}"

        if git -C "$repo" apply --reverse --check "$patch" >/dev/null 2>&1; then
            echo "==> [$name] already patched: $label"
        elif git -C "$repo" apply --check "$patch" >/dev/null 2>&1; then
            git -C "$repo" apply "$patch"
            echo "==> [$name] applied: $label"
        else
            echo "error: patch does not apply and is not already applied:" >&2
            echo "       $patch" >&2
            echo "       The submodule has probably moved. Refresh the patch against" >&2
            echo "       the current submodule commit before building." >&2
            exit 1
        fi
    done
}

# Detect edits made directly inside a submodule on top of our patches.
#
# apply_patches only proves our patches are applied; it cannot tell whether
# someone also hand-edited the submodule afterwards. Such an edit is invisible
# to git in this repo and is lost on a fresh clone — exactly the failure the
# patches/ mechanism exists to prevent. Our patch files are generated with
# `git diff HEAD`, so if the working tree is precisely HEAD+patches then the
# submodule's current `git diff HEAD` reproduces them byte for byte.
check_patch_drift() {
    local name="$1"
    local repo="$SCRIPT_DIR/$name"
    local dir="$REPO_ROOT/patches/$name"

    [ -d "$dir" ] || return 0
    compgen -G "$dir"/*.patch >/dev/null || return 0

    # Compare with CR stripped. `git diff` emits LF, but a .patch checked out
    # on Windows may be CRLF, which would otherwise read as drift that isn't
    # there. .gitattributes marks *.patch as -text to prevent that at source;
    # this handles trees checked out before that existed.
    local expected actual
    expected="$(cat "$dir"/*.patch | tr -d '\r')"
    actual="$(git -C "$repo" diff HEAD 2>/dev/null | tr -d '\r' || true)"

    if [ "$expected" != "$actual" ]; then
        echo >&2
        echo "warning: [$name] working tree differs from patches/$name/." >&2
        echo "         Something was edited inside the submodule beyond our patches." >&2
        echo "         Those edits are NOT tracked by this repo and will be lost on a" >&2
        echo "         fresh clone. Fold them into the patch:" >&2
        echo "           git -C tools/$name diff HEAD > patches/$name/0001-*.patch" >&2
        echo >&2
    fi
}

REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
apply_patches "XenonRecomp"
apply_patches "XenosRecomp"
check_patch_drift "XenonRecomp"
check_patch_drift "XenosRecomp"

# Choose the generator once, up front. This previously attempted Ninja with
# stderr suppressed and retried with the default generator on failure —
# which hid the real CMake error and made every configure appear to run
# twice. Checking for ninja explicitly keeps the output honest.
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
else
    GENERATOR_ARGS=()
    echo "note: ninja not found; using CMake's default generator." >&2
fi

# Extra flags for every configure, e.g.
#   CMAKE_ARGS="-DCMAKE_POLICY_DEFAULT_CMP0141=OLD" ./tools/build_tools.sh
read -ra CMAKE_EXTRA_ARGS <<< "${CMAKE_ARGS:-}"

# Strip directories and any .exe suffix, so "clang-cl" compares equal to
# "C:/.../clang-cl.exe".
tool_basename() {
    local b
    b="$(basename "${1:-}")"
    printf '%s' "${b%.[eE][xX][eE]}"
}

# Read a cache entry, e.g. cache_value <cache> CMAKE_AR
cache_value() {
    sed -n "s/^$2:[^=]*=//p" "$1" 2>/dev/null | head -1
}

build_one() {
    local name="$1"
    local src_dir="$SCRIPT_DIR/$name"
    local build_dir="$src_dir/build"
    local cache="$build_dir/CMakeCache.txt"

    if [ ! -f "$src_dir/CMakeLists.txt" ]; then
        echo "error: $src_dir is missing/empty — did you run 'git submodule update --init --recursive'?" >&2
        exit 1
    fi

    # A CMake build tree pins its toolchain at the FIRST configure: the
    # generated CMakeFiles/<ver>/CMakeCCompiler.cmake does a plain
    # set(CMAKE_AR ...) which is re-included on every reconfigure and
    # shadows anything passed with -D. So changing compiler or archiver
    # requires wiping the tree — reconfiguring silently keeps the old tool
    # while *appearing* to accept the new flag.
    if [ "${CLEAN:-0}" != "0" ] && [ -d "$build_dir" ]; then
        echo "==> CLEAN set; wiping $name/build"
        rm -rf "$build_dir"
    elif [ -f "$cache" ]; then
        local cached_cc cached_ar wipe=0
        cached_cc="$(cache_value "$cache" CMAKE_C_COMPILER)"
        cached_ar="$(cache_value "$cache" CMAKE_AR)"

        if [ -n "$cached_cc" ] && \
           [ "$(tool_basename "$cached_cc")" != "$(tool_basename "$CC_BIN")" ]; then
            echo "==> Compiler changed ($(tool_basename "$cached_cc") -> $(tool_basename "$CC_BIN"))"
            wipe=1
        fi
        if [ -n "$WANT_AR" ] && [ -n "$cached_ar" ] && \
           [ "$(tool_basename "$cached_ar")" != "$(tool_basename "$WANT_AR")" ]; then
            echo "==> Archiver changed ($(tool_basename "$cached_ar") -> $(tool_basename "$WANT_AR"))"
            wipe=1
        fi
        if [ "$wipe" -eq 1 ]; then
            echo "==> Wiping stale $name/build so the new toolchain takes effect"
            rm -rf "$build_dir"
        fi
    fi

    echo "==> Configuring $name"
    cmake -S "$src_dir" -B "$build_dir" \
        ${GENERATOR_ARGS[@]+"${GENERATOR_ARGS[@]}"} \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER="$CC_BIN" \
        -DCMAKE_CXX_COMPILER="$CXX_BIN" \
        ${TOOLCHAIN_ARGS[@]+"${TOOLCHAIN_ARGS[@]}"} \
        ${CMAKE_EXTRA_ARGS[@]+"${CMAKE_EXTRA_ARGS[@]}"}

    echo "==> Building $name"
    cmake --build "$build_dir" --config RelWithDebInfo -j "$JOBS"
}

build_one "XenonRecomp"
build_one "XenosRecomp"
build_one "xex_info"
build_one "extract-xiso"

cat <<EOF

Build complete.

  XenonAnalyse  : tools/XenonRecomp/build/XenonAnalyse/XenonAnalyse
  XenonRecomp   : tools/XenonRecomp/build/XenonRecomp/XenonRecomp
  XenosRecomp   : tools/XenosRecomp/build/XenosRecomp/XenosRecomp
  xex_info      : tools/xex_info/build/xex_info
  extract-xiso  : tools/extract-xiso/build/extract-xiso

Note: XenosRecomp (shader recompiler) isn't needed until Phase 4
(docs/04-roadmap.md) — XenonAnalyse/XenonRecomp (the CPU code path) are what
you need first. It builds fine cross-platform via the bundled dxc-bin
submodule (verified on Linux), but if it ever fails to build for you, that's
not a blocker for early phases.

xex_info and extract-xiso are our own additions (not upstream XenonRecomp) —
see tools/import_dump.sh, which uses both to ingest a raw disc dump into
private/.

Next: see docs/01-getting-started.md step 4 onward.
EOF

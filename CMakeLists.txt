cmake_minimum_required(VERSION 3.20)
project("WoSRecomp-ALL")

# --------------------------------------------------------------------------
# This top-level CMake file currently only wires in the recompiler tools
# (XenonRecomp/XenosRecomp) as subdirectories so they can be built via a
# normal CMake configure/build from the repo root, in addition to using
# tools/build_tools.sh directly.
#
# WoSRecompLib (generated PPC code) and WoSRecomp (the runtime app) are not
# yet buildable targets — see docs/04-roadmap.md. They will be added here
# once WoSRecompLib/ppc/ contains real generated output and WoSRecomp/ has
# enough runtime code to link against it.
# --------------------------------------------------------------------------

option(WOSRECOMP_BUILD_TOOLS "Build XenonRecomp/XenosRecomp toolchain" ON)

if(WOSRECOMP_BUILD_TOOLS)
    add_subdirectory(tools/XenonRecomp)

    # XenosRecomp depends on DirectXShaderCompiler and is primarily
    # developed/tested on Windows; keep it optional so a Linux/macOS
    # configure of the CPU-code toolchain doesn't hard-fail on it.
    option(WOSRECOMP_BUILD_XENOS "Build XenosRecomp (shader recompiler)" OFF)
    if(WOSRECOMP_BUILD_XENOS)
        add_subdirectory(tools/XenosRecomp)
    endif()
endif()

# Future:
# add_subdirectory(WoSRecompLib)
# add_subdirectory(WoSRecomp)

# The host harness — see main.cpp.
#
# Standalone project, mirroring tools/xex_info: it pulls in XenonUtils (for XEX
# parsing, so the harness loads the image with exactly the same code the
# recompiler used) and WoSRecompLib (the recompiled game code).
#
# Expect this to FAIL TO LINK at first. That is the point: every undefined
# symbol is a kernel/OS import Web of Shadows calls that we have not
# implemented yet, and that list is the runtime to-do list.

cmake_minimum_required(VERSION 3.20)
project("WoSRecomp" CXX C)

set(CMAKE_CXX_STANDARD 20)
set(BUILD_SHARED_LIBS OFF)

set(XENONRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../tools/XenonRecomp")
set(THIRDPARTY_ROOT "${XENONRECOMP_ROOT}/thirdparty")

add_subdirectory("${THIRDPARTY_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/thirdparty")
add_subdirectory("${XENONRECOMP_ROOT}/XenonUtils" "${CMAKE_CURRENT_BINARY_DIR}/XenonUtils")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../WoSRecompLib" "${CMAKE_CURRENT_BINARY_DIR}/WoSRecompLib")

if(NOT TARGET WoSRecompLib)
    message(FATAL_ERROR
        "WoSRecompLib produced no target — the recompiled sources in "
        "WoSRecompLib/ppc/ are missing. Run XenonRecomp first.")
endif()

add_executable(WoSRecomp main.cpp)

target_compile_definitions(WoSRecomp PRIVATE NOMINMAX)
target_link_libraries(WoSRecomp PRIVATE WoSRecompLib XenonUtils)

if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(WoSRecomp PRIVATE /clang:-march=sandybridge)
else()
    target_compile_options(WoSRecomp PRIVATE -march=sandybridge)
endif()

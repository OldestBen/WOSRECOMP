# Standalone project: reuses XenonUtils' sources directly (rather than
# sharing a CMake target with tools/XenonRecomp/) so it can be built
# independently, the same way tools/build_tools.sh builds XenonRecomp and
# XenosRecomp as separate configures.
cmake_minimum_required(VERSION 3.20)
project("xex_info")

set(CMAKE_CXX_STANDARD 17)
set(BUILD_SHARED_LIBS OFF)

set(XENONRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../XenonRecomp")
set(THIRDPARTY_ROOT "${XENONRECOMP_ROOT}/thirdparty")

add_subdirectory("${THIRDPARTY_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/thirdparty")
add_subdirectory("${XENONRECOMP_ROOT}/XenonUtils" "${CMAKE_CURRENT_BINARY_DIR}/XenonUtils")

add_executable(xex_info main.cpp)
target_compile_definitions(xex_info PRIVATE NOMINMAX)
target_link_libraries(xex_info PRIVATE XenonUtils)

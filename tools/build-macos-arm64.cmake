# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+

cmake_minimum_required(VERSION 3.22)

if(NOT CMAKE_HOST_APPLE)
	message(FATAL_ERROR "Run this command on an Apple Silicon Mac.")
endif()

execute_process(COMMAND /usr/bin/uname -m
	OUTPUT_VARIABLE host_arch OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT host_arch STREQUAL "arm64")
	message(FATAL_ERROR "Run this command from a native arm64 terminal.")
endif()

get_filename_component(source_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(deps_dir "${source_dir}/deps-arm64")
set(build_dir "${source_dir}/build-macos-arm64")
set(app_dir "${source_dir}/bin/PCSX2.app")
if(NOT EXISTS "${deps_dir}/lib/cmake/Qt6/Qt6Config.cmake" OR
	NOT EXISTS "${deps_dir}/lib/libavcodec.dylib")
	message(FATAL_ERROR "Build the fork's macOS dependencies first. See docs/building_macos_arm64.md.")
endif()

execute_process(COMMAND /usr/bin/xcrun metal -v
	RESULT_VARIABLE metal_result OUTPUT_QUIET ERROR_QUIET)
if(NOT metal_result STREQUAL "0")
	message(FATAL_ERROR "The selected Xcode needs the Metal Toolchain. See docs/building_macos_arm64.md.")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -S "${source_dir}" -B "${build_dir}" -G "Unix Makefiles"
	"-DCMAKE_PREFIX_PATH=${deps_dir}"
	"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${source_dir}/bin"
	-DCMAKE_BUILD_TYPE=Release
	-DCMAKE_OSX_ARCHITECTURES=arm64
	-DDISABLE_ADVANCE_SIMD=ON
	-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
	-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON
	-DUSE_LINKED_FFMPEG=ON
	COMMAND_ERROR_IS_FATAL ANY)

include(ProcessorCount)
ProcessorCount(jobs)
if(jobs EQUAL 0)
	set(jobs 1)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build_dir}"
	--target pcsx2-postprocess-bundle --parallel "${jobs}"
	COMMAND_ERROR_IS_FATAL ANY)

execute_process(COMMAND /usr/bin/lipo -archs "${app_dir}/Contents/MacOS/PCSX2"
	OUTPUT_VARIABLE app_arch OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT app_arch STREQUAL "arm64")
	message(FATAL_ERROR "Expected an arm64 PCSX2 executable, got '${app_arch}'.")
endif()
message(STATUS "Built ${app_dir}")

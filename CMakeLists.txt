#[[
Licensed to the Apache Software Foundation (ASF) under one
or more contributor license agreements.  See the NOTICE file
distributed with this work for additional information
regarding copyright ownership.  The ASF licenses this file
to you under the Apache License, Version 2.0 (the
"License"); you may not use this file except in compliance
with the License.  You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the License is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied.  See the License for the
specific language governing permissions and limitations
under the License.
]]

cmake_minimum_required(VERSION 3.11)

set(TARGET_NAME tsfile)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TSFILE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tsfile"
    CACHE PATH "Path to the Apache TsFile source tree")
set(TSFILE_CPP_SOURCE_DIR "${TSFILE_SOURCE_DIR}/cpp")
if(NOT EXISTS "${TSFILE_CPP_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "Apache TsFile sources were not found under "
                      "${TSFILE_SOURCE_DIR}. Initialize Git submodules first.")
endif()

include(ExternalProject)

# TsFile currently expects to be the top-level CMake project. Build it in an
# isolated CMake invocation, then expose its static archives as imported
# targets. This keeps TsFile's global flags and generic options out of DuckDB.
set(TSFILE_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/tsfile_dependency")
set(TSFILE_CMAKE_ARGS
    -DTSFILE_BUILD_SHARED:BOOL=OFF
    -DBUILD_TEST:BOOL=OFF
    -DBUILD_TOOLS:BOOL=OFF
    -DENABLE_ANTLR4:BOOL=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON)

if(MSVC)
  # TsFile declares CMake 3.11, so CMP0091 otherwise keeps its OLD behavior
  # and ignores CMAKE_MSVC_RUNTIME_LIBRARY. Opt in before TsFile's project()
  # call so its static archives use the same CRT as DuckDB.
  list(APPEND TSFILE_CMAKE_ARGS
       -DCMAKE_POLICY_DEFAULT_CMP0091:STRING=NEW)
endif()

if(NOT MSVC)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND TSFILE_CMAKE_ARGS -DTSFILE_OPTIMIZATION_FLAGS:STRING=-O0)
  else()
    # Avoid coupling the dependency archive to compiler-specific LTO tooling.
    list(APPEND TSFILE_CMAKE_ARGS -DTSFILE_OPTIMIZATION_FLAGS:STRING=-O2)
  endif()
endif()

foreach(TSFILE_TOOLCHAIN_VAR
        CMAKE_BUILD_TYPE
        CMAKE_C_COMPILER
        CMAKE_CXX_COMPILER
        CMAKE_C_COMPILER_LAUNCHER
        CMAKE_CXX_COMPILER_LAUNCHER
        CMAKE_TOOLCHAIN_FILE
        CMAKE_OSX_ARCHITECTURES
        CMAKE_OSX_DEPLOYMENT_TARGET
        CMAKE_SYSROOT
        CMAKE_MSVC_RUNTIME_LIBRARY)
  if(DEFINED ${TSFILE_TOOLCHAIN_VAR} AND
     NOT "${${TSFILE_TOOLCHAIN_VAR}}" STREQUAL "")
    list(APPEND TSFILE_CMAKE_ARGS
         "-D${TSFILE_TOOLCHAIN_VAR}:STRING=${${TSFILE_TOOLCHAIN_VAR}}")
  endif()
endforeach()

set(TSFILE_GENERATOR_ARGS CMAKE_GENERATOR "${CMAKE_GENERATOR}")
if(CMAKE_GENERATOR_PLATFORM)
  list(APPEND TSFILE_GENERATOR_ARGS
       CMAKE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
endif()
if(CMAKE_GENERATOR_TOOLSET)
  list(APPEND TSFILE_GENERATOR_ARGS
       CMAKE_GENERATOR_TOOLSET "${CMAKE_GENERATOR_TOOLSET}")
endif()

function(tsfile_archive_path OUTPUT_VAR ARCHIVE_NAME CONFIG_NAME)
  set(ARCHIVE_OUTPUT_NAME "${ARCHIVE_NAME}")
  string(TOUPPER "${CONFIG_NAME}" CONFIG_NAME_UPPER)
  if(WIN32 AND ARCHIVE_NAME STREQUAL "z")
    # Bundled zlib uses the target name on Windows/MinGW, while Unix builds
    # rename the archive to libz. Debug builds also inherit zlib's `d` postfix.
    set(ARCHIVE_OUTPUT_NAME "zlibstatic")
    if(CONFIG_NAME_UPPER STREQUAL "DEBUG")
      string(APPEND ARCHIVE_OUTPUT_NAME "d")
    endif()
  endif()

  if(CMAKE_CONFIGURATION_TYPES)
    set(ARCHIVE_DIR "${TSFILE_BINARY_DIR}/lib/${CONFIG_NAME}")
  else()
    set(ARCHIVE_DIR "${TSFILE_BINARY_DIR}/lib")
  endif()
  set(${OUTPUT_VAR}
      "${ARCHIVE_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}${ARCHIVE_OUTPUT_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX}"
      PARENT_SCOPE)
endfunction()

set(TSFILE_ARCHIVE_NAMES tsfile snappy LZ4 lzokay z)
set(TSFILE_BUILD_BYPRODUCTS "")
if(CMAKE_CONFIGURATION_TYPES)
  foreach(TSFILE_CONFIG ${CMAKE_CONFIGURATION_TYPES})
    foreach(TSFILE_ARCHIVE_NAME ${TSFILE_ARCHIVE_NAMES})
      tsfile_archive_path(TSFILE_ARCHIVE_PATH "${TSFILE_ARCHIVE_NAME}"
                          "${TSFILE_CONFIG}")
      list(APPEND TSFILE_BUILD_BYPRODUCTS "${TSFILE_ARCHIVE_PATH}")
    endforeach()
  endforeach()
else()
  foreach(TSFILE_ARCHIVE_NAME ${TSFILE_ARCHIVE_NAMES})
    tsfile_archive_path(TSFILE_ARCHIVE_PATH "${TSFILE_ARCHIVE_NAME}"
                        "${CMAKE_BUILD_TYPE}")
    list(APPEND TSFILE_BUILD_BYPRODUCTS "${TSFILE_ARCHIVE_PATH}")
  endforeach()
endif()

ExternalProject_Add(
  tsfile_external
  SOURCE_DIR "${TSFILE_CPP_SOURCE_DIR}"
  BINARY_DIR "${TSFILE_BINARY_DIR}"
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND ""
  PATCH_COMMAND ""
  ${TSFILE_GENERATOR_ARGS}
  CMAKE_ARGS ${TSFILE_CMAKE_ARGS}
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG>
                --target tsfile
  BUILD_BYPRODUCTS ${TSFILE_BUILD_BYPRODUCTS}
  INSTALL_COMMAND ""
  TEST_COMMAND "")

function(import_tsfile_archive TARGET_NAME ARCHIVE_NAME)
  add_library(${TARGET_NAME} STATIC IMPORTED GLOBAL)
  add_dependencies(${TARGET_NAME} tsfile_external)
  if(CMAKE_CONFIGURATION_TYPES)
    set_property(TARGET ${TARGET_NAME} PROPERTY IMPORTED_CONFIGURATIONS
                                                ${CMAKE_CONFIGURATION_TYPES})
    foreach(TSFILE_CONFIG ${CMAKE_CONFIGURATION_TYPES})
      string(TOUPPER "${TSFILE_CONFIG}" TSFILE_CONFIG_UPPER)
      tsfile_archive_path(TSFILE_ARCHIVE_PATH "${ARCHIVE_NAME}"
                          "${TSFILE_CONFIG}")
      set_target_properties(
        ${TARGET_NAME}
        PROPERTIES "IMPORTED_LOCATION_${TSFILE_CONFIG_UPPER}"
                   "${TSFILE_ARCHIVE_PATH}")
    endforeach()
  else()
    tsfile_archive_path(TSFILE_ARCHIVE_PATH "${ARCHIVE_NAME}"
                        "${CMAKE_BUILD_TYPE}")
    set_target_properties(${TARGET_NAME}
                          PROPERTIES IMPORTED_LOCATION
                                     "${TSFILE_ARCHIVE_PATH}")
  endif()
endfunction()

import_tsfile_archive(tsfile_sdk tsfile)
import_tsfile_archive(tsfile_snappy snappy)
import_tsfile_archive(tsfile_lz4 LZ4)
import_tsfile_archive(tsfile_lzokay lzokay)
import_tsfile_archive(tsfile_zlib z)
find_package(Threads REQUIRED)
target_compile_definitions(tsfile_sdk INTERFACE TSFILE_STATIC)
if(MSVC)
  # MSVC reports the legacy C++98 value for __cplusplus unless this option is
  # enabled. TsFile's public headers use __cplusplus to select portable C++11
  # implementations, so propagate the same option used by TsFile's own build.
  target_compile_options(tsfile_sdk INTERFACE /Zc:__cplusplus)
endif()
target_include_directories(tsfile_sdk INTERFACE "${TSFILE_CPP_SOURCE_DIR}/src")
target_link_libraries(tsfile_sdk INTERFACE tsfile_snappy tsfile_lz4
                                             tsfile_lzokay tsfile_zlib
                                             Threads::Threads)

include_directories(src/include "${TSFILE_CPP_SOURCE_DIR}/src")

set(EXTENSION_SOURCES
    src/tsfile_extension.cpp
    src/tsfile_copy.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} "-warnings" ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} tsfile_sdk)
target_link_libraries(${LOADABLE_EXTENSION_NAME} tsfile_sdk)
add_dependencies(${EXTENSION_NAME} tsfile_external)
add_dependencies(${LOADABLE_EXTENSION_NAME} tsfile_external)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

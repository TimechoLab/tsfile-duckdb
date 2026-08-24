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

set(TSFILE_ROOT "$ENV{TSFILE_ROOT}" CACHE PATH
    "Path to the Apache TsFile source tree")
if(NOT TSFILE_ROOT)
  message(FATAL_ERROR
          "TSFILE_ROOT must point to an Apache TsFile source tree")
endif()

set(TSFILE_BUILD_DIR "${TSFILE_ROOT}/cpp/build/Release" CACHE PATH
    "Path to a built Apache TsFile C++ tree")

find_library(
  TSFILE_LIBRARY
  NAMES tsfile
  PATHS "${TSFILE_BUILD_DIR}/lib"
  NO_DEFAULT_PATH)
if(NOT TSFILE_LIBRARY)
  message(FATAL_ERROR "libtsfile was not found under ${TSFILE_BUILD_DIR}/lib")
endif()

include_directories(src/include "${TSFILE_ROOT}/cpp/src")

set(EXTENSION_SOURCES
    src/tsfile_extension.cpp
    src/tsfile_copy.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} "-warnings" ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} ${TSFILE_LIBRARY})
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${TSFILE_LIBRARY})

get_filename_component(TSFILE_LIBRARY_DIR "${TSFILE_LIBRARY}" DIRECTORY)
set_target_properties(
  ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME}
  PROPERTIES BUILD_RPATH "${TSFILE_LIBRARY_DIR}")

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

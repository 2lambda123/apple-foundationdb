cmake_minimum_required(VERSION 3.13)
project(googlebenchmark-download NONE)

include(ExternalProject)
ExternalProject_Add(googlebenchmark
  GIT_REPOSITORY    https://github.com/google/benchmark.git
  # If you change this, then be sure to also update the directory name (which contains this SHA) in flowbench/CMakeLists.txt,
  # and the fdb-build-support Dockerfile (which downloads + compiles the prebuilt version)
  GIT_TAG           2dd015dfef425c866d9a43f2c67d8b52d709acb6 # v1.8.0
  GIT_SHALLOW       ON
  GIT_CONFIG        advice.detachedHead=false
  SOURCE_DIR        "${CMAKE_CURRENT_BINARY_DIR}/googlebenchmark-src"
  BINARY_DIR        "${CMAKE_CURRENT_BINARY_DIR}/googlebenchmark-build"
  CMAKE_ARGS        "-DCMAKE_BUILD_TYPE=Release -DBENCHMARK_ENABLE_LTO=true"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND   ""
  TEST_COMMAND      ""
)

include(ExternalProject)
ExternalProject_Add(googletest DEPENDS googlebenchmark
  GIT_REPOSITORY    https://github.com/google/googletest.git
  # If you change this, be sure to update the perl one-liner in the part of fdb-build-support Dockerfile that downloads
  # googlebenchmark.  That one-liner determines which SHA will be used in the pre-built version of googlebenchmark.
  GIT_TAG           58d77fa8070e8cec2dc1ed015d66b454c8d78850 # release 1.12.1
  GIT_SHALLOW       ON
  GIT_CONFIG        advice.detachedHead=false
  SOURCE_DIR        "${CMAKE_CURRENT_BINARY_DIR}/googlebenchmark-src/googletest"
  BINARY_DIR        "${CMAKE_CURRENT_BINARY_DIR}/googlebenchmark-build/googletest"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND     ""
  INSTALL_COMMAND   ""
  TEST_COMMAND      ""
)

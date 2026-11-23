cmake_minimum_required(VERSION 3.15)

project(sjq
  VERSION 0.2.0
  DESCRIPTION "A jq-compatible command-line JSON processor built on simdjson"
  LANGUAGES CXX
)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SJQ_SYSTEM_SIMDJSON "Use an installed simdjson (find_package) instead of downloading it" OFF)
option(SJQ_WERROR "Treat compiler warnings as errors" OFF)
option(SJQ_SANITIZE "Build with the address and undefined-behavior sanitizers" OFF)
option(SJQ_FUZZING "Build the fuzzer (fuzz/fuzz_sjq.cpp); needs clang and libFuzzer" OFF)
set(SJQ_FUZZ_LDFLAGS "-fsanitize=fuzzer" CACHE STRING "Link flags of the fuzzer")

# simdjson: version 5.3.0 or later (sjq uses simdjson::padded_memory_map).
if(SJQ_SYSTEM_SIMDJSON)
  find_package(simdjson 5.3 REQUIRED)
else()
  include(FetchContent)
  FetchContent_Declare(simdjson
    URL https://github.com/simdjson/simdjson/archive/refs/tags/v5.3.0.tar.gz
    URL_HASH SHA256=5e59df8bd3fa760937e6a2ca68193399c85b52b5042716700b149a734737dced
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
  set(SIMDJSON_EXCEPTIONS ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(simdjson)
  if(NOT TARGET simdjson::simdjson)
    add_library(simdjson::simdjson ALIAS simdjson)
  endif()
endif()

find_package(Threads REQUIRED)

if(SJQ_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# The engine, shared by the executable and the fuzzer.
add_library(sjq-core STATIC
  src/builtins.cpp
  src/diagnose.cpp
  src/input.cpp
  src/ondemand.cpp
  src/parser.cpp
  src/text.cpp
)
target_compile_features(sjq-core PUBLIC cxx_std_17)
target_include_directories(sjq-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(sjq-core PUBLIC simdjson::simdjson Threads::Threads)
# Warnings (the same as simdjson's developer mode).
add_library(sjq-warnings INTERFACE)
if(MSVC)
  target_compile_options(sjq-warnings INTERFACE /W3 /sdl /permissive-)
  # getenv and fopen are fine here; /sdl would make their deprecation an error.
  target_compile_definitions(sjq-warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
  if(SJQ_WERROR)
    target_compile_options(sjq-warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(sjq-warnings INTERFACE
    -Wall -Wextra -Weffc++ -Wsign-compare -Wshadow -Wwrite-strings
    -Wpointer-arith -Winit-self -Wconversion -Wno-sign-conversion)
  if(SJQ_WERROR)
    target_compile_options(sjq-warnings INTERFACE -Werror)
  endif()
endif()
target_link_libraries(sjq-core PRIVATE sjq-warnings)

add_executable(sjq src/sjq.cpp)
target_link_libraries(sjq PRIVATE sjq-core sjq-warnings)
target_compile_definitions(sjq PRIVATE SJQ_VERSION="${PROJECT_VERSION}")

if(WIN32)
  # Map input files with simdjson::padded_memory_map, which needs Windows 10
  # version 1803 or later and the onecore import library.
  target_compile_definitions(sjq-core PUBLIC
    SIMDJSON_ENABLE_MEMORY_FILE_MAPPING_ON_WINDOWS=1
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=0x0A000006
  )
  target_link_libraries(sjq-core PUBLIC onecore)
  # The default main thread stack on Windows is 1 MiB; sjq recurses up to
  # 10000 levels on deep values (as on Linux and macOS, with 8 MiB).
  if(MSVC)
    target_link_options(sjq PRIVATE "/STACK:8388608")
  elseif(MINGW)
    target_link_options(sjq PRIVATE "-Wl,--stack,8388608")
  endif()
  # The manifest makes UTF-8 the code page (Windows 10 version 1903 and
  # later), so that arguments and file names can hold any character.
  if(MSVC)
    target_sources(sjq PRIVATE src/sjq.manifest)
  elseif(MINGW)
    enable_language(RC)
    target_sources(sjq PRIVATE src/sjq.rc)
  endif()
endif()

# Release builds: drop unused code and, when available, link the C++
# standard library statically so that the binary can be copied around.
if(APPLE)
  target_link_options(sjq PRIVATE "-Wl,-dead_strip")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_link_options(sjq PRIVATE "-Wl,--gc-sections")
endif()
if(NOT SJQ_SANITIZE AND (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT APPLE)))
  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.a
    OUTPUT_VARIABLE SJQ_LIBSTDCXX_A
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(IS_ABSOLUTE "${SJQ_LIBSTDCXX_A}" AND EXISTS "${SJQ_LIBSTDCXX_A}")
    target_link_options(sjq PRIVATE "-static-libstdc++")
  endif()
endif()
if(MINGW)
  # Without it, sjq.exe needs libgcc_s_seh-1.dll next to it.
  target_link_options(sjq PRIVATE "-static-libgcc" "-static")
endif()

include(GNUInstallDirs)
install(TARGETS sjq RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(EXISTS "${PROJECT_SOURCE_DIR}/man/sjq.1")
  install(FILES "${PROJECT_SOURCE_DIR}/man/sjq.1" DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
endif()

if(SJQ_FUZZING)
  add_executable(fuzz_sjq fuzz/fuzz_sjq.cpp)
  target_link_libraries(fuzz_sjq PRIVATE sjq-core)
  target_compile_definitions(fuzz_sjq PRIVATE FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)
  target_link_options(fuzz_sjq PRIVATE ${SJQ_FUZZ_LDFLAGS})
endif()

# Tests: python3 tests/run_tests.py (see that file). The comparison with jq
# runs only when a jq is found.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  enable_testing()
  add_test(NAME sjq_tests
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.py
            --sjq $<TARGET_FILE:sjq> --group golden --group targeted --group jqsuite)
  set_tests_properties(sjq_tests PROPERTIES TIMEOUT 900)
  find_program(SJQ_JQ_EXECUTABLE jq)
  if(SJQ_JQ_EXECUTABLE)
    add_test(NAME sjq_diff_jq
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.py
              --sjq $<TARGET_FILE:sjq> --jq ${SJQ_JQ_EXECUTABLE} --group diff)
    set_tests_properties(sjq_diff_jq PROPERTIES TIMEOUT 900)
  endif()
endif()

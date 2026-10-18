cmake_minimum_required(VERSION 3.18)
project(fastsym C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# fastsym's cache layer uses the C++ STL and is built against the reduced
# runtime libc++ (LSPosed prefab-libcxx, org.lsposed.libcxx:libcxx), compiled
# with -fno-exceptions -fno-rtti and statically linked. Consumers of the
# prebuilt libs/ artifacts don't need any of this: the .a already has libc++
# baked in and links like any ordinary static library.
#
# To rebuild from source, point FASTSYM_LIBCXX_DIR at the prefab AAR root
# (the directory containing prefab/modules/cxx):
set(FASTSYM_LIBCXX_DIR "" CACHE PATH "Path to prefab-libcxx AAR root (prefab/modules/cxx)")

add_library(fastsym STATIC
  cpp/fastsym.cc
  cpp/fastsym_cache.cc
  cpp/fs_sig_guard.c
  cpp/xdl_ext.c
  cpp/xdl_lzma.cc
  cpp/xdl_util.cc
  cpp/fs_gnu_hash_neon.cc
  cpp/third_party/xdl/xdl_iterate.c
  cpp/third_party/xdl/xdl_linker.c
  cpp/third_party/llvm-demangle
  cpp/third_party/xz-embedded/xz_crc32.c
  cpp/third_party/xz-embedded/xz_dec_stream.c
  cpp/third_party/xz-embedded/xz_dec_lzma2.c
  cpp/third_party/llvm-demangle/ItaniumDemangle.cpp
)

target_include_directories(fastsym PUBLIC include)
target_include_directories(fastsym PRIVATE
  cpp
  cpp/third_party/xdl
  cpp/third_party/xz-embedded
)

# xz-embedded: decoder-only, CRC32. NOTE: do NOT pass -DXZ_DEC_*_BCJ ids here
# (even =0): xz_private.h uses #if defined(...), so defining any of them turns
# BCJ on and the link then fails on missing xz_dec_bcj*. Only ANY_CHECK is set.
target_compile_definitions(fastsym PRIVATE
  XZ_DEC_ANY_CHECK=1
)

if(NOT FASTSYM_LIBCXX_DIR STREQUAL "")
  # Bare-CMake path: point at an extracted prefab-libcxx AAR manually.
  set(_fs_cxx_mod ${FASTSYM_LIBCXX_DIR}/prefab/modules/cxx)
  target_compile_options(fastsym PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
    $<$<COMPILE_LANGUAGE:CXX>:-isystem$<SEMICOLON>${_fs_cxx_mod}/include>
    -fno-exceptions -fno-rtti)
  target_link_options(fastsym PRIVATE -Wl,--as-needed)
  target_link_libraries(fastsym INTERFACE ${_fs_cxx_mod}/libs/android.${CMAKE_ANDROID_ARCH_ABI}/libcxx.a)
else()
  # AGP/prefab path (official): enable prefab in gradle, add
  # implementation("org.lsposed.libcxx:libcxx:<ndk-version>"), pass
  # -DANDROID_STL=none, then the cxx package is findable:
  find_package(cxx QUIET CONFIG)
  if(cxx_FOUND)
    target_link_libraries(fastsym PRIVATE cxx::cxx)
    target_compile_options(fastsym PRIVATE -fno-exceptions -fno-rtti)
  endif()
endif()

option(FASTSYM_SHARED "Build fastsym as a shared library instead of static" OFF)
if(FASTSYM_SHARED)
  add_library(fastsym_shared SHARED $<TARGET_OBJECTS:fastsym>)
  set_target_properties(fastsym_shared PROPERTIES OUTPUT_NAME fastsym)
  if(UNIX)
    target_link_options(fastsym_shared PRIVATE
      -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/fastsym.map.txt)
  endif()
endif()

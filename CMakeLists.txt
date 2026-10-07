
# Sniper — host-side build.
#
# Builds everything: the simulator (libcarbon_sim, libsift, libdecoder,
# lib/sniper, siftdump), the SDE SIFT recorder (through Intel's mfile.py, see
# cmake/SniperPinTools.cmake), and optionally the DynamoRIO front end
# (SNIPER_ENABLE_DYNAMORIO), the ARM decoder (SNIPER_ENABLE_ARM), the QEMU
# plugin (SNIPER_ENABLE_QEMU) and the userspace MimicOS kernel.  The top-level
# Makefile is a forwarder that fetches the dependencies and calls this.
#
#   cmake -S . -B build/release -G Ninja
#   cmake --build build/release
#
# The point of moving here is not the syntax.  It is that the build system
# tracks the flags each object was compiled with, so changing a configuration
# option rebuilds what that option affects.  With the recursive makefiles it did
# not: `make DEBUG=1` on a release tree recompiled nothing and handed back the
# release binary, and enabling RISC-V left all 237 core objects compiled with
# SNIPER_RISCV=0 while the decoder was built with it set.

cmake_minimum_required(VERSION 3.18)   # Python3 Development.Embed

project(sniper
  VERSION 8.1
  DESCRIPTION "Sniper multi-core simulator"
  LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(SniperOptions)
include(SniperDependencies)

# ---------------------------------------------------------------------------
# Shared compile settings
# ---------------------------------------------------------------------------

# Two layers, deliberately.
#
# sniper_base is the language and ABI contract every object in the tree shares.
# sniper_common adds the simulator's own dependency set on top.
#
# They were one target, and that was wrong: libsift linked sniper_common, so
# anything linking libsift inherited Python, libtorch, sqlite and XED.  The
# DynamoRIO client is the case that exposed it -- DynamoRIO refused to load the
# client with "Unable to load client library: libc10.so", a libtorch library a
# trace frontend has no business depending on.
add_library(sniper_base INTERFACE)

target_compile_features(sniper_base INTERFACE cxx_std_20)

target_compile_options(sniper_base INTERFACE
  -Wall -Wextra -Wcast-align
  -Wno-unused-parameter -Wno-unknown-pragmas
  -fno-strict-aliasing)

target_compile_definitions(sniper_base INTERFACE
  TARGET_INTEL64
  TARGET_IA32E
  PIN_REV=${SNIPER_PIN_REV}
  SNIPER_RISCV=$<BOOL:${SNIPER_ENABLE_RISCV}>
  SNIPER_ARM=$<BOOL:${SNIPER_ENABLE_ARM}>)

# libtorch ships a pre-cxx11-ABI build, and every object that may end up in the
# same link has to agree.  Confined to the torch-enabled configuration: with
# torch off, Sniper builds with the platform's normal ABI.
if(SNIPER_ENABLE_TORCH)
  target_compile_definitions(sniper_base INTERFACE _GLIBCXX_USE_CXX11_ABI=0)
  # The libtorch branch predictor guards its entire implementation, its header
  # and its use in the factory behind HAS_LIBTORCH.  Nothing had ever defined
  # it -- not this build, not the makefiles before it -- so nn_branch_predictor
  # compiled to an empty translation unit and the predictor was unreachable
  # while libtorch was still linked in.
  target_compile_definitions(sniper_base INTERFACE HAS_LIBTORCH=1)
endif()

add_library(sniper_common INTERFACE)
target_link_libraries(sniper_common INTERFACE sniper_base)

# Every directory under common/ is an include path, matching the historical
# `find common -type d` behaviour: headers are included by bare name from
# anywhere in the tree.
file(GLOB_RECURSE _sniper_common_dirs LIST_DIRECTORIES true
     "${CMAKE_CURRENT_SOURCE_DIR}/common/*")
set(SNIPER_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/common")
foreach(_d IN LISTS _sniper_common_dirs)
  if(IS_DIRECTORY "${_d}")
    list(APPEND SNIPER_INCLUDE_DIRS "${_d}")
  endif()
endforeach()
list(APPEND SNIPER_INCLUDE_DIRS
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/sift"
  "${CMAKE_CURRENT_SOURCE_DIR}/decoder_lib"
  "${CMAKE_CURRENT_SOURCE_DIR}/linux")

target_include_directories(sniper_common INTERFACE ${SNIPER_INCLUDE_DIRS})

if(SNIPER_CHAMPSIM_INCLUDE)
  target_include_directories(sniper_common INTERFACE "${SNIPER_CHAMPSIM_INCLUDE}")
endif()

target_link_libraries(sniper_common INTERFACE
  Sniper::Xed
  Python3::Python
  ${CMAKE_DL_LIBS}
  rt z bz2 sqlite3 pthread)

if(SNIPER_HAVE_LZMA)
  target_link_libraries(sniper_common INTERFACE lzma)
endif()

if(SNIPER_ENABLE_TORCH)
  target_link_libraries(sniper_common INTERFACE Sniper::Torch)
endif()

# Capstone.  The include directories were here already; the library was not, so
# an ARM build compiled arm_decoder.cc and then failed to link on cs_open,
# cs_option and the rest.  The old decoder_lib/Makefile named
# ../../capstone/libcapstone.a explicitly and that went with it.
if(SNIPER_ENABLE_ARM)
  find_library(SNIPER_CAPSTONE_LIBRARY NAMES capstone
               HINTS "${CMAKE_CURRENT_SOURCE_DIR}/capstone" NO_DEFAULT_PATH)
  if(NOT SNIPER_CAPSTONE_LIBRARY)
    message(FATAL_ERROR
      "Capstone not found under ${CMAKE_CURRENT_SOURCE_DIR}/capstone.\n"
      "  Acquire it first:  deps/sniper-deps --arm fetch")
  endif()
  add_library(Sniper::Capstone UNKNOWN IMPORTED)
  set_target_properties(Sniper::Capstone PROPERTIES
    IMPORTED_LOCATION "${SNIPER_CAPSTONE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES
      "${CMAKE_CURRENT_SOURCE_DIR}/capstone/include;${CMAKE_CURRENT_SOURCE_DIR}/capstone/include/capstone")
  target_link_libraries(sniper_common INTERFACE Sniper::Capstone)
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

# SIFT, host ABI.  An object library so the trace reader and siftdump share one
# compile; the PinCRT variant is a separate target, added in phase 2.
file(GLOB SIFT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sift/*.cc")
list(REMOVE_ITEM SIFT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/sift/siftdump.cc")

add_library(sift STATIC ${SIFT_SOURCES})

# SIFT reads and writes compressed trace files.  That is the whole of it: no
# Python, no libtorch, no XED.  Keeping this list honest is what lets a
# DynamoRIO client or a pintool link libsift without dragging the simulator's
# dependencies in behind it.
target_include_directories(sift PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/sift"
  "${CMAKE_CURRENT_SOURCE_DIR}/common/misc")
target_link_libraries(sift PUBLIC sniper_base z bz2)
if(SNIPER_HAVE_LZMA)
  target_link_libraries(sift PUBLIC lzma)
endif()
if(SNIPER_CHAMPSIM_INCLUDE)
  target_include_directories(sift PUBLIC "${SNIPER_CHAMPSIM_INCLUDE}")
endif()
set_target_properties(sift PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(siftdump "${CMAKE_CURRENT_SOURCE_DIR}/sift/siftdump.cc")
target_link_libraries(siftdump PRIVATE sift)

# Instruction decoders.  ARM sources are compiled only when their option is
# on (Capstone); the RISC-V decoder has no external dependency.
file(GLOB DECODER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/decoder_lib/*.cc")
if(NOT SNIPER_ENABLE_ARM)
  list(FILTER DECODER_SOURCES EXCLUDE REGEX "/arm[^/]*\\.cc$")
endif()
if(NOT SNIPER_ENABLE_RISCV)
  # The decoder needs the RISC-V model; the generated class tables (riscv_isa*)
  # stay, since the BOOM core model reads them whatever the ISA option.
  list(FILTER DECODER_SOURCES EXCLUDE REGEX "/riscv_decoder\\.cc$")
endif()
add_library(decoder STATIC ${DECODER_SOURCES})
target_link_libraries(decoder PUBLIC sniper_common)
set_target_properties(decoder PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The RISC-V decoder is generated from the RISC-V Unified Database
# (tools/riscv/gen_isa.py); `cmake --build <build> --target riscv-isa-gen` regenerates it when
# SNIPER_UDB_ROOTS lists the specification checkouts to build it from.
set(SNIPER_UDB_ROOTS "" CACHE STRING "UDB roots (;-separated) for tools/riscv/gen_isa.py: upstream, then overlays")
if(SNIPER_UDB_ROOTS)
  set(_udb_args "")
  foreach(_r ${SNIPER_UDB_ROOTS})
    list(APPEND _udb_args --udb "${_r}")
  endforeach()
  set(SNIPER_AME_ROOT "" CACHE PATH "riscv-attached-matrix-extension checkout: regenerate the AME (Ztt) overlay first")
  set(_ame_cmd "")
  if(SNIPER_AME_ROOT)
    set(_ame_cmd COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tools/riscv/gen_ame_udb.py" --ame "${SNIPER_AME_ROOT}" --out "${CMAKE_CURRENT_SOURCE_DIR}/tools/riscv/overlay")
  endif()
  add_custom_target(riscv-isa-gen
    ${_ame_cmd}
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tools/riscv/gen_isa.py" ${_udb_args}
            --udb "${CMAKE_CURRENT_SOURCE_DIR}/tools/riscv/overlay"
            --policy "${CMAKE_CURRENT_SOURCE_DIR}/tools/riscv/isa_policy.yaml"
            --out "${CMAKE_CURRENT_SOURCE_DIR}/decoder_lib"
            --config "${CMAKE_CURRENT_SOURCE_DIR}/config/common_configs/riscv_isa_classes.cfg"
            --header "${CMAKE_CURRENT_SOURCE_DIR}/include/riscv_insn.h"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Regenerating the RISC-V decoder tables from the Unified Database")
endif()

# The simulator core.
file(GLOB_RECURSE CARBON_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/common/*.cc"
     "${CMAKE_CURRENT_SOURCE_DIR}/common/*.cpp")

if(NOT SNIPER_ENABLE_RISCV)
  # The attached matrix tile executes RISC-V AME instructions; without the
  # RISC-V model ame_tile.h provides the stub the ROB timer links against.
  list(FILTER CARBON_SOURCES EXCLUDE REGEX "/ame_tile\\.cc$")
endif()

add_library(carbon STATIC ${CARBON_SOURCES})
target_link_libraries(carbon PUBLIC sniper_common sift decoder)
set_target_properties(carbon PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The simulator binary.
file(GLOB STANDALONE_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/simulator/*.cc")

add_executable(sniper ${STANDALONE_SOURCES})
target_link_libraries(sniper PRIVATE carbon sift decoder sniper_common)
target_link_options(sniper PRIVATE -no-pie)

# Artifacts stay inside the build directory.  That is the whole point of the
# exercise: two configurations can exist at once, which they cannot while every
# path in the tree is fixed.
set_target_properties(sniper siftdump PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set_target_properties(carbon PROPERTIES OUTPUT_NAME carbon_sim)

# run-sniper still looks for $SNIPER_ROOT/lib/sniper, so until it learns to
# resolve a build directory (phase 3) there has to be a way to point it at one.
# Opt in, and only one build directory may claim the tree at a time.
option(SNIPER_LINK_INTO_TREE
       "Point <source>/lib/sniper at this build directory, for run-sniper" OFF)
if(SNIPER_LINK_INTO_TREE)
  add_custom_command(TARGET sniper POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/lib"
    COMMAND "${CMAKE_COMMAND}" -E create_symlink
            "$<TARGET_FILE:sniper>" "${CMAKE_CURRENT_SOURCE_DIR}/lib/sniper"
    COMMENT "Linking lib/sniper -> ${CMAKE_BINARY_DIR}/lib/sniper")
endif()

# ---------------------------------------------------------------------------
# Pin-side targets
# ---------------------------------------------------------------------------

# Pin's flags carry their own optimisation level, their own C++ runtime and
# -nostdlib.  CMake's per-configuration flags on top of that produce a link that
# mixes two runtimes, so strip them for Pin-side targets.
set(SNIPER_PIN_NO_HOST_FLAGS "")
find_package(Pin)
include(SniperPinTools)

# The DynamoRIO frontend, built in the same tree rather than configured by hand
# into its own build directory from a rule in the Makefile.
if(SNIPER_ENABLE_DYNAMORIO)
  add_subdirectory(frontend/dr-frontend)
endif()

# The QEMU frontend: a TCG plugin, loaded into a stock QEMU with -plugin.
if(SNIPER_ENABLE_QEMU)
  add_subdirectory(frontend/qemu-plugin)
endif()

# Userspace MimicOS: a guest binary, built here so that it exists without a
# separate hand-run make and so each build directory gets its own.
add_subdirectory(mimicos)

# ---------------------------------------------------------------------------
# What the runtime tools need to know about this build
# ---------------------------------------------------------------------------
include(SniperRuntimeConfig)

sniper_print_configuration()

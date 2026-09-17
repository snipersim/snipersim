# Build configuration.
#
# Every one of these is an explicit, recorded cache entry.  The makefiles derived
# the equivalents from the environment and from whether directories happened to
# exist next to the repository -- cloning rv8 as a sibling silently turned on
# SNIPER_RISCV for the whole tree, and nothing rebuilt.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
             Debug Release RelWithDebInfo MinSizeRel)

# Match what the makefiles produced: -O2 -g for a normal build, -O0 -g for DEBUG=1.
set(CMAKE_C_FLAGS_RELEASE   "-O2 -g" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_DEBUG     "-O0 -g" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g" CACHE STRING "" FORCE)

set(SNIPER_FRONTEND "sde" CACHE STRING
    "Instrumentation frontend: sde, pin, pinplay or none")
set_property(CACHE SNIPER_FRONTEND PROPERTY STRINGS sde pin pinplay none)

option(SNIPER_ENABLE_RISCV     "RISC-V core model (decoder generated from the UDB)" ON)
option(SNIPER_ENABLE_ARM       "Build the ARM decoder (needs Capstone)"          OFF)
option(SNIPER_ENABLE_DYNAMORIO "Build the DynamoRIO frontend"                    OFF)
option(SNIPER_ENABLE_QEMU      "Build the QEMU TCG plugin frontend"              OFF)
option(SNIPER_ENABLE_TORCH     "Build the libtorch branch predictor"             ON)
option(SNIPER_ENABLE_MCPAT     "Use McPAT for power and area"                    ON)

set(SNIPER_CHAMPSIM_INCLUDE "" CACHE PATH
    "ChampSim include directory, if ChampSim trace support is wanted")

# Compiler cache.
#
# This matters more here than in most projects: with several build directories
# and a legacy ABI that a preset can turn on or off, the same translation unit
# gets compiled in more than one configuration, and switching between them used
# to mean a full rebuild from zero.  ccache makes the second and later builds of
# an already-seen configuration nearly free.
set(SNIPER_COMPILER_LAUNCHER "auto" CACHE STRING
    "Compiler cache: auto, ccache, sccache, or none")
set_property(CACHE SNIPER_COMPILER_LAUNCHER PROPERTY STRINGS auto ccache sccache none)

if(NOT SNIPER_COMPILER_LAUNCHER STREQUAL "none")
  if(SNIPER_COMPILER_LAUNCHER STREQUAL "auto")
    find_program(_sniper_launcher NAMES ccache sccache)
  else()
    find_program(_sniper_launcher NAMES ${SNIPER_COMPILER_LAUNCHER})
    if(NOT _sniper_launcher)
      message(FATAL_ERROR
        "SNIPER_COMPILER_LAUNCHER=${SNIPER_COMPILER_LAUNCHER} but it is not on PATH.\n"
        "  Install it, or configure with -DSNIPER_COMPILER_LAUNCHER=none.")
    endif()
  endif()
  if(_sniper_launcher)
    set(CMAKE_C_COMPILER_LAUNCHER   "${_sniper_launcher}" CACHE STRING "" FORCE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${_sniper_launcher}" CACHE STRING "" FORCE)
    set(SNIPER_LAUNCHER_IN_USE "${_sniper_launcher}")
  else()
    set(SNIPER_LAUNCHER_IN_USE "none found")
  endif()
else()
  set(SNIPER_LAUNCHER_IN_USE "disabled")
endif()

function(sniper_print_configuration)
  message(STATUS "")
  message(STATUS "Sniper ${PROJECT_VERSION}")
  message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
  message(STATUS "  compiler ........... ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
  message(STATUS "  frontend ........... ${SNIPER_FRONTEND}")
  message(STATUS "  RISC-V decoder ..... ${SNIPER_ENABLE_RISCV}")
  message(STATUS "  ARM decoder ........ ${SNIPER_ENABLE_ARM}")
  message(STATUS "  DynamoRIO frontend . ${SNIPER_ENABLE_DYNAMORIO}")
  message(STATUS "  QEMU frontend ...... ${SNIPER_ENABLE_QEMU}")
  message(STATUS "  libtorch ........... ${SNIPER_ENABLE_TORCH}")
  message(STATUS "  McPAT .............. ${SNIPER_ENABLE_MCPAT}")
  message(STATUS "  Pin revision ....... ${SNIPER_PIN_REV}")
  message(STATUS "  liblzma ............ ${SNIPER_HAVE_LZMA}")
  message(STATUS "  compiler cache ..... ${SNIPER_LAUNCHER_IN_USE}")
  message(STATUS "  Python ............. ${Python3_VERSION} (${Python3_EXECUTABLE})")
  message(STATUS "")
endfunction()

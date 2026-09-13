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

option(SNIPER_ENABLE_RISCV     "Build the RISC-V decoder (needs rv8)"            OFF)
option(SNIPER_ENABLE_ARM       "Build the ARM decoder (needs Capstone)"          OFF)
option(SNIPER_ENABLE_DYNAMORIO "Build the DynamoRIO frontend"                    OFF)
option(SNIPER_ENABLE_TORCH     "Build the libtorch branch predictor"             ON)
option(SNIPER_ENABLE_MCPAT     "Use McPAT for power and area"                    ON)

set(SNIPER_RV8_HOME "${CMAKE_CURRENT_SOURCE_DIR}/../rv8" CACHE PATH
    "rv8 checkout, for the RISC-V decoder")
set(SNIPER_CHAMPSIM_INCLUDE "" CACHE PATH
    "ChampSim include directory, if ChampSim trace support is wanted")

function(sniper_print_configuration)
  message(STATUS "")
  message(STATUS "Sniper ${PROJECT_VERSION}")
  message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
  message(STATUS "  compiler ........... ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
  message(STATUS "  frontend ........... ${SNIPER_FRONTEND}")
  message(STATUS "  RISC-V decoder ..... ${SNIPER_ENABLE_RISCV}")
  message(STATUS "  ARM decoder ........ ${SNIPER_ENABLE_ARM}")
  message(STATUS "  DynamoRIO frontend . ${SNIPER_ENABLE_DYNAMORIO}")
  message(STATUS "  libtorch ........... ${SNIPER_ENABLE_TORCH}")
  message(STATUS "  McPAT .............. ${SNIPER_ENABLE_MCPAT}")
  message(STATUS "  Pin revision ....... ${SNIPER_PIN_REV}")
  message(STATUS "  liblzma ............ ${SNIPER_HAVE_LZMA}")
  message(STATUS "  Python ............. ${Python3_VERSION} (${Python3_EXECUTABLE})")
  message(STATUS "")
endfunction()

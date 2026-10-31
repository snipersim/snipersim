# Locating the external dependencies.
#
# All of these are probed once, at configure time.  The makefiles re-ran the
# equivalents on every parse -- python3-config twice, pinversion.py, a compiler
# version query, a find over common/, and a live test compile -- in each of the
# eight make processes a build spawns.

include(CheckCXXSourceCompiles)

# --- Python, for the embedded scripting hooks -------------------------------
#
# Development.Embed gives both the include path and the library path.  The
# makefiles asked python3-config for --includes and --libs but never --ldflags,
# so the link failed against any interpreter outside the default search path
# (conda, pyenv, spack, an HPC `module load python`).
find_package(Python3 COMPONENTS Interpreter Development.Embed)
if(NOT Python3_Development.Embed_FOUND)
  # CMake's own message ("missing: Python3_INCLUDE_DIRS Development.Embed")
  # does not say that the interpreter's development package is what is
  # missing -- the usual case on a fresh system.
  message(FATAL_ERROR
    "Python ${Python3_VERSION} found (${Python3_EXECUTABLE}), but not its headers and library, "
    "which the simulator embeds for its scripting hooks.  Install the development package: "
    "python3-dev on Debian and Ubuntu, python3-devel on Fedora and RHEL; or set Python3_EXECUTABLE "
    "(make CMAKE_ARGS=-DPython3_EXECUTABLE=...) to an interpreter that has them.")
endif()

if(Python3_VERSION VERSION_LESS 3.8)
  message(FATAL_ERROR
    "Python ${Python3_VERSION} found, but the embedded interpreter needs 3.8 or newer "
    "(common/scripting/hooks_py.h enforces the same). Set Python3_EXECUTABLE to select one.")
endif()

# --- XED --------------------------------------------------------------------
set(SNIPER_XED_HOME "${CMAKE_CURRENT_SOURCE_DIR}/xed_kit" CACHE PATH "Built XED kit")
find_library(SNIPER_XED_LIBRARY NAMES xed
             HINTS "${SNIPER_XED_HOME}/lib" NO_DEFAULT_PATH)
find_path(SNIPER_XED_INCLUDE_DIR NAMES xed-interface.h
          HINTS "${SNIPER_XED_HOME}/include/xed" NO_DEFAULT_PATH)

if(NOT SNIPER_XED_LIBRARY OR NOT SNIPER_XED_INCLUDE_DIR)
  message(FATAL_ERROR
    "XED not found under ${SNIPER_XED_HOME}.\n"
    "  Acquire it first:  deps/sniper-deps fetch\n"
    "  or point SNIPER_XED_HOME at an existing kit.")
endif()

add_library(Sniper::Xed UNKNOWN IMPORTED)
set_target_properties(Sniper::Xed PROPERTIES
  IMPORTED_LOCATION "${SNIPER_XED_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${SNIPER_XED_INCLUDE_DIR}")

# --- libtorch ---------------------------------------------------------------
if(SNIPER_ENABLE_TORCH)
  set(SNIPER_TORCH_HOME "${CMAKE_CURRENT_SOURCE_DIR}/libtorch" CACHE PATH "libtorch")
  find_library(SNIPER_TORCH_LIBRARY NAMES torch
               HINTS "${SNIPER_TORCH_HOME}/lib" NO_DEFAULT_PATH)
  find_library(SNIPER_C10_LIBRARY NAMES c10
               HINTS "${SNIPER_TORCH_HOME}/lib" NO_DEFAULT_PATH)
  # libtorch.so is a thin dispatcher in current libtorch: the C++ API lives in
  # libtorch_cpu.so.  Linking only the first two leaves every torch::nn::Module
  # symbol undefined, which is what happened the moment anything actually used
  # the branch predictor.
  find_library(SNIPER_TORCH_CPU_LIBRARY NAMES torch_cpu
               HINTS "${SNIPER_TORCH_HOME}/lib" NO_DEFAULT_PATH)
  if(NOT SNIPER_TORCH_LIBRARY OR NOT SNIPER_C10_LIBRARY OR NOT SNIPER_TORCH_CPU_LIBRARY)
    message(FATAL_ERROR
      "libtorch not found under ${SNIPER_TORCH_HOME}.\n"
      "  Fetch it:  deps/sniper-deps fetch\n"
      "  or configure with -DSNIPER_ENABLE_TORCH=OFF.")
  endif()
  add_library(Sniper::Torch INTERFACE IMPORTED)
  # -isystem: libtorch pulls in <semaphore> via C++20, which collides with
  # common/misc/semaphore.h if its headers are searched as normal includes.
  set_target_properties(Sniper::Torch PROPERTIES
    INTERFACE_LINK_LIBRARIES "${SNIPER_TORCH_LIBRARY};${SNIPER_TORCH_CPU_LIBRARY};${SNIPER_C10_LIBRARY}"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
      "${SNIPER_TORCH_HOME}/include;${SNIPER_TORCH_HOME}/include/torch/csrc/api/include"
    INTERFACE_INCLUDE_DIRECTORIES
      "${SNIPER_TORCH_HOME}/include;${SNIPER_TORCH_HOME}/include/torch/csrc/api/include")
endif()

# --- Boost (headers only) ---------------------------------------------------
find_path(SNIPER_BOOST_INCLUDE_DIR NAMES boost/version.hpp)
if(NOT SNIPER_BOOST_INCLUDE_DIR)
  message(FATAL_ERROR "Boost headers not found; install libboost-dev.")
endif()

# --- liblzma (optional) -----------------------------------------------------
#
# Probed with a program that includes the header, not one that merely links the
# library: a system with liblzma.so but no development headers used to be told
# it had lzma and then failed to compile.
set(CMAKE_REQUIRED_LIBRARIES lzma)
check_cxx_source_compiles([[
  #include <lzma.h>
  int main() { lzma_stream s = LZMA_STREAM_INIT; return (int)s.avail_in; }
]] SNIPER_HAVE_LZMA)
unset(CMAKE_REQUIRED_LIBRARIES)

# --- Pin revision -----------------------------------------------------------
#
# Compiled into the simulator as PIN_REV.  The makefiles ran pinversion.py on
# every parse of Makefile.common, in every recursive make.
set(SNIPER_PIN_REV 0)
if(SNIPER_FRONTEND STREQUAL "sde")
  set(_pin_home "${CMAKE_CURRENT_SOURCE_DIR}/sde_kit/pinkit")
elseif(SNIPER_FRONTEND STREQUAL "none")
  set(_pin_home "")
else()
  set(_pin_home "${CMAKE_CURRENT_SOURCE_DIR}/pin_kit")
endif()

if(_pin_home AND EXISTS "${_pin_home}")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/pinversion.py" "${_pin_home}"
    OUTPUT_VARIABLE _pin_version OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE _pin_rc)
  if(_pin_rc EQUAL 0)
    string(REPLACE "." ";" _pin_parts "${_pin_version}")
    list(LENGTH _pin_parts _n)
    if(_n GREATER_EQUAL 3)
      list(GET _pin_parts 2 SNIPER_PIN_REV)
    endif()
  endif()
endif()
if(NOT SNIPER_PIN_REV MATCHES "^[0-9]+$")
  set(SNIPER_PIN_REV 0)
endif()

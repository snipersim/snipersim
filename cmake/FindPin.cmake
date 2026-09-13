# Locate a Pin or SDE kit and learn how it wants tools built.
#
# Pin ships its own build framework.  Every Sniper pintool used to get the flags
# by including $(PIN_ROOT)/source/tools/Config/makefile.config and inheriting the
# kit's rules wholesale, which meant each tool directory carried a copy of Pin's
# makefile scaffolding, its own OBJDIR convention, and -- because the rules only
# know how to build things inside the tool directory -- its own private copy of
# libsift assembled from symlinks to the real sources.
#
# Reimplementing those flags by hand would rot the first time Intel changes them.
# Instead include makefile.config once, at configure time, print what it produced
# and cache it.  The variable names have been stable across kits since Pin 2.14.
#
# Defines, on success:
#   Pin_FOUND, Pin_ROOT_DIR, Pin_TARGET, Pin_TOOL_SUFFIX
#   Sniper::PinTool   -- an interface target carrying the kit's tool build flags
#   Sniper::SdeLibs   -- the SDE/PinPlay libraries, when the kit is an SDE kit

include(FindPackageHandleStandardArgs)

set(Pin_PROBE "${CMAKE_CURRENT_LIST_DIR}/pin/probe.mk")

# --- find the kit -----------------------------------------------------------
if(SNIPER_FRONTEND STREQUAL "sde")
  set(_sde_kit "${CMAKE_SOURCE_DIR}/sde_kit")
  set(_pin_root "${_sde_kit}/pinkit")
  set(_probe_args "SDE_BUILD_KIT=${_sde_kit}")
  set(_kind "SDE")
elseif(SNIPER_FRONTEND STREQUAL "none")
  set(Pin_FOUND FALSE)
  return()
else()
  set(_sde_kit "")
  set(_pin_root "${CMAKE_SOURCE_DIR}/pin_kit")
  set(_probe_args "PIN_ROOT=${_pin_root}")
  set(_kind "Pin")
endif()

if(NOT EXISTS "${_pin_root}/source/tools/Config/makefile.config")
  set(Pin_FOUND FALSE)
  if(Pin_FIND_REQUIRED)
    message(FATAL_ERROR
      "No ${_kind} kit under ${_pin_root}.\n"
      "  Fetch it:  deps/sniper-deps --frontend ${SNIPER_FRONTEND} fetch\n"
      "  or configure with -DSNIPER_FRONTEND=none to build without a frontend.")
  endif()
  return()
endif()

# --- ask the kit ------------------------------------------------------------
function(_pin_probe var out)
  execute_process(
    COMMAND make -s -f "${Pin_PROBE}" ${_probe_args} "print-${var}"
    OUTPUT_VARIABLE _v OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Probing the ${_kind} kit for ${var} failed:\n${_err}")
  endif()
  set(${out} "${_v}" PARENT_SCOPE)
endfunction()

_pin_probe(TARGET          Pin_TARGET)
_pin_probe(PINTOOL_SUFFIX  Pin_TOOL_SUFFIX)
_pin_probe(TOOL_CXXFLAGS   _tool_cxxflags)
_pin_probe(TOOL_LDFLAGS    _tool_ldflags)
_pin_probe(TOOL_LPATHS     _tool_lpaths)
_pin_probe(TOOL_LIBS       _tool_libs)
if(_sde_kit)
  _pin_probe(SDE_TOOL_CXXFLAGS _sde_cxxflags)
  _pin_probe(SDE_TOOL_LPATHS   _sde_lpaths)
  _pin_probe(SDE_LIBS          _sde_libs)
endif()

if(Pin_TARGET STREQUAL "" OR _tool_cxxflags STREQUAL "")
  message(FATAL_ERROR
    "The ${_kind} kit under ${_pin_root} did not yield the expected variables.\n"
    "  This usually means the kit's makefile.config changed shape.\n"
    "  ${Pin_PROBE} is the place to adapt.")
endif()

# Options that take a separate path argument have to stay glued to it, and must
# survive CMake's de-duplication of repeated identical options.  Pin passes
# -isystem five times; left as plain list entries, CMake keeps the first and
# drops the rest, so four PinCRT include directories lose their -isystem, the
# host's /usr/include/c++ wins the search order, and the tool compiles against a
# mixture of two C++ runtimes.  "SHELL:" groups are kept verbatim and intact.
function(_pin_pair_options in out)
  set(_result "")
  set(_pending "")
  foreach(_arg IN LISTS ${in})
    if(_pending)
      list(APPEND _result "SHELL:${_pending} ${_arg}")
      set(_pending "")
    elseif(_arg MATCHES "^-(isystem|include|imacros|idirafter|iquote)$")
      set(_pending "${_arg}")
    else()
      list(APPEND _result "${_arg}")
    endif()
  endforeach()
  if(_pending)
    list(APPEND _result "${_pending}")
  endif()
  set(${out} "${_result}" PARENT_SCOPE)
endfunction()

separate_arguments(_tool_cxxflags NATIVE_COMMAND "${_tool_cxxflags}")
_pin_pair_options(_tool_cxxflags _tool_cxxflags)
separate_arguments(_tool_ldflags  NATIVE_COMMAND "${_tool_ldflags}")
separate_arguments(_tool_lpaths   NATIVE_COMMAND "${_tool_lpaths}")
separate_arguments(_tool_libs     NATIVE_COMMAND "${_tool_libs}")
if(_sde_kit)
  separate_arguments(_sde_cxxflags NATIVE_COMMAND "${_sde_cxxflags}")
  _pin_pair_options(_sde_cxxflags _sde_cxxflags)
  separate_arguments(_sde_lpaths NATIVE_COMMAND "${_sde_lpaths}")
  separate_arguments(_sde_libs   NATIVE_COMMAND "${_sde_libs}")
endif()

set(Pin_ROOT_DIR "${_pin_root}")
set(Pin_SDE_KIT  "${_sde_kit}")

# --- expose it as a target --------------------------------------------------
#
# The flags go in as compile/link *options* rather than being split into include
# directories and definitions.  They arrive as one ordered command line -- with
# -isystem paired with its path, a specific CRT object at a specific position,
# and -nostdlib before the libraries it suppresses -- and reordering them breaks
# the link.
if(NOT TARGET Sniper::PinTool)
  add_library(Sniper::PinTool INTERFACE IMPORTED)
  set_target_properties(Sniper::PinTool PROPERTIES
    INTERFACE_COMPILE_OPTIONS "${_tool_cxxflags}"
    INTERFACE_LINK_OPTIONS    "${_tool_ldflags};${_tool_lpaths}")
endif()

# An SDE tool: the base flags plus SDE_INIT, PinPlay and their libraries.  A
# plain pintool must not get these -- they gate real code, and building the Pin
# frontend with them fails to compile.
if(_sde_kit AND NOT TARGET Sniper::SdeTool)
  add_library(Sniper::SdeTool INTERFACE IMPORTED)
  set_target_properties(Sniper::SdeTool PROPERTIES
    INTERFACE_COMPILE_OPTIONS "${_tool_cxxflags};${_sde_cxxflags}"
    INTERFACE_LINK_OPTIONS    "${_tool_ldflags};${_tool_lpaths};${_sde_lpaths}"
    INTERFACE_LINK_LIBRARIES  "${_sde_libs}")
endif()

# Kept as a plain list: these must land at the end of the link line, after the
# tool's own objects and after libsift.
set(Pin_TOOL_LIBS "${_tool_libs}")

find_package_handle_standard_args(Pin
  REQUIRED_VARS Pin_ROOT_DIR Pin_TARGET Pin_TOOL_SUFFIX
  FAIL_MESSAGE "No usable Pin or SDE kit")

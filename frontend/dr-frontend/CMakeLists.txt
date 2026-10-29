# **********************************************************
# Copyright (c) 2010-2015 Google, Inc.    All rights reserved.
# Copyright (c) 2009-2010 VMware, Inc.    All rights reserved.
# **********************************************************
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice,
#   this list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials distributed with the distribution.
#
# * Neither the name of VMware, Inc. nor the names of its contributors may be
#   used to endorse or promote products derived from this software without
#   specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
# DAMAGE.

# Built as part of the main project when SNIPER_ENABLE_DYNAMORIO is on, rather
# than as a standalone project configured by hand into its own build/ directory
# with a cmake invocation buried in the top-level Makefile.
#
# Changes from the standalone version:
#  - no project() call, since this is now a subdirectory
#  - links the `sift` target instead of a hardcoded path to sift/libsift.a, so
#    the dependency is expressed rather than assumed to have been built already
#  - SNIPER_FRONTEND_DIR, not SNIPER_FRONTEND: the latter is now the option that
#    selects the instrumentation frontend

# deps/sniper-deps builds DynamoRIO and links it in at dynamorio/, so look
# there by default; -DDynamoRIO_DIR still wins for a DynamoRIO built elsewhere.
if(NOT DynamoRIO_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/dynamorio/build/cmake")
  set(DynamoRIO_DIR "${CMAKE_SOURCE_DIR}/dynamorio/build/cmake")
endif()

find_package(DynamoRIO)
if(NOT DynamoRIO_FOUND)
  message(FATAL_ERROR
    "DynamoRIO not found.\n"
    "  Acquire it:  deps/sniper-deps --dynamorio fetch\n"
    "  or point -DDynamoRIO_DIR at an existing <dynamorio>/build/cmake,\n"
    "  or turn the frontend off with -DSNIPER_ENABLE_DYNAMORIO=OFF.")
endif()

set(SNIPER_FRONTEND_DIR "${CMAKE_SOURCE_DIR}/frontend")

set(DR_FRONTEND_HEADERS
  ${SNIPER_FRONTEND_DIR}/frontend.h
  ${SNIPER_FRONTEND_DIR}/frontend_threads.h
  ${SNIPER_FRONTEND_DIR}/frontend_defs.h
  ${SNIPER_FRONTEND_DIR}/bbv_count.h
  ${SNIPER_FRONTEND_DIR}/frontend_callbacks.h
  ${SNIPER_FRONTEND_DIR}/frontend_control.h
  ${SNIPER_FRONTEND_DIR}/frontend_options.h
  ${SNIPER_FRONTEND_DIR}/frontend_syscall.h
  ${SNIPER_FRONTEND_DIR}/frontend_utils.h)

add_library(dr_frontend SHARED
  dr_frontend.cc
  dr_fe_time.cc
  ${SNIPER_FRONTEND_DIR}/bbv_count.cc
  ${DR_FRONTEND_HEADERS})

target_include_directories(dr_frontend PRIVATE
  "${SNIPER_FRONTEND_DIR}"
  "${CMAKE_SOURCE_DIR}/sift"
  "${CMAKE_SOURCE_DIR}/common/misc"
  "${CMAKE_SOURCE_DIR}/decoder_lib")    # decoder.h: the ISA record's values

# Plain signature, not PRIVATE/PUBLIC: configure_DynamoRIO_client() below calls
# target_link_libraries() the old way, and CMake refuses to mix the two forms on
# one target.
target_link_libraries(dr_frontend sift z)

# OUTPUT_NAME dr-frontend, not the target name: drrun loads libdr-frontend.so,
# which is what docs/ARM64.md and the tool config below both name.
set_target_properties(dr_frontend PROPERTIES
  OUTPUT_NAME dr-frontend
  CXX_STANDARD 11
  CXX_EXTENSIONS OFF
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

configure_DynamoRIO_client(dr_frontend)
foreach(_ext drmgr droption drreg drutil)
  use_DynamoRIO_extension(dr_frontend ${_ext})
endforeach()

# Config file telling drrun how to load the client.
#
# Keyed off CMAKE_SIZEOF_VOID_P rather than DynamoRIO's X64: that variable is not
# defined by the package at this scope, so a 64-bit build wrote a .drrun32 config
# and named the wrong file.  The compiler's pointer size is the thing actually
# being asked about, and it does not depend on DynamoRIO's internals.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_dr_config "${CMAKE_BINARY_DIR}/lib/dr-frontend.drrun64")
else()
  set(_dr_config "${CMAKE_BINARY_DIR}/lib/dr-frontend.drrun32")
endif()

# CLIENT_ABS with the path the build actually produced, written by
# file(GENERATE) so the generator expression resolves.
#
# It used to be CLIENT_REL=${INSTALL_CLIENTS_LIB}/${LIB_PFX}dr-frontend${LIB_EXT}
# -- three variables that DynamoRIO defines inside its own build, not here, so
# they all expanded to nothing and the file said "CLIENT_REL=/dr-frontend".
# CLIENT_REL is relative to the DynamoRIO installation anyway, and the client
# is not installed into it: it lives in this build directory.
file(GENERATE OUTPUT "${_dr_config}" CONTENT
"# dr-frontend tool config file
# DynamoRIO options: may as well optimize the bb lock
DR_OP=-nop_initial_bblock
# client tool path
CLIENT_ABS=$<TARGET_FILE:dr_frontend>
# client tool options
TOOL_OP=
")

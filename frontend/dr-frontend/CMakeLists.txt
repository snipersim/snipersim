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

find_package(DynamoRIO)
if(NOT DynamoRIO_FOUND)
  message(FATAL_ERROR
    "DynamoRIO not found.\n"
    "  Fetch it:  deps/sniper-deps --dynamorio fetch\n"
    "  then build it, and configure with -DDynamoRIO_DIR=<dynamorio>/build/cmake\n"
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
  ${SNIPER_FRONTEND_DIR}/bbv_count.cc
  ${DR_FRONTEND_HEADERS})

target_include_directories(dr_frontend PRIVATE
  "${SNIPER_FRONTEND_DIR}"
  "${CMAKE_SOURCE_DIR}/sift"
  "${CMAKE_SOURCE_DIR}/common/misc")

# Plain signature, not PRIVATE/PUBLIC: configure_DynamoRIO_client() below calls
# target_link_libraries() the old way, and CMake refuses to mix the two forms on
# one target.
target_link_libraries(dr_frontend sift z)

set_target_properties(dr_frontend PROPERTIES
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
file(WRITE  ${_dr_config} "# dr-frontend tool config file\n")
file(APPEND ${_dr_config} "# DynamoRIO options: may as well optimize the bb lock\n")
file(APPEND ${_dr_config} "DR_OP=-nop_initial_bblock\n")
file(APPEND ${_dr_config} "# client tool path\n")
file(APPEND ${_dr_config} "CLIENT_REL=${INSTALL_CLIENTS_LIB}/${LIB_PFX}dr-frontend${LIB_EXT}\n")
file(APPEND ${_dr_config} "# client tool options\n")
file(APPEND ${_dr_config} "TOOL_OP=\n")

# **********************************************************
# Copyright (c) 2010-2015 Google, Inc.    All rights reserved.
# Copyright (c) 2009-2010 VMware, Inc.    All rights reserved.
# **********************************************************

# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice,
#   this list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials provided with the distribution.
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

cmake_minimum_required(VERSION 2.6)

#include(CheckCXXCompilerFlag)
#CHECK_CXX_COMPILER_FLAG("-std=c++11" COMPILER_SUPPORTS_CXX11)
#CHECK_CXX_COMPILER_FLAG("-std=c++0x" COMPILER_SUPPORTS_CXX0X)
#if(COMPILER_SUPPORTS_CXX11)
#        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
#elseif(COMPILER_SUPPORTS_CXX0X)
#        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x")
#else()
#        message(STATUS "The compiler ${CMAKE_CXX_COMPILER} has no C++11 support. Please use a different C++ compiler.")
#endif()

message("SNIPER_HOME:" $ENV{SNIPER_HOME})
if(NOT DEFINED ENV{SNIPER_HOME})
    message(FATAL_ERROR "Please define SNIPER_HOME environment variable")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c\\+\\+11")

set(SNIPER_FRONTEND $ENV{SNIPER_HOME}/frontend)
set(SNIPER_SIFT $ENV{SNIPER_HOME}/sift)

include_directories(${SNIPER_FRONTEND} ${SNIPER_SIFT})

set(HEADER_FILES ${SNIPER_FRONTEND}/frontend.h ${SNIPER_FRONTEND}/frontend_threads.h ${SNIPER_FRONTEND}/frontend_defs.h ${SNIPER_FRONTEND}/bbv_count.h ${SNIPER_FRONTEND}/frontend_callbacks.h ${SNIPER_FRONTEND}/frontend_control.h ${SNIPER_FRONTEND}/frontend_options.h ${SNIPER_FRONTEND}/frontend_syscall.h ${SNIPER_FRONTEND}/frontend_utils.h)

#todo: include options and defs headers

#find_package(DynamoRIO)
#if (NOT DynamoRIO_FOUND)
#  message(FATAL_ERROR "DynamoRIO package required to build")
#endif(NOT DynamoRIO_FOUND)
#
add_library(dr-frontend SHARED
       dr_frontend.cc 
       ${SNIPER_FRONTEND}/bbv_count.cc 
       ${HEADER_FILES})

# sniper's libraries and others
find_library(libz z)
target_link_libraries(dr-frontend 
        $ENV{SNIPER_HOME}/sift/libsift.a
        ${libz})

configure_DynamoRIO_client(dr-frontend)
use_DynamoRIO_extension(dr-frontend drmgr)
use_DynamoRIO_extension(dr-frontend droption)
use_DynamoRIO_extension(dr-frontend drreg)
use_DynamoRIO_extension(dr-frontend drutil)
#use_DynamoRIO_extension(drltrace drwrap)
#use_DynamoRIO_extension(drltrace drx)
# We keep our shared libs in the lib dir, not the bin dir:
place_shared_lib_in_lib_dir(dr-frontend)

# ensure we rebuild if includes change
add_dependencies(dr-frontend api_headers)

# Provide a hint for how to use the client
if (NOT DynamoRIO_INTERNAL OR NOT "${CMAKE_GENERATOR}" MATCHES "Ninja")
  add_custom_command(TARGET dr-frontend
    POST_BUILD
    COMMAND ${CMAKE_COMMAND}
    ARGS -E echo "Usage: pass to drconfig or drrun: -t dr-frontend"
    VERBATIM)
endif ()

install_target(dr-frontend ${INSTALL_CLIENTS_LIB})

set(INSTALL_DRLTRACE_CONFIG ${INSTALL_CLIENTS_BASE})

if (X64)
  set(CONFIG ${PROJECT_BINARY_DIR}/dr-frontend.drrun64)
else (X64)
  set(CONFIG ${PROJECT_BINARY_DIR}/dr-frontend.drrun32)
endif (X64)

file(WRITE  ${CONFIG} "# dr-frontend tool config file\n")
file(APPEND ${CONFIG} "# DynamoRIO options: may as well optimize the bb lock\n")
file(APPEND ${CONFIG} "DR_OP=-nop_initial_bblock\n")
file(APPEND ${CONFIG} "# client tool path\n")
file(APPEND ${CONFIG} "CLIENT_REL=${INSTALL_CLIENTS_LIB}/${LIB_PFX}dr-frontend${LIB_EXT}\n")
file(APPEND ${CONFIG} "# client tool options\n")
file(APPEND ${CONFIG} "TOOL_OP=\n")

DR_install(FILES "${CONFIG}" DESTINATION ${INSTALL_DRLTRACE_CONFIG})
register_tool_file("dr-frontend")

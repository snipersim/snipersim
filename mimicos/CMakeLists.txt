# Userspace MimicOS.
#
# This is a *guest* program: it runs under simulation as a pinned native
# process, alongside the application, when general/enable_userspace_mimicos is
# set.  It is not part of the simulator, and it deliberately does not link
# against sniper_base -- no PinCRT, no legacy libstdc++ ABI, none of the
# simulator's compile contract applies to it.
#
# It used to be built by its own makefile, invoked by hand; testing/run_tests.py
# still documents "run 'make -C mimicos' first".  Building it here means it
# lands in the build directory with everything else and a second build
# configuration gets its own copy.

file(GLOB_RECURSE MIMICOS_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc")

add_executable(startup_mimicos
  startup_mimicos.cc
  ${MIMICOS_SOURCES})

target_include_directories(startup_mimicos PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/mm"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/misc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/include")

target_compile_options(startup_mimicos PRIVATE -Wall -Wextra)
target_link_libraries(startup_mimicos PRIVATE rt)

set_target_properties(startup_mimicos PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

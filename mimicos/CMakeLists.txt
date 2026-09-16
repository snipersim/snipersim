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

# Built non-PIE, and not by preference.
#
# The fault-path headers pin their own optimisation level with
# `#pragma GCC optimize ("O0")` -- linux_phase_work.h does it unguarded, and
# buddy.h and linux_buddy_anon.h do it behind MIMICOS_CALIBRATION_O0 -- so the
# instructions the simulator sees for an allocation match what those paths
# were calibrated against.  GCC's optimize pragma resets code generation for
# the region it covers, position independence included, so objects containing
# those functions carry absolute relocations and will not go into a PIE.
#
# This is a guest program, not a library, so nothing is lost by linking it
# fixed-position.  The alternative -- dropping the pragmas -- would change the
# instruction counts the fault paths were calibrated to produce.
set_target_properties(startup_mimicos PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_link_options(startup_mimicos PRIVATE -no-pie)

set_target_properties(startup_mimicos PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

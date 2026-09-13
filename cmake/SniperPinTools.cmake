# Pin-side targets: SIFT built once against PinCRT, and the pintools that use it.
#
# SIFT was compiled three times: once for the host, and twice against PinCRT --
# separately for the recorder and for the Pin frontend, from identical sources
# with identical flags, sharing nothing.  Each PinCRT copy was assembled by
# symlinking sift/*.cc, sift/*.h and common/misc/fixed_types.h into a gitignored
# directory inside the tool, because Pin's build rules only compile sources that
# live in the tool's own directory.  The same eight-line stanza appeared in three
# makefiles.
#
# CMake has no such restriction, so there is one PinCRT build here and both tools
# link it.

if(NOT Pin_FOUND)
  return()
endif()

# --- SIFT, PinCRT ABI -------------------------------------------------------
#
# Same source list as the host library; a different toolchain, so a different
# target.  Two builds of SIFT is the honest number: host and PinCRT really are
# incompatible ABIs.  Three was not.
add_library(sift_pincrt STATIC ${SIFT_SOURCES})

target_include_directories(sift_pincrt PUBLIC
  "${CMAKE_SOURCE_DIR}/sift"
  "${CMAKE_SOURCE_DIR}/common/misc")   # fixed_types.h, previously symlinked in

target_link_libraries(sift_pincrt PUBLIC Sniper::PinTool)

# Pin's flags are a complete command line of their own, including its own
# optimisation level and its own C++ runtime.  Do not layer CMake's on top.
# Keeps the name libsift.a -- mfile.py links it as -lsift -- but in its own
# directory, since the host build produces a libsift.a of the same name.
set_target_properties(sift_pincrt PROPERTIES
  OUTPUT_NAME sift
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pincrt"
  POSITION_INDEPENDENT_CODE ON
  CXX_STANDARD 17
  CXX_EXTENSIONS OFF)
target_compile_options(sift_pincrt PRIVATE ${SNIPER_PIN_NO_HOST_FLAGS})

# --- the Pin frontend -------------------------------------------------------
#
# A pintool, so a MODULE library: Pin loads it, nothing links against it.
add_library(pin_frontend MODULE
  "${CMAKE_SOURCE_DIR}/frontend/pin-frontend/pin_frontend.cc"
  "${CMAKE_SOURCE_DIR}/frontend/pin-frontend/globals.cc"
  "${CMAKE_SOURCE_DIR}/frontend/bbv_count.cc")

target_include_directories(pin_frontend PRIVATE
  "${CMAKE_SOURCE_DIR}/frontend"
  "${CMAKE_SOURCE_DIR}/sift"
  "${CMAKE_SOURCE_DIR}/common/misc")

# The kit defines TARGET_IA32E and HOST_IA32E but not TARGET_INTEL64, which is
# what frontend_syscall.tcc keys the 64-bit clone() argument layout off; without
# it neither arm of the #if is taken and tidptr is never declared.  The tools'
# own makefiles added this (pin-frontend/makefile.rules, recorder/makefile.sde.rules).
target_compile_definitions(pin_frontend PRIVATE TARGET_INTEL64)

target_link_libraries(pin_frontend PRIVATE
  sift_pincrt
  Sniper::PinTool
  ${Pin_TOOL_LIBS})

set_target_properties(pin_frontend PROPERTIES
  PREFIX ""
  SUFFIX "${Pin_TOOL_SUFFIX}"
  CXX_STANDARD 17
  CXX_EXTENSIONS OFF
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
target_compile_options(pin_frontend PRIVATE ${SNIPER_PIN_NO_HOST_FLAGS})

# --- the SDE SIFT recorder: deliberately not built here ---------------------
#
# It is an SDE tool, not a plain pintool, and Intel builds it with
# sift/recorder/mfile.py on top of the kit's own mbuild and build_kit modules.
#
# Reproducing that link from the probed makefile variables was tried and does not
# work.  sift/recorder/makefile.sde.rules -- the rules file the recorder's
# Makefile includes -- specifies
#     -lpinplay -lsde -lpinplay -lsde -lbz2 -lzlib
# but that path is not the one that builds this tool: the rule delegates to
# mfile.py, which links
#     -lpinplay -lpin -lzlib -lbz2 -lsift
# instead.  Building against the former produces a tool 2.6 MB larger with ten
# extra undefined Pin symbols, and Pin refuses to load it:
#
#     E: Unable to load .../sde_sift_recorder.so: dlopen failed: cannot locate
#        symbol "_ZN15LEVEL_PINCLIENT26PIN_StopApplicationThreadsEj"
#
# It links cleanly and fails at run time, which is the worst shape a build bug
# can take, so the recorder keeps the build Intel supports.  It still needs its
# own PinCRT libsift until it moves; sift_pincrt above is what it should consume
# when it does.

# Historical note.  It is an SDE tool rather than
# a plain pintool and Intel builds it with sift/recorder/mfile.py, on top of the
# kit's own mbuild and build_kit modules; reimplementing that in CMake would mean
# reimplementing SDE's tool linkage, which is exactly the kind of guesswork this
# rework is trying to remove.  It keeps its own build and now links the shared
# sift_pincrt above instead of a private copy.

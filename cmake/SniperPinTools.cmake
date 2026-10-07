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

# --- the SDE SIFT recorder -------------------------------------------------
#
# Built by Intel's sift/recorder/mfile.py, not by CMake.  It is an SDE tool
# rather than a plain pintool, and reproducing that link from the probed
# makefile variables was tried and fails: sift/recorder/makefile.sde.rules
# specifies -lsde, but that rules file is not what builds the tool -- it
# delegates to mfile.py, which links -lpin instead.  The result is 2.6 MB larger
# with ten extra undefined Pin symbols, links cleanly, and dies at run time with
#
#     dlopen failed: cannot locate symbol "PIN_StopApplicationThreads"
#
# So the recorder keeps the build Intel supports.  What it no longer keeps is its
# own private copy of SIFT: SNIPER_SIFT_INCLUDE and SNIPER_SIFT_LIBDIR point
# mfile.py at sift_pincrt above, so the symlink farm is not needed and SIFT is
# built once per ABI rather than once per tool.
if(TARGET Sniper::SdeTool)
  set(_recorder_dir "${CMAKE_SOURCE_DIR}/sift/recorder")
  set(_recorder_so "${_recorder_dir}/obj-${Pin_TARGET}/sde_sift_recorder${Pin_TOOL_SUFFIX}")

  # Kept in step with tool_sources[] in mfile.py, plus the headers beside them.
  file(GLOB _recorder_sources
       "${_recorder_dir}/*.cc" "${_recorder_dir}/*.h")

  add_custom_command(
    OUTPUT "${_recorder_so}"
    COMMAND "${CMAKE_COMMAND}" -E env
            "SDE_BUILD_KIT=${Pin_SDE_KIT}"
            # mfile.py only adds the kit's mbuild to sys.path when SDE_BUILD_KIT
            # is unset; with it set, PYTHONPATH has to supply mbuild and the
            # pinplay scripts.  makefile.sde.rules did the same.
            # The kit's own sde-example comes first so build_kit.py is the one shipped
            # with the kit being built against.  pinplay-tools carries a vendored copy
            # frozen at an older SDE, and it knows nothing of the 10.x layout.
            "PYTHONPATH=${Pin_SDE_KIT}/pinkit/sde-example:${CMAKE_SOURCE_DIR}/pinplay-tools/pinplay-scripts:${Pin_SDE_KIT}/pinkit/sde-example/mbuild"
            "SNIPER_SIFT_INCLUDE=${CMAKE_SOURCE_DIR}/sift:${CMAKE_SOURCE_DIR}/common/misc"
            "SNIPER_SIFT_LIBDIR=$<TARGET_FILE_DIR:sift_pincrt>"
            "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
            # From SDE 10.x the kit compiles through pin-gcc, which otherwise
            # execs whatever "gcc" is on PATH -- not the compiler this build was
            # configured with.  Ignored by kits that have no wrapper.
            "PIN_WRAPPER_GCC=${CMAKE_C_COMPILER}"
            "PYTHONDONTWRITEBYTECODE=1"
            "${Python3_EXECUTABLE}" ./mfile.py --host-cpu ${Pin_TARGET}
    WORKING_DIRECTORY "${_recorder_dir}"
    # The tool's own sources have to be listed: mfile.py is opaque to CMake, so
    # without them an edit to the recorder rebuilds nothing and the stale .so is
    # silently kept.
    DEPENDS sift_pincrt ${_recorder_sources} "${_recorder_dir}/mfile.py"
    COMMENT "Building sde_sift_recorder (Intel mfile.py, shared sift_pincrt)"
    VERBATIM)

  add_custom_target(sde_sift_recorder ALL DEPENDS "${_recorder_so}")
endif()

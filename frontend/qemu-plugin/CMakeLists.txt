# The QEMU frontend: a TCG plugin loaded into a stock QEMU with -plugin.
#
# Unlike the Pin side there is nothing exotic here -- it is an ordinary host-ABI
# shared object that links libsift.  It must NOT pick up sniper_common, which
# drags in libtorch: the plugin lives inside QEMU's process and has no business
# loading the simulator's dependencies.

# deps/sniper-deps builds QEMU and links it in at qemu/; the plugin headers come
# from that same tree, so the API version the plugin is compiled against always
# matches the QEMU it will be loaded into.  That is not optional: QEMU rejects
# plugins outside its supported API range outright.
set(SNIPER_QEMU_HOME "${CMAKE_SOURCE_DIR}/qemu" CACHE PATH
    "QEMU built with --enable-plugins")

find_path(SNIPER_QEMU_PLUGIN_INCLUDE
          NAMES qemu-plugin.h
          HINTS "${SNIPER_QEMU_HOME}/include/plugins"   # QEMU >= 11
                "${SNIPER_QEMU_HOME}/include/qemu"      # QEMU <= 10
          NO_DEFAULT_PATH)

if(NOT SNIPER_QEMU_PLUGIN_INCLUDE)
  message(FATAL_ERROR
    "QEMU plugin headers not found under ${SNIPER_QEMU_HOME}.\n"
    "  Acquire it first:  deps/sniper-deps --qemu fetch\n"
    "  or point -DSNIPER_QEMU_HOME at a QEMU built with --enable-plugins,\n"
    "  or turn the frontend off with -DSNIPER_ENABLE_QEMU=OFF.")
endif()

# The plugin API passes GLib types (qemu-plugin.h includes glib.h).  CMake's
# own messages ("Could NOT find PkgConfig (missing: PKG_CONFIG_EXECUTABLE)")
# do not say which packages are missing.
find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(GLIB2 IMPORTED_TARGET glib-2.0)
endif()
if(NOT PkgConfig_FOUND OR NOT GLIB2_FOUND)
  message(FATAL_ERROR
    "The QEMU front end needs GLib's headers, found through pkg-config.  Install "
    "pkgconf and libglib2.0-dev on Debian and Ubuntu, pkgconf and glib2-devel on "
    "Fedora and RHEL; or build without QEMU=ON.")
endif()

# riscv_isa*.cc: the generated RISC-V instruction tables, shared with Sniper's
# decoder, so the plugin knows which instructions are branches and which need
# their whole memory footprint recorded.
add_library(qemu_frontend MODULE sniper_qemu.cc ame_backend.cc acc_backend.cc pacsim_qemu.cc
  "${CMAKE_SOURCE_DIR}/decoder_lib/riscv_isa.cc" "${CMAKE_SOURCE_DIR}/decoder_lib/riscv_isa_gen.cc")

target_include_directories(qemu_frontend PRIVATE
  "${SNIPER_QEMU_PLUGIN_INCLUDE}"
  "${CMAKE_SOURCE_DIR}/sift"
  "${CMAKE_SOURCE_DIR}/common/misc"
  # for dl_isa: the ISA numbering RecOtherISAChange carries
  "${CMAKE_SOURCE_DIR}/decoder_lib"
  # sim_api.h: the magic commands (SIM_CMD_ROI_START, ...)
  "${CMAKE_SOURCE_DIR}/include")

# sniper_base, not sniper_common: the language and ABI contract, without the
# simulator's libraries.
# XED so the plugin can answer "is this a branch?" the same way Sniper's own
# decoder does.  Disagreeing is not a fidelity question: Sniper asserts on it.
target_link_libraries(qemu_frontend PRIVATE sniper_base sift z PkgConfig::GLIB2 Sniper::Xed)

set_target_properties(qemu_frontend PROPERTIES
  OUTPUT_NAME sniper-qemu
  PREFIX "lib"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

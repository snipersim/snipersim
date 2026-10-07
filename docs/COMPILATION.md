How to compile an application to run under Sniper
=================================================

All options are defined in ${SNIPER_BUILD_DIR}/config/buildconf.<format>,
where SNIPER_BUILD_DIR is the build directory you want to compile against --
${SNIPER_ROOT}/build/default unless you chose another one.  Each build
directory has its own, so an application built against a debug build and one
built against a release build do not have to share flags.

<format> is:

* sh: for inclusion in shell-based build scripts:
    source ${SNIPER_BUILD_DIR}/config/buildconf.sh

* makefile: for inclusion in make-based builds:
    include ${SNIPER_BUILD_DIR}/config/buildconf.makefile


Single-threaded and Pthread applications
----------------------------------------

* No source code modifications necessary
* Link with the SNIPER_LDFLAGS options
* Run with LD_LIBRARY_PATH set to SNIPER_LD_LIBRARY_PATH


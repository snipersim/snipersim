#!/usr/bin/env python3
# -*- python -*-

# Copyright (C) 2004-2023 Intel Corporation.
# SPDX-License-Identifier: MIT
#

import sys
import os
import re
import copy
import inspect
import shutil
import platform

# set the SDE_BUILD_KIT environment variable to point to the root directory
# containing the sde package.
# addpend the directories containing the mbuild package and the build_kit.py file
# to the python path

if not 'SDE_BUILD_KIT' in os.environ:
    dir = os.path.dirname(os.path.abspath(__file__))
    sde_bk_dir = os.path.dirname(os.path.dirname(os.path.dirname(dir)))
    os.environ['SDE_BUILD_KIT'] = sde_bk_dir
    sys.path.append(os.path.join(sde_bk_dir, 'pinkit', 'sde-example'))
    sys.path.append(os.path.join(sde_bk_dir, 'pinkit', 'sde-example', 'mbuild'))

import mbuild
import build_kit


def add_link_libs(env):
    # Support PIN CRT link
    if env.on_linux():
        env['LIBS'] += ' -lpinplay -lpin -lzlib -lbz2 -lsift'
        # Add internal libs if exists
        if os.path.exists(os.path.join(pinplay_link_dir,'libxml2'+env['LIBEXT'])):
            env['LIBS'] += ' -larchxml -lxml2 '

    elif env.on_windows():
        env['LIBS'] += ' libpinplay%(LIBEXT)s'
        env['LIBS'] += ' bz2%(LIBEXT)s'
        env['LIBS'] += ' zlib%(LIBEXT)s'
        if os.path.exists(os.path.join(pinplay_link_dir,'xml2'+env['LIBEXT'])):
            # Add internal libs if exists
            env['LIBS'] += ' xml2%(LIBEXT)s'
            env['LIBS'] += ' libarchxml%(LIBEXT)s'

    else:
        mbuild.die('no supported OS')

env = mbuild.env_t()

build_kit.early_init(env,build_kit=True)

if env.on_windows():
   # Add clang tools definitions
   env['clang-cl']=True
   clang_which = shutil.which('clang-cl.exe')
   if clang_which != None and clang_which != '':
       clang_path = clang_which[:clang_which.find('clang-cl.exe')]
       compiler_path = '"'+os.path.join(clang_path,'clang-cl.exe')+'"'
       linker_path = '"'+os.path.join(clang_path,'lld-link.exe')+'"'
       mbuild.msgb("COMPILER_PATH",compiler_path)
       mbuild.msgb("LINKER_PATH",linker_path)
   env.parse_args({'shared':True,'cc':compiler_path,'cxx':compiler_path,'linker':linker_path})
else:
   env.parse_args({'shared':True})

# gcc_spec points at a spec file that only exists when Pin is built from source;
# a binary kit does not ship one, so ask for it to be left alone.  Kits older
# than SDE 10.x have no such parameter -- and no such behaviour to switch off.
env['gcc_spec'] = False
if 'gcc_spec' in inspect.signature(build_kit.late_init).parameters:
    build_kit.late_init(env, gcc_spec=False)
else:
    build_kit.late_init(env)

if 'clean' in env['targets']:
    mbuild.remove_tree(env['build_dir'])
    sys.exit(0)
env['build_dir'] = 'obj-{0:s}'.format(env['pin_arch'])
mbuild.cmkdir(env['build_dir'])

# Determine architecture
if env['host_cpu'] == 'x86-64':
    env['arch'] = 'intel64'
else:
    env['arch'] = 'ia32'

# Set include and link dirs
pinplay_include_dir = os.path.join(os.environ['SDE_BUILD_KIT'],'pinkit','pinplay','include')
instlib_include_dir = os.path.join(os.environ['SDE_BUILD_KIT'],'pinkit','source',
                                                                'tools','InstLib')
pinplay_link_dir = os.path.join(os.environ['SDE_BUILD_KIT'],'pinkit','pinplay',env['arch'])
example_link_dir = os.path.join(os.environ['SDE_BUILD_KIT'], 'pinkit','sde-example',
                                                             'lib',env['arch'])
pin_lib_dir = os.path.join(os.environ['SDE_BUILD_KIT'],env['arch'],'pin_lib')
# PinCRT, the private libc that tools link instead of the system one, moved in
# SDE 10.x: pinkit/<arch>/runtime/pincrt became pinkit/<arch>/pinrt/lib, and the
# kit now drives the build through the pin-gcc/pin-g++ wrappers in
# pinkit/<arch>/pinrt/bin rather than through raw gcc with hand-assembled flags.
# Those wrappers pass -nostdinc and the PinCRT header and library paths; without
# them the PinCRT libc++ headers #include_next their way into /usr/include and
# the compile fails on redeclarations.  Both layouts are recognised so a kit
# older than 10.x still builds.
_pinrt_dir = os.path.join(os.environ['SDE_BUILD_KIT'],'pinkit',env['arch'],'pinrt')
_pinrt_lib_dir = os.path.join(_pinrt_dir,'lib')
_legacy_crt_dir = os.path.join(os.environ['SDE_BUILD_KIT'],'pinkit',env['arch'],
                               'runtime','pincrt')
if os.path.isdir(_pinrt_lib_dir):
    pin_crt_dir = _pinrt_lib_dir
    pinrt_bin_dir = os.path.join(_pinrt_dir,'bin')
else:
    pin_crt_dir = _legacy_crt_dir
    pinrt_bin_dir = None
have_pin_crt = os.path.isdir(pin_crt_dir)

def crt_objs(*names):
    """CRT objects to prepend to a link, empty on kits that have no PinCRT."""
    if not have_pin_crt:
        return []
    return [mbuild.join(pin_crt_dir, n) for n in names
            if os.path.exists(mbuild.join(pin_crt_dir, n))]

####################################################
# Start to collect info for the build
#   by adding stuff to the dag.
####################################################
dag = mbuild.dag_t(env=env)


# Tools
tools = ['sde_sift_recorder']

# Standalone programs
programs = {}

# Always support pinplay
mbuild.msgb('PINPLAY IS BEING USED')
env.add_define('PINPLAY')
env.add_include_dir(pinplay_include_dir)
env.add_include_dir(instlib_include_dir)
# SIFT headers and library.
#
# By default these come from ./sift, a directory of symlinks to the real sources
# that this tool's makefile creates at build time, and from a copy of libsift
# built inside it.  That is why SIFT used to be compiled three times: once for
# the host and once privately for each PinCRT tool.
#
# SNIPER_SIFT_INCLUDE and SNIPER_SIFT_LIBDIR let the caller point at a single
# shared PinCRT build instead.  Unset, the historical behaviour is unchanged.
_sift_include = os.environ.get('SNIPER_SIFT_INCLUDE', './sift')
for _d in _sift_include.split(os.pathsep):
    if _d:
        env.add_include_dir(_d)
env.add_link_dir(pinplay_link_dir)
env.add_link_dir(example_link_dir)
add_link_libs(env)

# Support PIN CRT
env.add_link_dir(pin_lib_dir)
if have_pin_crt:
    env.add_link_dir(pin_crt_dir)
env.add_link_dir(os.environ.get('SNIPER_SIFT_LIBDIR', './sift/obj-intel64'))
if env.on_linux():
    env['LINKFLAGS'] += ' -Wl,--hash-style=sysv '
    env['LINKFLAGS'] += ' -Wl,--rpath,\$ORIGIN/../../../../%(arch)s/pin_lib:\$ORIGIN/../../../../%(arch)s/xed_lib:\$ORIGIN/pin_lib:\$ORIGIN/xed_lib'

# Tools sources
tool_sources = {}
tool_sources['sde_sift_recorder'] =  ['bbv_count.cc', 'emulation.cc','globals.cc','papi.cc','pinboost_debug.cc','recorder_base.cc','recorder_control.cc','sift_recorder.cc','syscall_modeling.cc','threads.cc','trace_rtn.cc' ]

# Programs sources
programs_sources = {}

# The wrapper compilers know where PinCRT lives; plain gcc does not.  Only newer
# kits ship them, so older kits keep using whatever mbuild picked.
if pinrt_bin_dir and os.path.exists(os.path.join(pinrt_bin_dir,'pin-g++')):
    env['toolchain']    = pinrt_bin_dir + os.sep
    env['CC_COMPILER']  = 'pin-gcc'
    env['CXX_COMPILER'] = 'pin-g++'
    env['LINKER']       = 'pin-g++'

# Build tools
for tool in tools:
    objs = []
    for s in tool_sources[tool]:
        if not os.path.exists(s):
            mbuild.msgb('SKIP', 'tool %s was not found' %(s))
            continue
        if s.endswith('.cpp'):
            cmd = dag.add(env, env.cxx_compile( s ))
        elif s.endswith('.cc'):
            cmd = dag.add(env, env.cxx_compile( s ))
        elif s.endswith('.c'):
            cmd = dag.add(env, env.cc_compile( s ))
        elif s.endswith('.s'):
            cmd = dag.add(env, env.cc_assemble( s ))
        else:
            mbuild.die("Do not know hot to compile file: %s" % (s))
        objs.extend(cmd.targets)

    # Support PIN CRT - add crtbeginS and crtendS objects
    if not env.on_windows():
        all_objs = crt_objs('crtbeginS.o')
        all_objs.extend(objs)
        if not env.on_mac():
            all_objs.extend(crt_objs('crtendS.o'))
    else:
        all_objs = crt_objs('crtbeginS.obj')
        all_objs.extend(objs)
    objs = all_objs

    toolname = tool + "%(pintool_suffix)s"

    cmd2 = dag.add(env, env.dynamic_lib(objs, toolname, relocate=True))

# Create environment for standalone as well
# replace pin standalone lib
env_sa = copy.deepcopy(env)
env_sa['LIBS'] = env_sa['LIBS'].replace(' -lpin ',' -lsapin ')

# Build programs
for program in programs:
    objs = []
    for s in programs_sources[program]:
        if not os.path.exists(s):
            mbuild.msgb('SKIP', 'tool %s was not found' %(s))
            continue
        if s.endswith('.cpp'):
            cmd = dag.add(env_sa, env.cxx_compile( s ))
        elif s.endswith('.c'):
            cmd = dag.add(env_sa, env.cc_compile( s ))
        elif s.endswith('.s'):
            cmd = dag.add(env_sa, env.cc_assemble( s ))
        else:
            mbuild.die("Do not know hot to compile file: %s" % (s))
        objs.extend(cmd.targets)

    # Support PIN CRT - add crtbegin and crtend objects for standalone
    if not env_sa.on_windows():
        all_objs = crt_objs('crtbegin.o')
        all_objs.extend(objs)
        if not env_sa.on_mac():
            all_objs.extend(crt_objs('crtend.o'))
    else:
        all_objs = crt_objs('crtbegin.obj')
        all_objs.extend(objs)
    objs = all_objs

    target = program + '.exe'
    cmd2 = dag.add(env_sa, env_sa.link(objs, target, relocate=True))


####################################################
# Do the build based on the dag
####################################################
work_queue = mbuild.work_queue_t(env['jobs'])
okay = work_queue.build(dag)
if not okay:
    mbuild.die("build failed")
mbuild.msgb("SUCCESS")

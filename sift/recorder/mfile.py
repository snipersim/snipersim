#!/usr/bin/env python2
# Copyright (C) 2022 Intel Corporation
# SPDX-License-Identifier: BSD-3-Clause
# -*- python -*-
#BEGIN_LEGAL
#BSD License
#
#Copyright (c)2022 Intel Corporation. All rights reserved.
#
#Redistribution and use in source and binary forms, with or without modification,
# are permitted provided that the following conditions are met:
#
#1. Redistributions of source code must retain the above copyright notice,
#   this list of conditions and the following disclaimer.
#
#2. Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials provided with the distribution.
#
#3. Neither the name of the copyright holder nor the names of its contributors
#   may be used to endorse or promote products derived from this software without
#   specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED.
# IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
# INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
# LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
# OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#END_LEGAL
import sys
import os
import re
import copy

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
    elif env.on_windows():
        env['LIBS'] += ' libpinplay%(LIBEXT)s'
        env['LIBS'] += ' bz2%(LIBEXT)s'
        env['LIBS'] += ' zlib%(LIBEXT)s'
    else:
        mbuild.die('no supported OS')

env = mbuild.env_t()

build_kit.early_init(env,build_kit=True)
env.parse_args({'shared':True})
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

# Set compiler
try:
    env['CC'] = os.environ['CC']
except KeyError:
    pass
try:
    env['CXX'] = os.environ['CXX']
except KeyError:
    pass

# Set include and link dirs
pinplay_include_dir = os.path.join(os.environ['SDE_BUILD_KIT'],'pinkit','pinplay','include')
instlib_include_dir = os.path.join(os.environ['SDE_BUILD_KIT'],'pinkit','source',
                                                                'tools','InstLib')
pinplay_link_dir = os.path.join(os.environ['SDE_BUILD_KIT'],'pinkit','pinplay',env['arch'])
example_link_dir = os.path.join(os.environ['SDE_BUILD_KIT'], 'pinkit','sde-example',
                                                             'lib',env['arch'])
pin_lib_dir = os.path.join(os.environ['SDE_BUILD_KIT'],env['arch'],'pin_lib')
pin_crt_dir = os.path.join(os.environ['SDE_BUILD_KIT'],'pinkit',env['arch'],'runtime','pincrt')

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
env.add_include_dir('./sift')
env.add_link_dir(pinplay_link_dir)
env.add_link_dir(example_link_dir)
add_link_libs(env)

# Support PIN CRT
env.add_link_dir(pin_lib_dir)
env.add_link_dir(pin_crt_dir)
env.add_link_dir('./sift/obj-intel64')
#env.add_link_dir('../obj-intel64') # for libsift.a
if env.on_linux():
    env['LINKFLAGS'] += ' -Wl,--hash-style=sysv '
    env['LINKFLAGS'] += ' -Wl,--rpath,\$ORIGIN/../../../../%(arch)s/pin_lib:\$ORIGIN/../../../../%(arch)s/xed_lib:\$ORIGIN/pin_lib:\$ORIGIN/xed_lib'
'pinplay-branch-predictor', 'pinplay-debugger', 'pinplay-driver', 'pinplay_controller', 'pinplay_debugtrace', 'pinplay_isimpoint', 'pinplay_strace', 'pinplayatrace'
# Tools sources
tool_sources = {}
tool_sources['sde_sift_recorder'] =  ['bbv_count.cc', 'emulation.cc','globals.cc','papi.cc','pinboost_debug.cc','recorder_base.cc','recorder_control.cc','sift_recorder.cc','syscall_modeling.cc','threads.cc','trace_rtn.cc' ]

# Build tools
for tool in tools:
    objs = []
    for s in tool_sources[tool]:
        if not os.path.exists(s):
            mbuild.msgb('SKIP', 'tool %s was not found' %(s))
            continue
        if s.endswith('.cpp'):
            cmd = dag.add(env, env.cxx_compile( s ))
        if s.endswith('.cc'):
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
        all_objs = [mbuild.join(pin_crt_dir,'crtbeginS.o')]
        all_objs.extend(objs)
        if not env.on_mac():
            all_objs.append(mbuild.join(pin_crt_dir,'crtendS.o'))
    else:
        all_objs = [mbuild.join(pin_crt_dir,'crtbeginS.obj')]
        all_objs.extend(objs)
    objs = all_objs

    toolname = tool #+ "%(pintool_suffix)s"

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
        all_objs = [mbuild.join(pin_crt_dir,'crtbegin.o')]
        all_objs.extend(objs)
        if not env_sa.on_mac():
            all_objs.append(mbuild.join(pin_crt_dir,'crtend.o'))
    else:
        all_objs = [mbuild.join(pin_crt_dir,'crtbegin.obj')]
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
#mbuild.msgb("SUCCESS")

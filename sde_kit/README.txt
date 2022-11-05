Copyright (C) 2004-2021 Intel Corporation.

This software and the related documents are Intel copyrighted materials, and your
use of them is governed by the express license under which they were provided to
you ("License"). Unless the License provides otherwise, you may not use, modify,
copy, publish, distribute, disclose or transmit this software or the related
documents without Intel's prior written permission.

This software and the related documents are provided as is, with no express or
implied warranties, other than those that are expressly stated in the License.

Documentation:

  http://www.intel.com/software/sde

Support is via Intel Community Forums:

  New-instruction related questions and Intel(R) SDE usage questions:
 
  https://community.intel.com/t5/Intel-ISA-Extensions/bd-p/isa-extensions


==============================================================

Linux Notes:

 RH systems: You must turn off SELinux to allow pin to work. Put
 "SELINUX=disabled" in /etc/sysconfig/selinux

 Ubuntu systems: Need to disable yama once, as root:
   $ echo 0 > /proc/sys/kernel/yama/ptrace_scope

 To use the debugging support, you must use gdb 7.8 or later.

==============================================================

Windows Notes:

 Winzip adds executable permissions to every file. Cygwin users must
 do a "chmod -R +x ." in the unpacked kit directory.

 To use the debugging support you must install the VSIX package from our
 download page and use it with MSVS2017 (no support yet to VS2019).

==============================================================

Mac OS X notes:

 Intel SDE is using the MACH taskport APIs. By default, when trying 
 to use these APIs, user-authentication is required once per a GUI 
 session. In order to allow PIN/SDE run without this authentication 
 you need to disable it. This is done by configuring the machine 
 to auto-confirm takeover of the process as described in SDE web page
 in the system configuration section.

 The debugger connection support does not work yet on Mac OSX.

# Copyright (C) 2021-2021 Intel Corporation.
# 
# This software and the related documents are Intel copyrighted materials, and your
# use of them is governed by the express license under which they were provided to
# you ("License"). Unless the License provides otherwise, you may not use, modify,
# copy, publish, distribute, disclose or transmit this software or the related
# documents without Intel's prior written permission.
# 
# This software and the related documents are provided as is, with no express or
# implied warranties, other than those that are expressly stated in the License.

The external build kit is currently tested and support only on Linux and Windows.

Requirements:
--------------
Linux:
    GNU make
    One of GCC versions: 8.3 9.1 10.1
    GNU Binutils 2.29

Windows:
    GNU make
    Microsoft Visual Studio 2019

To build all the examples for 64 bits run:
% make 

For 32 bits run:
% make TARGET=ia32

Building the examples on Windows requires running with the compiler environment
for the target. This means that you need to set the environment before calling make.

By default Intel (R) SDE looks for its tools in the 'arch' directory in
the kit. This means the ia32 or intel64 directories in the kit's root.

Running your built tool with SDE can be done by copying the tool to the
corresponsnding 'arch' directory or by using the -t64 or -t32 with full path
to the tool. For example:
% sde64 -skx -t64 ${PWD}/obj-intel64/example.so -- <app>

Please note that if you are using sde64 binary on Linux, then it is enough to build
the 64 bits tool, and if you are using sde binary you need to build and copy 
both 32 and 64 bits tools. 
% cp obj-ia32/example.so <KIT>/ia32
% cp obj-intel64/example.so <KIT>/intel64
% sde -skx -t example.so -- <app>

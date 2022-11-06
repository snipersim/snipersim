#BEGIN_LEGAL
#
#Copyright (c) 2018 Intel Corporation
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#  
#END_LEGAL


To build the examples, a relatively recent version of python 2.7 is required.

================================
STATIC LIBRARY XED BUILD:
================================

  Linux or Mac:

    % ./mfile.py

  Windows:

   % C:/python27/python mfile.py

================================
DYNAMIC  LIBRARY XED BUILD:
================================

If you have a a shared-object (or DLL build on windows) you must also include
"--shared" on the command line:

  Linux or Mac:

    % ./mfile.py --shared

  Windows:

   % C:/python27/python mfile.py --shared
 
Add "--help" (no quotes) for more build options.

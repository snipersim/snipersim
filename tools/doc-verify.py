#!/usr/bin/env python3
"""Extract the '# verify:' blocks of a document and emit one bash script that
runs them in order (the env block sourced into the shell, every other block in
a subshell), printing a banner and the exit status per block, and exiting non-zero
if any block failed.  Usage:
  doc-verify.py DOC.md [--long] > run.sh"""
import pathlib
import re, sys
doc = sys.argv[1]; want_long = '--long' in sys.argv
text = pathlib.Path(doc).read_text()
blocks = re.findall(r'```bash\n(# verify:[^\n]*)\n(.*?)```', text, re.S)
out = ['#!/bin/bash', 'export PS4="+ "', 'fail=0']
for head, body in blocks:
    name = head.split('# verify:', 1)[1].strip()
    is_long = name.startswith('long:')
    if is_long != want_long and name != 'env':      # the env block is sourced by both passes
        continue
    out.append('echo "=== BLOCK %s ==="' % name)
    if name == 'env':
        out.append(body)
    else:
        # A build block stops at its first failing command: its last line is a
        # listing that succeeds whether or not the build did, so a failed make
        # (a dependency that could not be built) passed as a built tree.  The
        # other blocks print the exit codes they expect (a guest's 100) and run on.
        strict = 'set -e\n' if name == 'build' else ''
        out.append('( set -x\n%s%s\n); st=$?; [ $st -eq 0 ] || fail=1; echo "=== END %s status=$st ==="' % (strict, body, name))
out.append('exit $fail      # non-zero when any block failed, so make doc-verify fails with it')
print('\n'.join(out))

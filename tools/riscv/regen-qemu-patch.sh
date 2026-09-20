#!/bin/bash
# Regenerate tools/riscv/qemu-11.1.1-ime.patch from a pristine QEMU 11.1.1 tree and
# the patched one: tools/riscv/regen-qemu-patch.sh <pristine tree> <patched tree>
# (the pristine tree is deps' unpatched checkout, qemu-src; the patched one qemu-riscv).
# Keeps the patch's header text, rewrites every hunk, and checks the result applies.
set -e
PRISTINE=${1:?pristine tree}; PATCHED=${2:?patched tree}; ROOT=$(cd "$(dirname "$0")/../.." && pwd); OUT=$ROOT/tools/riscv/qemu-11.1.1-ime.patch
HDR=$(mktemp); sed -n '1,/^--- /p' "$OUT" | sed '$d' > "$HDR"
# every file the patch already touches, plus any other file that differs between the trees
FILES=$( { grep '^+++ b/' "$OUT" | sed 's|^+++ b/||'; diff -rq --exclude=build --exclude=.git "$PRISTINE" "$PATCHED" | grep '^Files ' | awk '{print $2}' | sed "s|^$PRISTINE/||"; } | sort -u)
NEW=$(mktemp); cat "$HDR" > "$NEW"
for f in $FILES; do
   if [ -f "$PRISTINE/$f" ]; then diff -u --label "a/$f" --label "b/$f" "$PRISTINE/$f" "$PATCHED/$f" >> "$NEW" || true
   else diff -u --label /dev/null --label "b/$f" /dev/null "$PATCHED/$f" >> "$NEW" || true; fi
done
CHK=$(mktemp -d); for f in $FILES; do [ -f "$PRISTINE/$f" ] && mkdir -p "$CHK/$(dirname $f)" && cp "$PRISTINE/$f" "$CHK/$f"; done
( cd "$CHK" && git init -q . && git apply --check "$NEW" ) && mv "$NEW" "$OUT" && echo "wrote $OUT ($(grep -c '^+++ ' "$OUT") files)"
rm -rf "$CHK" "$HDR"

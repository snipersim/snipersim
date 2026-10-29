#!/bin/bash
# Regenerate tools/riscv/qemu-11.1.1-ime.patch from a pristine QEMU 11.1.1 tree and
# the patched one: tools/riscv/regen-qemu-patch.sh <pristine tree> <patched tree> [out]
# (out defaults to the committed patch; give another path to compare without
# touching the tree, as the RISC-V document's verify block does)
# (the pristine tree is deps' unpatched checkout, qemu-src; the patched one qemu-riscv).
# Keeps the patch's header text, rewrites every hunk, and checks the result applies.
set -e
PRISTINE=${1:?pristine tree}; PATCHED=${2:?patched tree}; ROOT=$(cd "$(dirname "$0")/../.." && pwd); SRC=$ROOT/tools/riscv/qemu-11.1.1-ime.patch; OUT=${3:-$SRC}
HDR=$(mktemp); sed -n '1,/^--- /p' "$SRC" | sed '$d' > "$HDR"
# every file the patch already touches, plus any other file that differs between the trees
FILES=$( { grep '^+++ b/' "$SRC" | sed 's|^+++ b/||'; diff -rq --exclude=build --exclude=.git "$PRISTINE" "$PATCHED" | grep '^Files ' | awk '{print $2}' | sed "s|^$PRISTINE/||"; } | sort -u)
# The patched tree also carries tools/qemu-11.1.1-plugin-wait.patch,
# tools/qemu-11.1.1-plugin-counters.patch and tools/qemu-11.1.1-plugin-syscall.patch
# (the plugin wait call, counter callback and plugin system call, patches of
# their own that deps/sniper-deps applies after this one,
# and to the x86-64 and AArch64 builds too): they are taken back out of a copy
# of the patched files, the last applied first, so that this patch stays the
# IME one.  A tree without them is used as it is.  One that does not come
# out must be one the tree lacks -- it, and any other missing, then apply to
# the patched tree in their order -- or the tree carries another version of
# it (the patch changed since, or the tree was edited over it), which the IME
# patch would take in: an error, not a silent skip.
PLUGIN_FILES=$(cat $ROOT/tools/qemu-11.1.1-plugin-*.patch 2>/dev/null | grep '^+++ b/' | sed 's|^+++ b/||')
WORK=$(mktemp -d); for f in $FILES $PLUGIN_FILES; do [ -f "$PATCHED/$f" ] && mkdir -p "$WORK/$(dirname $f)" && cp "$PATCHED/$f" "$WORK/$f"; done
ORIG=$(mktemp -d); cp -r "$WORK/." "$ORIG/"; ABSENT=
for P in $ROOT/tools/qemu-11.1.1-plugin-syscall.patch $ROOT/tools/qemu-11.1.1-plugin-counters.patch $ROOT/tools/qemu-11.1.1-plugin-wait.patch; do
   [ -f "$P" ] || continue
   if ( cd "$WORK" && git apply -R --check "$P" 2>/dev/null ); then ( cd "$WORK" && git apply -R "$P" ); echo "took out $P"
   else ABSENT="$P $ABSENT"; fi
done
for P in $ABSENT; do
   if ( cd "$ORIG" && git apply "$P" 2>/dev/null ); then echo "not in the patched tree: $P"
   else echo "error: $P does not come out of the patched tree, nor is it missing from it: the tree has another version of it" >&2
        rm -rf "$WORK" "$ORIG" "$HDR"; exit 1; fi
done
rm -rf "$ORIG"
NEW=$(mktemp); cat "$HDR" > "$NEW"
for f in $FILES; do
   if [ -f "$PRISTINE/$f" ]; then diff -u --label "a/$f" --label "b/$f" "$PRISTINE/$f" "$WORK/$f" >> "$NEW" || true
   else diff -u --label /dev/null --label "b/$f" /dev/null "$WORK/$f" >> "$NEW" || true; fi
done
CHK=$(mktemp -d); for f in $FILES; do [ -f "$PRISTINE/$f" ] && mkdir -p "$CHK/$(dirname $f)" && cp "$PRISTINE/$f" "$CHK/$f"; done
( cd "$CHK" && git init -q . && git apply --check "$NEW" ) && mv "$NEW" "$OUT" && echo "wrote $OUT ($(grep -c '^+++ ' "$OUT") files)"
rm -rf "$CHK" "$HDR" "$WORK"

#!/bin/sh
# quick tests of the Lite archiver: a mixed folder (Unicode names, empty files, an empty folder, a file over several pieces),
# restore all, restore one, check; then 100 damaged copies: never a wrong file written, never a crash.
X=${1:-./xign-lite}; T=$(mktemp -d); set -e
mkdir -p "$T/in/docs/sub" "$T/in/Fotos ñ 日本" "$T/in/empty"
seq 1 300000 > "$T/in/docs/numbers.txt"; head -c 2500000 /dev/urandom > "$T/in/docs/sub/random.bin"; : > "$T/in/docs/zero.txt"
cp "$(dirname "$0")/../src/"*.c "$T/in/Fotos ñ 日本/"; echo hi > "$T/in/one.txt"
"$X" a -P 1 -t 2 "$T/a.xla" "$T/in" > /dev/null
"$X" t "$T/a.xla" > /dev/null
"$X" x "$T/a.xla" "$T/out" > /dev/null
diff -r "$T/in" "$T/out/in" && echo "PASS restore all: every file exact"
"$X" e "$T/a.xla" "in/docs/numbers.txt" "$T/one" > /dev/null && cmp "$T/one" "$T/in/docs/numbers.txt" && echo "PASS restore one"
set +e
wrong=0; crash=0; i=0
while [ $i -lt 100 ]; do
  i=$((i+1)); cp "$T/a.xla" "$T/f.xla"; sz=$(wc -c < "$T/f.xla")
  off=$(( ($(od -An -N4 -tu4 /dev/urandom) % sz) )); printf '\377' | dd of="$T/f.xla" bs=1 seek=$off conv=notrunc 2>/dev/null
  rm -rf "$T/fo"; "$X" x "$T/f.xla" "$T/fo" > /dev/null 2>&1; rc=$?
  [ $rc -gt 3 ] && crash=$((crash+1))
  if [ -d "$T/fo/in" ]; then (cd "$T/fo/in" && find . -type f) | while read -r f; do cmp -s "$T/fo/in/$f" "$T/in/$f" || echo W; done | grep -q W && wrong=$((wrong+1)); fi
done
[ $wrong = 0 ] && [ $crash = 0 ] && echo "PASS 100 damaged archives: no wrong file written, no crash" || echo "FAIL damaged archives: wrong $wrong crash $crash"
rm -rf "$T"

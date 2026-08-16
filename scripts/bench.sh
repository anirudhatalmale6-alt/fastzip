#!/bin/sh
# Same benchmark as bench.ps1, for a Linux/WSL box.
#   ./scripts/bench.sh <folder-to-zip> [level]
set -e
DATA=${1:?usage: bench.sh <folder> [level]}
LEVEL=${2:-6}
EXE=${EXE:-./build/fastzip}
OUT=${TMPDIR:-/tmp}/bench_fastzip.zip
CORES=$(getconf _NPROCESSORS_ONLN)

printf '\nfastzip benchmark\n  input: %s\n  cores: %s\n  level: %s\n\n' "$DATA" "$CORES" "$LEVEL"
printf '%-34s %12s %14s\n' "configuration" "elapsed ms" "archive bytes"
printf '%-34s %12s %14s\n' "----------------------------------" "------------" "--------------"

row() {
	label=$1; shift
	json=$("$EXE" --json -l "$LEVEL" "$@" "$OUT" "$DATA")
	python3 -c "import json,sys; d=json.loads(sys.argv[1]); print('%-34s %12.1f %14d' % (sys.argv[2], d['elapsed_ms'], d['bytes_out']))" "$json" "$label"
}

row "baseline (1 thread, zlib)" --baseline
for t in 1 2 4 8 "$CORES"; do
	[ "$t" -le "$CORES" ] && row "fastzip, $t thread(s)" -t "$t"
done
printf '\nCorrectness check:\n'
"$EXE" --verify -l "$LEVEL" "$OUT" "$DATA"

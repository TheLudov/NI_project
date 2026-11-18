#!/bin/sh
# The student test suite: runs your programs as the submission platform does, never linking against your code.
#   tests/run.sh [BUILD_DIR [SCENARIO...]]      BUILD_DIR: build (default) or build-debug
# - BUILD_DIR/autotune must print exactly one line of key=value pairs, separated by commas (or an empty line),
#   on standard output, and exit with status 0;
# - BUILD_DIR/lb runs on each scenario of tests/ (BUILD_DIR/lb SCENARIO "PARAMS"), and the results file must
#   show the outcomes each scenario expects. PARAMS is built as the submission platform builds it: autotune's
#   output, then config.params, then the harness keys of the run (the scenario's, then results=...), joined by
#   commas. A key may appear only once: if one of yours is also a key of the run, the LB fails to start.
#   Each scenario states, in comments:
#     # params k=v,...                 harness keys of the run
#     # bonus                          run only with BONUS=1
#     # expect SCOPE,KEY OP VALUE      OP is = or >; VALUE a number or another SCOPE,KEY of the results
bin=${1:-build}
[ $# -gt 0 ] && shift
dir=$(dirname "$0")
out=$bin/tests
mkdir -p "$out"
fail=0
tune=                   # autotune's line, if it is well formed
conf=$(cat "$dir/../config.params" 2>/dev/null)

# autotune: one well-formed line on standard output
if [ ! -x "$bin/autotune" ]; then
    echo "autotune: FAIL: no $bin/autotune (make first)"
    fail=1
elif ! "$bin/autotune" > "$out/autotune.out" 2> "$out/autotune.log"; then
    echo "autotune: FAIL: exited with an error (see $out/autotune.log)"
    fail=1
elif [ "$(wc -l < "$out/autotune.out")" -ne 1 ] || [ -n "$(tail -c 1 "$out/autotune.out")" ]; then
    echo "autotune: FAIL: standard output is not exactly one line ending with a newline (see $out/autotune.out)"
    fail=1
elif ! grep -Eq '^([A-Za-z_][A-Za-z0-9_.]*=[^,]*(,[A-Za-z_][A-Za-z0-9_.]*=[^,]*)*)?$' "$out/autotune.out"; then
    echo "autotune: FAIL: not key=value,key=value,...: $(cat "$out/autotune.out")"
    fail=1
else
    echo "autotune: ok"
    tune=$(cat "$out/autotune.out")
fi

# lb: the scenarios
[ -x "$bin/lb" ] || { echo "lb: FAIL: no $bin/lb (make first)"; exit 1; }
[ $# -gt 0 ] || set -- "$dir"/*.scn
for scn in "$@"; do
    name=$(basename "$scn" .scn)
    if grep -q '^# bonus' "$scn" && [ "${BONUS:-0}" != 1 ]; then
        echo "$name: skipped (bonus: BONUS=1 to run it)"
        continue
    fi
    params=$(sed -n 's/^# params //p' "$scn" | tr -d ' ')
    res="$out/$name.csv"
    rm -f "$res"
    if ! GEN_QUIET=1 GEN_ABORT=0 "$bin/lb" "$scn" "$tune,$conf,${params:+$params,}results=$res" \
            > "$out/$name.log" 2>&1; then
        echo "$name: FAIL: the LB exited with an error (see $out/$name.log)"
        grep 'duplicate key' "$out/$name.log" | sed 's/^/  /'
        fail=1
        continue
    fi
    if [ ! -f "$res" ]; then
        echo "$name: FAIL: no results file (see $out/$name.log)"
        fail=1
        continue
    fi
    bad=$(grep '^# expect ' "$scn" | sed 's/^# expect //' | awk -v res="$res" '
        BEGIN { while ((getline line < res) > 0) { split(line, f, ","); v[f[1] "," f[2]] = f[3] } }
        function val(x) { return (x ~ /^[0-9.]+$/) ? x + 0 : ((x in v) ? v[x] + 0 : "missing:" x) }
        {
            a = val($1); b = val($3)
            if (a ~ /^missing/ || b ~ /^missing/) { print "  " $0 ": not in the results"; next }
            if (($2 == "=" && a != b) || ($2 == ">" && !(a > b))) print "  " $0 ": got " a " and " b
        }')
    if [ -n "$bad" ]; then
        echo "$name: FAIL"
        echo "$bad"
        fail=1
    else
        echo "$name: ok"
    fi
done
exit $fail

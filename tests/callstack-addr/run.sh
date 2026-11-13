#!/bin/bash
# Regression test for the callstack address contract (client walkers).
# usage: run.sh [TRACY_ROOT]   (default: the tree containing this script)
# For each walker (glibc backtrace(), _Unwind_Backtrace) it builds cstest.cpp against TRACY_ROOT, captures a callstack
# from work() and checks, for the frames of the test executable:
#   1. for the caller() and main() frames, Tracy's resolved inline chain == addr2line -i at the stored address
#      (the resolver needs no compensation);
#   2. the frame of caller() resolves to exactly "caller(int)": no inlined code of the temporary's destructor.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=${1:-$here/../..}
out=$(mktemp -d)
fail=0
for mode in backtrace unwind; do
    flags=""; [ $mode = unwind ] && flags="-DTRACY_FORCE_UNWIND_BACKTRACE"
    g++ -O2 -g -std=c++20 -w -DTRACY_ENABLE $flags -I"$root/public" -I"$root/public/tracy" -I"$here" "$here/cstest.cpp" "$root/public/TracyClient.cpp" -o "$out/cstest-$mode" -lpthread -ldl || { echo "build failed"; exit 2; }
    (cd "$out" && ./cstest-$mode 2>/dev/null | grep '^FRAME' > frames-$mode.txt)
    caller_ok=0
    while read -r _ idx img off chain; do
        [ "$(basename "$img")" = "cstest-$mode" ] || continue
        case "$chain" in *"caller(int)"*|main) ;; *) continue ;; esac   # the frames below work(): the others differ for unrelated reasons (libbacktrace vs binutils naming of Tracy's own inline frames, _start)
        # addr2line -i prints "function\nfile:line" pairs, innermost first
        a2l=$(addr2line -f -C -i -e "$out/cstest-$mode" 0x$off | awk 'NR%2==1' | paste -sd'|')
        if [ "$a2l" != "$chain" ]; then echo "FAIL [$mode] frame $idx: tracy='$chain' addr2line='$a2l'"; fail=1; fi
        case "$chain" in *"caller(int)"*)
            caller_ok=1
            [ "$chain" = "caller(int)" ] || { echo "FAIL [$mode] caller frame has extra inline frames: $chain"; fail=1; } ;;
        esac
    done < "$out/frames-$mode.txt"
    [ $caller_ok = 1 ] || { echo "FAIL [$mode] no caller(int) frame found"; fail=1; }
    [ $fail = 0 ] && echo "PASS [$mode]"
done
rm -rf "$out"
exit $fail

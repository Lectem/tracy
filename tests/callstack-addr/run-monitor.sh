#!/bin/bash
# Regression test for the perf sampling path, through tracy-monitor and tracy-stackdump.
# usage: run-monitor.sh MONITOR STACKDUMP     (needs perf_event access, see tracy-monitor -h)
# (perf callchains follow frame pointers: the test programs are built with -fno-omit-frame-pointer)
#  t.cpp:    the dominant sampled stack must be ..;main;caller(int);work(..) with no inlined destructor frames in between;
#  leaf.cpp: the exact PC of a sample must stay exact: samples of the 2-instruction tiny() are not attributed to [unknown] or to its
#            neighbour (how many samples tiny() gets varies between runs).
set -u
here=$(cd "$(dirname "$0")" && pwd)
mon=$1; dump=$2
out=$(mktemp -d); fail=0
g++ -O2 -g -fno-omit-frame-pointer -w "$here/t.cpp" -o "$out/t" && g++ -O2 -g -fno-omit-frame-pointer -w "$here/leaf.cpp" -o "$out/leaf" || exit 2
for p in t leaf; do
    timeout -s INT 5 "$mon" -f -o "$out/$p.tracy" "$out/$p" >/dev/null 2>&1
    "$dump" -n 100000 "$out/$p.tracy" > "$out/$p.txt" || { echo "FAIL no trace for $p"; fail=1; }
done
top=$(awk '{ if ($NF > m) { m=$NF; l=$0 } } END{print l}' "$out/t.txt")
case "$top" in *";main;caller(int);work("*) echo "PASS t: dominant stack is main;caller(int);work" ;; *) echo "FAIL t: dominant stack: $top" | cut -c1-300; fail=1 ;; esac
total=$(awk '{s+=$NF} END{print s}' "$out/leaf.txt")
tiny=$(awk '{n=$NF; $NF=""; if ($0 ~ /;tiny\(long\) $/ || $0 ~ /^tiny\(long\) $/) s+=n} END{print s+0}' "$out/leaf.txt")
unk=$(awk '{n=$NF; $NF=""; if ($0 ~ /\[unknown\] $/ || $0 ~ /neighbor\(long\) $/) s+=n} END{print s+0}' "$out/leaf.txt")
echo "leaf: total=$total tiny=$tiny unknown/neighbor leaf=$unk"
[ "$tiny" -gt 0 ] && [ "$unk" -le $((total/1000)) ] && echo "PASS leaf: tiny() samples kept" || { echo "FAIL leaf"; fail=1; }
rm -rf "$out"; exit $fail

#!/bin/sh
# [UIT 0076] Builds and runs test_ap_stub.c with the stub taken from src/host/preempt_win.c (Linux x86-64 box test).
set -e
d=$(mktemp -d)
python3 - "$d/stub.s" <<'PY'
import re, sys
src = open('src/host/preempt_win.c').read()
body = src[src.index('__asm__(".text'):]
body = body[:body.index('");') + 3]
lines = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
out = [l.replace('\\n', '') for l in lines]
out = [l for l in out if not l.strip().startswith(('.seh', '.def'))]
open(sys.argv[1], 'w').write('\n'.join(out) + '\n.section .note.GNU-stack,"",@progbits\n')
PY
cc -O2 -march=x86-64-v3 -mno-red-zone -pthread tools/tests/test_ap_stub.c "$d/stub.s" -o "$d/t"
"$d/t"

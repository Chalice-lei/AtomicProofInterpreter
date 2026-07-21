#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 /path/to/utxo_Interpreter" >&2
    exit 2
fi

bin="$1"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fixture="$repo_root/test/repl/repl_load.ct"
stdout_file="$(mktemp)"
stderr_file="$(mktemp)"
trap 'rm -f "$stdout_file" "$stderr_file"' EXIT

"$bin" shell -l none >"$stdout_file" 2>"$stderr_file" <<EOF
1 + 2
x = 5
x + 7
def inc(n: int):
    Return(n + 1)

inc(x)
%debug
%run "$fixture"
double(4)
%who
missing_name
history
exit
EOF

grep -F "In [1]:" "$stdout_file" >/dev/null
grep -F "Out[1]: 3" "$stdout_file" >/dev/null
grep -F "Out[3]: 12" "$stdout_file" >/dev/null
grep -F "Out[5]: 6" "$stdout_file" >/dev/null
grep -F "Loaded contract ReplLoad from $fixture" "$stdout_file" >/dev/null
grep -F "Out[6]: 8" "$stdout_file" >/dev/null
grep -F "double" "$stdout_file" >/dev/null
grep -F "1: 1 + 2" "$stdout_file" >/dev/null
grep -F "4: def inc(n: int):" "$stdout_file" >/dev/null
grep -F "Bye." "$stdout_file" >/dev/null
grep -F "Usage: %debug <file.ct>" "$stderr_file" >/dev/null
grep -F "Error[7]:" "$stderr_file" >/dev/null
grep -F "undefined_variable" "$stderr_file" >/dev/null

"$bin" shell "$fixture" -l none >"$stdout_file" 2>"$stderr_file" <<'EOF'
double(6)
exit
EOF

grep -F "Loaded contract ReplLoad from $fixture" "$stdout_file" >/dev/null
grep -F "Out[1]: 12" "$stdout_file" >/dev/null

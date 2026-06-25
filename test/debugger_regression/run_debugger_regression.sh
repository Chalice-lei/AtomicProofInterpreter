#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
COMPILER=${APC_COMPILER:-"$REPO_ROOT/build/bin/utxo_interpreter"}

if [[ ! -x "$COMPILER" ]]; then
    echo "Compiler not found or not executable: $COMPILER" >&2
    exit 1
fi

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

FIXTURE="$SCRIPT_DIR/debug_peephole_pc_drift.ct"
DEBUG_FILE="$TMP_DIR/peephole.debug"
BYTECODE_JSON="$TMP_DIR/debug_peephole_pc_drift.json"

run_compile() {
    (
        cd "$TMP_DIR"
        "$COMPILER" compile "$FIXTURE" "$@" >"$TMP_DIR/compile.log" 2>&1
    )
}

run_compile --debug-output "$DEBUG_FILE"
[[ -s "$DEBUG_FILE" ]]

python3 - "$DEBUG_FILE" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    debug_info = json.load(f)

assert debug_info.get("pcToSource"), "pcToSource should be generated"
assert debug_info.get("lineToPC"), "lineToPC should be generated"
PY

rm -f "$DEBUG_FILE" "$BYTECODE_JSON"
run_compile -d --debug-output "$DEBUG_FILE"
[[ -s "$DEBUG_FILE" ]]
[[ -s "$BYTECODE_JSON" ]]

python3 - "$DEBUG_FILE" "$BYTECODE_JSON" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    debug_info = json.load(f)
with open(sys.argv[2], "r", encoding="utf-8") as f:
    bytecode_json = json.load(f)

asm = bytecode_json["lock"]["asm"].split()
first_padding_pc = asm.index("OP_INVALIDOPCODE")
hex_bytes = bytecode_json["lock"]["hex"]
hex_ops = [hex_bytes[i:i + 2].lower() for i in range(0, len(hex_bytes), 2)]

mapped_pcs = sorted(int(pc) for pc in debug_info["pcToSource"].keys())
assert mapped_pcs, "expected at least one source mapping"
assert max(mapped_pcs) < first_padding_pc, (
    f"source mapped PC {max(mapped_pcs)} reaches padding at {first_padding_pc}"
)

for func in debug_info["functions"]:
    assert func["endPC"] <= first_padding_pc, (
        f"function {func['name']} ends in padding: "
        f"{func['endPC']} > {first_padding_pc}"
    )

for line, pcs in debug_info["lineToPC"].items():
    for pc in pcs:
        assert pc < first_padding_pc, (
            f"line {line} maps to padding PC {pc} at {first_padding_pc}"
        )

for inst in debug_info["instructions"]:
    pc = inst["pc"]
    assert pc < first_padding_pc, f"instruction maps to padding PC {pc}"
    assert inst["opcode"].lower() == hex_ops[pc], (
        f"instruction {pc} opcode is stale: "
        f"{inst['opcode']} != {hex_ops[pc]}"
    )
PY

SESSION_LOG="$TMP_DIR/debugger.log"
printf 'en\n1\n1\n2\nbytecode\nquit\n' | (
    cd "$TMP_DIR"
    "$COMPILER" debug "$FIXTURE" >"$SESSION_LOG" 2>&1
)

grep -q "function range total 8" "$SESSION_LOG"
if awk '/Bytecode list/,/\(apc-debug\)/' "$SESSION_LOG" | grep -q 'OP_INVALIDOPCODE'; then
    echo "Debugger function bytecode range includes OP_INVALIDOPCODE" >&2
    exit 1
fi

ARITH_TRACE="$TMP_DIR/arith_stack_trace.json"
"$COMPILER" run "$SCRIPT_DIR/debug_line_mapping_basic.ct" test_line_mapping 1 2 3 \
    --stack-trace-output "$ARITH_TRACE" >"$TMP_DIR/arith_trace.log" 2>&1
[[ -s "$ARITH_TRACE" ]]

python3 - "$ARITH_TRACE" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    trace = json.load(f)

steps = trace.get("steps", [])
assert steps, "stack trace should contain executed steps"
assert trace.get("source", {}).get("lines"), "source lines should be embedded"
assert any(step.get("sourceLine") for step in steps), "source lines should map to steps"
assert any(
    step.get("effects", {}).get("mainStack", {}).get("pushed")
    for step in steps
), "arithmetic trace should include pushed main-stack values"
assert any(
    step.get("effects", {}).get("mainStack", {}).get("popped")
    for step in steps
), "arithmetic trace should include popped main-stack values"
PY

ALT_TRACE="$TMP_DIR/alt_stack_trace.json"
"$COMPILER" run "$SCRIPT_DIR/debug_stack_visualizer_alt.ct" \
    test_alt_roundtrip 5 --stack-trace-output "$ALT_TRACE" >"$TMP_DIR/alt_trace.log" 2>&1
[[ -s "$ALT_TRACE" ]]

python3 - "$ALT_TRACE" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    trace = json.load(f)

steps = trace.get("steps", [])
ops = [step.get("opcode") for step in steps]
assert "OP_TOALTSTACK" in ops, "trace should include OP_TOALTSTACK"
assert "OP_FROMALTSTACK" in ops, "trace should include OP_FROMALTSTACK"

moves = [
    move
    for step in steps
    for move in step.get("effects", {}).get("moves", [])
]
assert any(move.get("from") == "main" and move.get("to") == "alt" for move in moves), (
    "trace should distinguish main -> alt movement"
)
assert any(move.get("from") == "alt" and move.get("to") == "main" for move in moves), (
    "trace should distinguish alt -> main movement"
)
PY

echo "Debugger regression checks passed."

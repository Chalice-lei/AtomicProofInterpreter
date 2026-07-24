#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
COMPILER=${APC_COMPILER:-"$REPO_ROOT/build/bin/utxo_Interpreter"}

if [[ ! -x "$COMPILER" ]]; then
    echo "Compiler not found or not executable: $COMPILER" >&2
    exit 1
fi

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

# 仅包含 fixed 声明的函数没有运行时指令，其合法空范围不能被调试信息
# 校验误判为反向范围。
for function_name in setup main; do
    empty_output="$TMP_DIR/empty-${function_name}.out"
    if ! "$COMPILER" run \
        "$SCRIPT_DIR/empty_runtime_function.ct" "$function_name" \
        >"$empty_output" 2>&1; then
        echo "Empty runtime function debug range failed: $function_name" >&2
        sed -n '1,120p' "$empty_output" >&2 || true
        exit 1
    fi
    if ! grep -q "status: finished" "$empty_output"; then
        echo "Empty runtime function did not finish: $function_name" >&2
        sed -n '1,120p' "$empty_output" >&2 || true
        exit 1
    fi
done

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

SCOPE_DEBUG_FILE="$TMP_DIR/fixed_scope_pairing.debug"
SCOPE_FIXTURE="$SCRIPT_DIR/debug_fixed_scope_pairing.ct"
(
    cd "$TMP_DIR"
    "$COMPILER" compile "$SCOPE_FIXTURE" \
        --debug-output "$SCOPE_DEBUG_FILE" \
        >"$TMP_DIR/fixed_scope_pairing.log" 2>&1
)
[[ -s "$SCOPE_DEBUG_FILE" ]]

python3 - "$SCOPE_DEBUG_FILE" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    debug_info = json.load(f)

assert debug_info.get("scopeNestingValid") is True, debug_info
scopes = debug_info["scopes"]
assert scopes, "expected debug scopes"

for scope in scopes:
    assert scope["startPC"] <= scope["endPC"], scope

global_scopes = [scope for scope in scopes if scope["type"] == "global"]
function_scopes = [scope for scope in scopes if scope["type"] == "function"]
block_scopes = [scope for scope in scopes if scope["type"] == "block"]
assert len(global_scopes) == 1, global_scopes
assert len(function_scopes) == 1, function_scopes
assert len(block_scopes) == 5, block_scopes

function_scope = function_scopes[0]
function_blocks = [
    scope for scope in block_scopes
    if scope["parentIndex"] == function_scope["index"]
]
assert len(function_blocks) == 1, function_blocks

outer_block = function_blocks[0]
branch_blocks = [
    scope for scope in block_scopes
    if scope["parentIndex"] == outer_block["index"]
]
assert len(branch_blocks) == 4, branch_blocks
assert all(
    scope["parentIndex"] == outer_block["index"]
    for scope in branch_blocks
)
PY

LOOP_DEBUG_FILE="$TMP_DIR/branch_loop_scope.debug"
LOOP_FIXTURE="$SCRIPT_DIR/debug_branch_loop_scope.ct"
(
    cd "$TMP_DIR"
    "$COMPILER" compile "$LOOP_FIXTURE" \
        --debug-output "$LOOP_DEBUG_FILE" \
        >"$TMP_DIR/branch_loop_scope.log" 2>&1
)
[[ -s "$LOOP_DEBUG_FILE" ]]

python3 - "$LOOP_DEBUG_FILE" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    debug_info = json.load(f)

assert debug_info.get("scopeNestingValid") is True, debug_info
scopes = debug_info["scopes"]
global_scope = next(scope for scope in scopes if scope["type"] == "global")
function_scope = next(scope for scope in scopes if scope["type"] == "function")
assert function_scope["parentIndex"] == global_scope["index"], function_scope

outer_blocks = [
    scope for scope in scopes
    if scope["type"] == "block"
    and scope["parentIndex"] == function_scope["index"]
]
assert len(outer_blocks) == 1, outer_blocks
outer = outer_blocks[0]

# The statically expanded Range(3) body must produce three balanced sibling
# block scopes, in addition to the then/else blocks.
children = [scope for scope in scopes if scope["parentIndex"] == outer["index"]]
assert len(children) == 5, children
assert sum(not scope["variables"] for scope in children) == 3, children
ordered = sorted(children, key=lambda scope: (scope["startPC"], scope["endPC"]))
for previous, current in zip(ordered, ordered[1:]):
    assert previous["endPC"] <= current["startPC"], (previous, current)

for scope in scopes:
    if scope["type"] != "global":
        assert scope["location"]["line"] > 0, scope
    for variable in scope["variables"]:
        assert variable["scopeName"] == scope["name"], (scope, variable)
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

python3 - "$COMPILER" "$SCRIPT_DIR/debug_stack_visualizer_alt.ct" <<'PY'
import json
import subprocess
import sys

compiler = sys.argv[1]
fixture = sys.argv[2]

proc = subprocess.Popen(
    [compiler, "debug-server", fixture, "test_alt_roundtrip", "5"],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)

def read_message():
    line = proc.stdout.readline()
    assert line, "live debug server closed stdout unexpectedly"
    return json.loads(line)

def send(seq, command, **extra):
    request = {"seq": seq, "command": command}
    request.update(extra)
    proc.stdin.write(json.dumps(request) + "\n")
    proc.stdin.flush()

ready = read_message()
assert ready["type"] == "event" and ready["event"] == "ready"

send(1, "stepIn")
step_response = read_message()
assert step_response["type"] == "response" and step_response["success"]
snapshot = step_response["body"]["snapshot"]
assert snapshot["pc"] == 1, snapshot
assert snapshot["opcode"] == "02", snapshot
assert snapshot["operand"] == "9600", snapshot
assert [item["pc"] for item in snapshot["lineInstructions"]] == [1], snapshot
assert snapshot["lineInstructionSummary"] == "pc 1*: 02 9600", snapshot
read_message()

send(2, "variables", scope="instruction")
variables_response = read_message()
assert variables_response["type"] == "response" and variables_response["success"]
variables = {
    item["name"]: item["value"]
    for item in variables_response["body"]["variables"]
}
assert variables["opcode"] == "pc 1*: 02 9600", variables
assert variables["currentOpcode"] == "02", variables
assert variables["operand"] == "9600", variables
assert variables["instructions"] == "pc 1*: 02 9600", variables

send(3, "stepIn")
line_response = read_message()
assert line_response["type"] == "response" and line_response["success"]
line_snapshot = line_response["body"]["snapshot"]
assert line_snapshot["pc"] == 2, line_snapshot
assert [item["pc"] for item in line_snapshot["lineInstructions"]] == [2, 3, 4], line_snapshot
assert "pc 2*:" in line_snapshot["lineInstructionSummary"], line_snapshot
assert "pc 3:" in line_snapshot["lineInstructionSummary"], line_snapshot
assert "pc 4:" in line_snapshot["lineInstructionSummary"], line_snapshot
read_message()

send(4, "variables", scope="instruction")
line_variables_response = read_message()
assert line_variables_response["type"] == "response" and line_variables_response["success"]
line_variables = {
    item["name"]: item["value"]
    for item in line_variables_response["body"]["variables"]
}
assert line_variables["instructions"] == line_snapshot["lineInstructionSummary"], line_variables
assert line_variables["opcode"] == line_snapshot["lineInstructionSummary"], line_variables
assert line_variables["currentOpcode"] == line_snapshot["opcode"], line_variables

proc.stdin.close()
stderr = proc.stderr.read()
return_code = proc.wait(timeout=10)
assert return_code == 0, stderr
PY

echo "Debugger regression checks passed."

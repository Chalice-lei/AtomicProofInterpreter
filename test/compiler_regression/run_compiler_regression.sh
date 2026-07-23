#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
COMPILER=${APC_COMPILER:-"$REPO_ROOT/build/bin/utxo_Interpreter"}
export APC_STDLIB_PATH=${APC_STDLIB_PATH:-"$REPO_ROOT/stdlib"}

if [[ ! -x "$COMPILER" ]]; then
    echo "Compiler not found or not executable: $COMPILER" >&2
    exit 1
fi

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

write_case() {
    local name=$1
    shift
    printf '%s\n' "$@" >"$TMP_DIR/$name"
}

run_compile() {
    local file=$1
    (
        cd "$TMP_DIR"
        "$COMPILER" compile "$file" >"$file.out" 2>"$file.err"
    )
}

expect_success() {
    local file=$1
    local stem=${file%.ct}

    if ! run_compile "$file"; then
        echo "Expected success for $file" >&2
        sed -n '1,120p' "$TMP_DIR/$file.err" >&2 || true
        sed -n '1,120p' "$TMP_DIR/$file.out" >&2 || true
        exit 1
    fi

    if [[ ! -s "$TMP_DIR/$stem.json" ]]; then
        echo "Expected JSON output for $file" >&2
        exit 1
    fi
}

expect_failure() {
    local file=$1
    local pattern=$2
    local stem=${file%.ct}

    set +e
    run_compile "$file"
    local rc=$?
    set -e

    if [[ $rc -eq 0 ]]; then
        echo "Expected failure for $file" >&2
        exit 1
    fi

    if [[ -e "$TMP_DIR/$stem.json" ]]; then
        echo "Unexpected JSON output for failing case $file" >&2
        exit 1
    fi

    if ! grep -q -- "$pattern" "$TMP_DIR/$file.err" "$TMP_DIR/$file.out"; then
        echo "Expected diagnostic '$pattern' for $file" >&2
        sed -n '1,120p' "$TMP_DIR/$file.err" >&2 || true
        sed -n '1,120p' "$TMP_DIR/$file.out" >&2 || true
        exit 1
    fi
}

write_case bang.ct \
    "Contract Bang:" \
    "    def main():" \
    "        Return !1"

write_case notequal.ct \
    "Contract NotEqual:" \
    "    def main():" \
    "        Return 1 != 0"

write_case two_contracts.ct \
    "Contract First:" \
    "    def main():" \
    "        Return 1" \
    "" \
    "Contract Second:" \
    "    def main():" \
    "        Return 0"

write_case trailing_garbage.ct \
    "Contract First:" \
    "    def main():" \
    "        Return 1" \
    "" \
    "THIS IS GARBAGE"

write_case return_bool_ok.ct \
    "Contract ReturnBoolOk:" \
    "    def main() -> bool:" \
    "        Return 1"

write_case return_bool_bad.ct \
    "Contract ReturnBoolBad:" \
    "    def main() -> bool:" \
    "        Return 123"

write_case bool_two.ct \
    "Contract BoolTwo:" \
    "    def main():" \
    "        flag: bool = 2" \
    "        Return flag"

write_case hex_string.ct \
    "Contract HexString:" \
    "    def main():" \
    "        h: hex = \"not hex\"" \
    "        Return h"

write_case string_num.ct \
    "Contract StringNum:" \
    "    def main():" \
    "        s: string = 1" \
    "        Return s"

write_case self_return.ct \
    "Contract SelfReturn:" \
    "    def main():" \
    "        Return self.value"

write_case no_return.ct \
    "Contract NoReturn:" \
    "    def main():" \
    "        x: int = 1"

write_case huge_num.ct \
    "Contract HugeNum:" \
    "    def main():" \
    "        Return 999999999999999999999999999999999999999999999999999999"

write_case param_array_loop_index.ct \
    "Contract ParamArrayLoopIndex:" \
    "    def main(values: int[3]):" \
    "        acc: int = 0" \
    "        for i in Range(2, -1, -1):" \
    "            acc = acc + values[i]" \
    "        Return acc"

write_case branch_outer_rebind.ct \
    "Contract BranchOuterRebind:" \
    "    def main(flag: int):" \
    "        acc: int = 0" \
    "        if flag > 0:" \
    "            branch_value: int = 1" \
    "            acc = acc + branch_value" \
    "        else:" \
    "            fallback: int = 2" \
    "            acc = acc + fallback" \
    "        Return(acc)"

write_case branch_fixed_return.ct \
    "Contract BranchFixedReturn:" \
    "    def main(flag: int):" \
    "        value: int = 0" \
    "        if flag > 0:" \
    "            value = 1" \
    "        else:" \
    "            value = 2" \
    "        Return(value)"

write_case branch_fixed_one_sided.ct \
    "Contract BranchFixedOneSided:" \
    "    def main(flag: int):" \
    "        value: int = 7" \
    "        if flag > 0:" \
    "            value = 1" \
    "        else:" \
    "            local_only: int = 9" \
    "        Return(value)"

write_case branch_fixed_multiple.ct \
    "Contract BranchFixedMultiple:" \
    "    def main(flag: int):" \
    "        first: int = 0" \
    "        second: int = 0" \
    "        if flag > 0:" \
    "            first = 1" \
    "            second = 2" \
    "        else:" \
    "            first = 3" \
    "            second = 4" \
    "        Return(first * 10 + second)"

write_case branch_fixed_nested.ct \
    "Contract BranchFixedNested:" \
    "    def main(flag: int):" \
    "        value: int = 0" \
    "        if flag.Clone() > 0:" \
    "            if flag.Clone() > 1:" \
    "                value = 1" \
    "            else:" \
    "                value = 2" \
    "        else:" \
    "            value = 3" \
    "        Return(value)"

write_case branch_stack_to_fixed.ct \
    "Contract BranchStackToFixed:" \
    "    def main(flag: int):" \
    "        if flag > 0:" \
    "            flag = 1" \
    "        else:" \
    "            flag = 2" \
    "        Return(flag)"

write_case branch_uninitialized_fixed.ct \
    "Contract BranchUninitializedFixed:" \
    "    def main(flag: int):" \
    "        value: int" \
    "        if flag > 0:" \
    "            value = 1" \
    "        else:" \
    "            value = 2" \
    "        Return(value)"

write_case branch_terminating_fixed.ct \
    "Contract BranchTerminatingFixed:" \
    "    def main(flag: int):" \
    "        value: int = 0" \
    "        if flag > 0:" \
    "            Return(9)" \
    "        else:" \
    "            value = 2" \
    "        Return(value)"

expect_failure bang.ct "unexpected character '!'"
expect_success notequal.ct
expect_failure two_contracts.ct "unexpected token after contract definition"
expect_failure trailing_garbage.ct "unexpected token after contract definition"
expect_success return_bool_ok.ct
expect_failure return_bool_bad.ct "return type mismatch"
expect_failure bool_two.ct "type mismatch in variable declaration"
expect_failure hex_string.ct "type mismatch in variable declaration"
expect_failure string_num.ct "type mismatch in variable declaration"
expect_success self_return.ct
expect_failure no_return.ct "generated bytecode is empty"
expect_failure huge_num.ct "integer literal out of range"
expect_success param_array_loop_index.ct
expect_success branch_outer_rebind.ct
expect_success branch_fixed_return.ct
expect_success branch_fixed_one_sided.ct
expect_success branch_fixed_multiple.ct
expect_success branch_fixed_nested.ct
expect_success branch_stack_to_fixed.ct
expect_success branch_uninitialized_fixed.ct
expect_success branch_terminating_fixed.ct

python3 - "$TMP_DIR/branch_fixed_return.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    tokens = json.load(f)["lock"]["asm"].split()

if_pos = tokens.index("OP_IF")
else_pos = tokens.index("OP_ELSE", if_pos + 1)
endif_pos = tokens.index("OP_ENDIF", else_pos + 1)
return_pos = tokens.index("OP_RETURN", endif_pos + 1)

assert "OP_1" in tokens[if_pos + 1:else_pos], tokens
assert "OP_2" in tokens[else_pos + 1:endif_pos], tokens
assert "OP_1" not in tokens[endif_pos + 1:return_pos], tokens
assert "OP_2" not in tokens[endif_pos + 1:return_pos], tokens
PY

python3 - "$TMP_DIR/self_return.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    data = json.load(f)

asm = data["lock"]["asm"].split()
assert asm and asm[0] == "<self.value>", data["lock"]["asm"]
assert "<self.value>" in data["lock"]["hex"], data["lock"]["hex"]
PY

for flag in 1 -1; do
    trace_file="$TMP_DIR/branch_outer_rebind_${flag}.json"
    (
        cd "$TMP_DIR"
        "$COMPILER" run branch_outer_rebind.ct main "$flag" \
            --stack-trace-output "$trace_file" \
            >"branch_outer_rebind_${flag}.out" \
            2>"branch_outer_rebind_${flag}.err"
    )
done

python3 - \
    "$TMP_DIR/branch_outer_rebind_1.json" \
    "$TMP_DIR/branch_outer_rebind_-1.json" <<'PY'
import json
import sys

def return_value(path):
    with open(path, "r", encoding="utf-8") as f:
        trace = json.load(f)
    returns = [step for step in trace["steps"] if step["opcode"] == "OP_RETURN"]
    assert returns, f"missing OP_RETURN in {path}"
    stack = returns[-1]["mainStackAfter"]
    assert stack, f"empty return stack in {path}"
    return stack[-1]["intString"]

assert return_value(sys.argv[1]) == "1", sys.argv[1]
assert return_value(sys.argv[2]) == "2", sys.argv[2]
PY

expect_ast_and_run_int() {
    local file=$1
    local argument=$2
    local expected=$3
    local stem=${file%.ct}

    for mode in ast run; do
        local output="$TMP_DIR/${stem}_${mode}_${argument}.out"
        if ! (
            cd "$TMP_DIR"
            "$COMPILER" "$mode" "$file" main "$argument" >"$output" 2>&1
        ); then
            echo "Expected $mode execution to succeed for $file $argument" >&2
            sed -n '1,160p' "$output" >&2 || true
            exit 1
        fi
        if ! grep -q "status: finished" "$output" ||
           ! grep -q "int=$expected" "$output"; then
            echo "Expected $mode result int=$expected for $file $argument" >&2
            sed -n '1,160p' "$output" >&2 || true
            exit 1
        fi
    done
}

expect_ast_and_run_int branch_fixed_return.ct 1 1
expect_ast_and_run_int branch_fixed_return.ct -1 2
expect_ast_and_run_int branch_fixed_one_sided.ct 1 1
expect_ast_and_run_int branch_fixed_one_sided.ct -1 7
expect_ast_and_run_int branch_fixed_multiple.ct 1 12
expect_ast_and_run_int branch_fixed_multiple.ct -1 34
expect_ast_and_run_int branch_fixed_nested.ct 2 1
expect_ast_and_run_int branch_fixed_nested.ct 1 2
expect_ast_and_run_int branch_fixed_nested.ct -1 3
expect_ast_and_run_int branch_stack_to_fixed.ct 1 1
expect_ast_and_run_int branch_stack_to_fixed.ct -1 2
expect_ast_and_run_int branch_uninitialized_fixed.ct 1 1
expect_ast_and_run_int branch_uninitialized_fixed.ct -1 2
expect_ast_and_run_int branch_terminating_fixed.ct 1 9
expect_ast_and_run_int branch_terminating_fixed.ct -1 2

echo "Compiler regression checks passed."

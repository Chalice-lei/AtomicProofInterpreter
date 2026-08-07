#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)

if [[ $# -gt 1 ]]; then
    echo "Usage: $0 [/path/to/utxo_Interpreter]" >&2
    exit 2
fi

COMPILER=${1:-${APC_COMPILER:-"$REPO_ROOT/build/bin/utxo_Interpreter"}}
export APC_STDLIB_PATH=${APC_STDLIB_PATH:-"$REPO_ROOT/stdlib"}

if [[ ! -x "$COMPILER" ]]; then
    echo "Compiler not found or not executable: $COMPILER" >&2
    exit 1
fi

COMPILER=$(realpath "$COMPILER")
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

write_case() {
    local name=$1
    shift
    printf '%s\n' "$@" >"$TMP_DIR/$name"
}

compile_case() {
    local file=$1
    (
        cd "$TMP_DIR"
        "$COMPILER" compile "$file" >"$file.out" 2>"$file.err"
    )
}

expect_success() {
    local file=$1
    local stem=${file%.ct}

    if ! compile_case "$file"; then
        echo "Expected success for $file" >&2
        sed -n '1,120p' "$TMP_DIR/$file.err" >&2 || true
        sed -n '1,120p' "$TMP_DIR/$file.out" >&2 || true
        exit 1
    fi

    [[ -s "$TMP_DIR/$stem.json" ]]
}

expect_failure() {
    local file=$1
    local stem=${file%.ct}
    local expected_message=${2:-}

    if compile_case "$file"; then
        echo "Expected failure for $file" >&2
        exit 1
    fi

    if [[ -e "$TMP_DIR/$stem.json" ]]; then
        echo "Unexpected JSON output for failing case $file" >&2
        exit 1
    fi

    if [[ -n "$expected_message" ]] &&
        ! grep -Fq "$expected_message" \
            "$TMP_DIR/$file.out" "$TMP_DIR/$file.err"; then
        echo "Expected diagnostic not found for $file: $expected_message" >&2
        sed -n '1,120p' "$TMP_DIR/$file.err" >&2 || true
        sed -n '1,120p' "$TMP_DIR/$file.out" >&2 || true
        exit 1
    fi
}

write_case verify_top.ct \
    "Contract VerifyTop:" \
    "    def main(condition: bool):" \
    "        Verify(condition)"

write_case verify_swap.ct \
    "Contract VerifySwap:" \
    "    def main(condition: bool, other: bool):" \
    "        Verify(condition)" \
    "        Verify(other)"

write_case verify_rot.ct \
    "Contract VerifyRot:" \
    "    def main(condition: bool, other1: bool, other2: bool):" \
    "        Verify(condition)" \
    "        Verify(other1)" \
    "        Verify(other2)"

write_case verify_roll.ct \
    "Contract VerifyRoll:" \
    "    def main(condition: bool, other1: bool, other2: bool, other3: bool):" \
    "        Verify(condition)" \
    "        Verify(other1)" \
    "        Verify(other2)" \
    "        Verify(other3)"

write_case verify_expression.ct \
    "Contract VerifyExpression:" \
    "    def main(left: bool, right: bool):" \
    "        Verify(And(left, right))"

write_case verify_true_literal.ct \
    "Contract VerifyTrueLiteral:" \
    "    def main():" \
    "        Verify(1)"

write_case verify_false_literal.ct \
    "Contract VerifyFalseLiteral:" \
    "    def main():" \
    "        Verify(0)"

write_case verify_no_args.ct \
    "Contract VerifyNoArgs:" \
    "    def main():" \
    "        Verify()"

write_case verify_two_args.ct \
    "Contract VerifyTwoArgs:" \
    "    def main(left: bool, right: bool):" \
    "        Verify(left, right)"

write_case verify_consumes_field.ct \
    "Contract VerifyConsumesField:" \
    "    Struct State:" \
    "        condition: bool" \
    "    def main(state: State):" \
    "        Verify(state.condition)" \
    "        Verify(state.condition)"

write_case verify_assignment.ct \
    "Contract VerifyAssignment:" \
    "    def main(condition: bool, fallback: int):" \
    "        result = Verify(condition)"

write_case verify_nested_value.ct \
    "Contract VerifyNestedValue:" \
    "    def main(condition: bool):" \
    "        Verify(Verify(condition))"

write_case verify_move_argument.ct \
    "Contract VerifyMoveArgument:" \
    "    def main(condition: bool, fallback: int):" \
    "        Move(Verify(condition))"

write_case verify_set_alt_argument.ct \
    "Contract VerifySetAltArgument:" \
    "    def main(condition: bool, fallback: int):" \
    "        SetAlt(Verify(condition))"

write_case verify_set_main_argument.ct \
    "Contract VerifySetMainArgument:" \
    "    def main(condition: bool, fallback: int):" \
    "        SetMain(Verify(condition))"

write_case verify_method_object.ct \
    "Contract VerifyMethodObject:" \
    "    def main(condition: bool, fallback: int):" \
    "        Verify(condition).Clone()"

write_case verify_wrapped_special_argument.ct \
    "Contract VerifyWrappedSpecialArgument:" \
    "    def main(condition: bool, fallback: int):" \
    "        SetAlt(Add(Verify(condition), 1))"

write_case verify_wrapped_method_object.ct \
    "Contract VerifyWrappedMethodObject:" \
    "    def main(condition: bool, fallback: int):" \
    "        (Verify(condition) + 1).Clone()"

write_case verify_index_expression.ct \
    "Contract VerifyIndexExpression:" \
    "    def main(condition: bool, fallback: int):" \
    "        Move(fallback[Verify(condition)])"

write_case verify_script_return.ct \
    "Contract VerifyScriptReturn:" \
    "    def main(condition: bool):" \
    "        Return(Verify(condition))"

write_case verify_value_return.ct \
    "Contract VerifyValueReturn:" \
    "    def main(condition: bool):" \
    "        return Verify(condition)"

expect_success verify_top.ct
expect_success verify_swap.ct
expect_success verify_rot.ct
expect_success verify_roll.ct
expect_success verify_expression.ct
expect_success verify_true_literal.ct
expect_success verify_false_literal.ct
expect_failure verify_no_args.ct
expect_failure verify_two_args.ct
expect_failure verify_consumes_field.ct
verify_no_value_message="Verify() does not return a value and cannot be used in a value context"
expect_failure verify_assignment.ct "$verify_no_value_message"
expect_failure verify_nested_value.ct "$verify_no_value_message"
expect_failure verify_move_argument.ct "$verify_no_value_message"
expect_failure verify_set_alt_argument.ct "$verify_no_value_message"
expect_failure verify_set_main_argument.ct "$verify_no_value_message"
expect_failure verify_method_object.ct "$verify_no_value_message"
expect_failure verify_wrapped_special_argument.ct "$verify_no_value_message"
expect_failure verify_wrapped_method_object.ct "$verify_no_value_message"
expect_failure verify_index_expression.ct "$verify_no_value_message"
expect_failure verify_script_return.ct "$verify_no_value_message"
expect_failure verify_value_return.ct "$verify_no_value_message"

python3 - "$TMP_DIR" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])

def load(stem):
    with (root / f"{stem}.json").open("r", encoding="utf-8") as f:
        return json.load(f)

def asm(stem):
    return load(stem)["lock"]["asm"].split()

assert asm("verify_top") == ["OP_VERIFY"]
assert asm("verify_swap") == ["OP_SWAP", "OP_VERIFY", "OP_VERIFY"]
assert asm("verify_rot") == [
    "OP_ROT", "OP_VERIFY", "OP_SWAP", "OP_VERIFY", "OP_VERIFY"
]
assert asm("verify_roll") == [
    "OP_3", "OP_ROLL", "OP_VERIFY",
    "OP_ROT", "OP_VERIFY",
    "OP_SWAP", "OP_VERIFY",
    "OP_VERIFY",
]
assert asm("verify_expression") == ["OP_BOOLAND", "OP_VERIFY"]
assert asm("verify_true_literal") == ["OP_1", "OP_VERIFY"]
assert asm("verify_false_literal") == ["OP_0", "OP_VERIFY"]

top = load("verify_top")
assert top["lock"]["hex"].lower() == "69", top["lock"]
assert any(item["name"] == "main" for item in top["abi"]), top["abi"]
assert all(item["name"] != "Verify" for item in top["abi"]), top["abi"]
PY

echo "Verify compiler regression checks passed."

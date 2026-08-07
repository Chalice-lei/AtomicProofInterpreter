#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
CASE_DIR="$SCRIPT_DIR/ownership"

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
trap 'rm -rf -- "$TMP_DIR"' EXIT
cp "$CASE_DIR"/*.ct "$TMP_DIR/"

run_compile() {
    local file=$1
    shift
    (
        cd "$TMP_DIR"
        "$COMPILER" "$@" compile "$file" >"$file.out" 2>"$file.err"
    )
}

print_diagnostics() {
    local file=$1
    sed -n '1,120p' "$TMP_DIR/$file.err" >&2 || true
    sed -n '1,120p' "$TMP_DIR/$file.out" >&2 || true
}

expect_success() {
    local file=$1
    local stem=${file%.ct}

    if ! run_compile "$file"; then
        echo "Expected success for $file" >&2
        print_diagnostics "$file"
        exit 1
    fi

    if [[ ! -s "$TMP_DIR/$stem.json" ]]; then
        echo "Expected JSON output for $file" >&2
        print_diagnostics "$file"
        exit 1
    fi
}

expect_failure() {
    local file=$1
    shift
    local stem=${file%.ct}
    local -a compiler_args=()

    if [[ ${1:-} == "--asa" ]]; then
        compiler_args+=("$1")
        shift
    fi

    set +e
    run_compile "$file" "${compiler_args[@]}"
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

    local pattern
    for pattern in "$@"; do
        if ! grep -Eiq -- "$pattern" \
            "$TMP_DIR/$file.err" "$TMP_DIR/$file.out"; then
            echo "Expected diagnostic pattern '$pattern' for $file" >&2
            print_diagnostics "$file"
            exit 1
        fi
    done
}

expect_success size_borrow.ct
if grep -q "OP_DUP" "$TMP_DIR/size_borrow.json"; then
    echo "Size borrow unexpectedly emitted OP_DUP" >&2
    exit 1
fi
expect_success local_uint64_array.ct
expect_failure use_after_delete.ct "amount(\\[0\\])?" "consum"
expect_failure size_after_move.ct "value" "consum|borrow"
expect_failure delete_after_use.ct "value" "consumed more than once"
expect_failure branch_element_delete.ct "amount\\[0\\]" "consum"
expect_failure size_after_array_delete.ct "amount\\[0\\]" "consum|borrow"
expect_success keep_borrow.ct
expect_failure keep_after_delete.ct "value" "consum|borrow"
expect_success scalar_copy_borrow.ct
expect_success field_copy_borrow.ct
expect_success array_copy_borrow.ct
expect_success branch_both_consume.ct
expect_failure branch_one_side_consume.ct \
    "stack state inconsistency|stack layouts differ"
expect_failure first_binding_moves.ct "source" "consumed more than once"
expect_failure uninitialized_field_moves.ct "source" "consumed more than once"
expect_failure copy_from_consumed.ct "source" "consum|borrow"
expect_success terminal_then_branch.ct
expect_success terminal_else_branch.ct
expect_success terminal_both_branches.ct
expect_success terminal_static_loop.ct
expect_success zero_iteration_falls_through.ct
expect_failure reachable_branch_consumes.ct "value" "consum"
expect_success lowercase_literal_return.ct
expect_success lowercase_expression_return.ct
if grep -q "Variable 'value' declared but not used" \
    "$TMP_DIR/lowercase_expression_return.ct.err"; then
    echo "Borrow-only lowercase return was reported as unused" >&2
    exit 1
fi
expect_success lowercase_field_return.ct
expect_success lowercase_array_element_return.ct
expect_failure rebind_consumed_scalar_moves.ct \
    "source" "consumed more than once"
expect_failure rebind_consumed_field_moves.ct \
    "source" "consumed more than once"
expect_failure rebind_consumed_element_moves.ct \
    "source" "consumed more than once"
expect_success rebind_deleted_field.ct
expect_success rebind_deleted_element.ct
expect_success rebind_whole_deleted_element.ct
expect_success branch_rebind_deleted_field.ct
expect_success branch_rebind_deleted_element.ct
expect_failure rebind_deleted_field_wrong_type.ct \
    "type mismatch" "expected 'number'"
expect_failure rebind_deleted_element_wrong_type.ct \
    "type mismatch" "expected 'number'"
expect_failure lowercase_void_return.ct "lowercase.*return" "produced no value"
expect_failure lowercase_void_private_return.ct \
    "lowercase.*return" "produced no value"
expect_success struct_return_direct.ct
expect_success struct_return_same_name.ct
expect_success struct_return_fresh_direct.ct
expect_success struct_return_fresh.ct
expect_success struct_return_typed_init.ct
expect_success struct_return_nested_direct.ct
expect_success struct_return_nested.ct
expect_success struct_return_fixed.ct
expect_success struct_return_fixed_direct.ct
expect_success struct_return_typed_same_name.ct
expect_success struct_return_alt.ct
expect_success struct_return_then_void_direct.ct
expect_success struct_return_then_void.ct
expect_success struct_return_local_cleanup_direct.ct
expect_success struct_return_local_cleanup.ct
expect_success struct_return_param_cleanup_direct.ct
expect_success struct_return_param_cleanup.ct
expect_success struct_return_field_update_direct.ct
expect_success struct_return_field_update.ct
expect_success struct_return_binding_scope_direct.ct
expect_success struct_return_binding_scope.ct
for file in private_alt_local_escape.ct private_alt_caller_binding.ct; do
    if ! run_compile "$file" --asa; then
        echo "Expected success for $file with --asa" >&2
        print_diagnostics "$file"
        exit 1
    fi
done
python3 - "$TMP_DIR/private_alt_local_escape.json" \
    "$TMP_DIR/private_alt_caller_binding.json" <<'PY'
import json
import sys

for path in sys.argv[1:]:
    with open(path, "r", encoding="utf-8") as f:
        tokens = json.load(f)["lock"]["asm"].split()
    assert tokens.count("OP_TOALTSTACK") == 1, (path, tokens)
    assert tokens.count("OP_FROMALTSTACK") == 1, (path, tokens)
PY
expect_failure missing_alt_binding.ct "alternate stack|altstack"
expect_failure uncalled_alt_producer.ct --asa "alternate stack|altstack"
expect_failure misspelled_alt_binding.ct --asa "alternate stack|altstack"
expect_success public_alt_across_private_definition.ct
expect_failure unused_struct_return.ct "returned struct value is unused"
expect_failure struct_multi_return.ct \
    "struct values cannot participate" "multi-value return"
expect_success scalar_multi_return.ct
expect_failure struct_multiple_returns.ct \
    "struct-returning function" "additional lowercase return"
expect_failure incomplete_struct_return.ct \
    "incomplete struct return" "input.left|value.left"

for file in rebind_deleted_field.ct rebind_deleted_element.ct; do
    output="$TMP_DIR/${file%.ct}.run.out"
    if ! "$COMPILER" run "$TMP_DIR/$file" main 7 3 >"$output" 2>&1; then
        echo "Rebinding runtime check failed for $file" >&2
        sed -n '1,120p' "$output" >&2 || true
        exit 1
    fi
    if ! grep -q "status: finished" "$output" ||
       ! grep -q "int=7" "$output"; then
        echo "Unexpected rebound value for $file" >&2
        sed -n '1,120p' "$output" >&2 || true
        exit 1
    fi
done

for file in branch_rebind_deleted_field.ct \
    branch_rebind_deleted_element.ct; do
    for flag in 0 1; do
        output="$TMP_DIR/${file%.ct}-${flag}.run.out"
        if ! "$COMPILER" run "$TMP_DIR/$file" main 7 3 "$flag" \
            >"$output" 2>&1; then
            echo "Branch rebinding runtime check failed for $file flag=$flag" \
                >&2
            sed -n '1,120p' "$output" >&2 || true
            exit 1
        fi
        if ! grep -q "status: finished" "$output" ||
           ! grep -q "int=7" "$output"; then
            echo "Unexpected branch rebound value for $file flag=$flag" >&2
            sed -n '1,120p' "$output" >&2 || true
            exit 1
        fi
    done
done

python3 - "$TMP_DIR/terminal_both_branches.json" \
    "$TMP_DIR/terminal_static_loop.json" \
    "$TMP_DIR/lowercase_literal_return.json" \
    "$TMP_DIR/lowercase_expression_return.json" \
    "$TMP_DIR/lowercase_field_return.json" \
    "$TMP_DIR/lowercase_array_element_return.json" <<'PY'
import json
import sys


def op_return_count(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)["lock"]["asm"].split().count("OP_RETURN")


assert op_return_count(sys.argv[1]) == 2, sys.argv[1]
assert op_return_count(sys.argv[2]) == 1, sys.argv[2]
assert op_return_count(sys.argv[3]) == 0, sys.argv[3]
assert op_return_count(sys.argv[4]) == 0, sys.argv[4]
with open(sys.argv[5], "r", encoding="utf-8") as f:
    field_ops = json.load(f)["lock"]["asm"].split()
with open(sys.argv[6], "r", encoding="utf-8") as f:
    element_ops = json.load(f)["lock"]["asm"].split()
assert field_ops.count("OP_DROP") == 0, sys.argv[5]
assert element_ops.count("OP_DROP") == 1, sys.argv[6]
PY

python3 - "$TMP_DIR/struct_return_direct.json" \
    "$TMP_DIR/struct_return_same_name.json" \
    "$TMP_DIR/struct_return_fresh_direct.json" \
    "$TMP_DIR/struct_return_fresh.json" \
    "$TMP_DIR/struct_return_nested_direct.json" \
    "$TMP_DIR/struct_return_nested.json" \
    "$TMP_DIR/struct_return_then_void_direct.json" \
    "$TMP_DIR/struct_return_then_void.json" \
    "$TMP_DIR/struct_return_fixed.json" \
    "$TMP_DIR/struct_return_alt.json" \
    "$TMP_DIR/struct_return_local_cleanup_direct.json" \
    "$TMP_DIR/struct_return_local_cleanup.json" \
    "$TMP_DIR/struct_return_param_cleanup_direct.json" \
    "$TMP_DIR/struct_return_param_cleanup.json" \
    "$TMP_DIR/struct_return_fixed_direct.json" \
    "$TMP_DIR/struct_return_field_update_direct.json" \
    "$TMP_DIR/struct_return_field_update.json" \
    "$TMP_DIR/struct_return_typed_init.json" \
    "$TMP_DIR/struct_return_typed_same_name.json" <<'PY'
import json
import sys


def lock(path):
    with open(path, "r", encoding="utf-8") as f:
        result = json.load(f)["lock"]
    bytes.fromhex(result["hex"])
    return result


for direct_index, routed_index in (
    (1, 2),
    (3, 4),
    (5, 6),
    (7, 8),
    (11, 12),
    (13, 14),
    (15, 9),
    (16, 17),
    (3, 18),
    (15, 19),
):
    direct = lock(sys.argv[direct_index])
    routed = lock(sys.argv[routed_index])
    assert routed["hex"] == direct["hex"], (
        sys.argv[direct_index],
        sys.argv[routed_index],
    )
    assert routed["asm"] == direct["asm"], (
        sys.argv[direct_index],
        sys.argv[routed_index],
    )

fixed = lock(sys.argv[9])
alt = lock(sys.argv[10])
assert fixed["hex"] and fixed["asm"], sys.argv[9]
alt_tokens = alt["asm"].split()
assert alt["hex"] and alt_tokens.count("OP_FROMALTSTACK") >= 5, sys.argv[10]
PY

echo "Ownership regression checks passed."

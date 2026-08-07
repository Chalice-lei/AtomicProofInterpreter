#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
COMPILER=${1:-${APC_COMPILER:-"$REPO_ROOT/build/bin/utxo_Interpreter"}}
export APC_STDLIB_PATH=${APC_STDLIB_PATH:-"$REPO_ROOT/stdlib"}

if [[ ! -x "$COMPILER" ]]; then
    echo "Compiler not found or not executable: $COMPILER" >&2
    exit 1
fi

COMPILER=$(realpath "$COMPILER")
TMP_DIR=$(mktemp -d)
trap 'rm -rf -- "$TMP_DIR"' EXIT

write_case() {
    local name=$1
    shift
    printf '%s\n' "$@" >"$TMP_DIR/$name"
}

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
    shift
    if ! run_compile "$file" "$@"; then
        echo "Expected success for $file" >&2
        print_diagnostics "$file"
        exit 1
    fi
    [[ -s "$TMP_DIR/${file%.ct}.json" ]]
}

expect_failure() {
    local file=$1
    local pattern=$2
    shift 2
    set +e
    run_compile "$file" "$@"
    local rc=$?
    set -e
    if [[ $rc -eq 0 ]]; then
        echo "Expected failure for $file" >&2
        exit 1
    fi
    if ! grep -Eiq -- "$pattern" "$TMP_DIR/$file.err" "$TMP_DIR/$file.out"; then
        echo "Expected diagnostic '$pattern' for $file" >&2
        print_diagnostics "$file"
        exit 1
    fi
}

write_case runtime_if_without_else.ct \
    "Contract RuntimeIfWithoutElse:" \
    "    def main(selector: number):" \
    "        if selector == 1:" \
    "            EqualVerify(1, 1)" \
    "        Return 1"

write_case runtime_notif_without_else.ct \
    "Contract RuntimeNotIfWithoutElse:" \
    "    def main(selector: number):" \
    "        if selector != 1:" \
    "            EqualVerify(1, 1)" \
    "        Return 1"

write_case static_range_if_without_else.ct \
    "Contract StaticRangeIfWithoutElse:" \
    "    def main():" \
    "        for i in Range(2):" \
    "            if i == 0:" \
    "                EqualVerify(1, 1)" \
    "        Return 1"

write_case literal_if_without_else.ct \
    "Contract LiteralIfWithoutElse:" \
    "    def main():" \
    "        if 0:" \
    "            EqualVerify(0, 1)" \
    "        if 1:" \
    "            EqualVerify(1, 1)" \
    "        Return 1"

write_case terminal_if_without_else.ct \
    "Contract TerminalIfWithoutElse:" \
    "    def main(flag: bool):" \
    "        if flag:" \
    "            Return 1" \
    "        Return 0"

write_case main_stack_imbalance_without_else.ct \
    "Contract MainStackImbalanceWithoutElse:" \
    "    def main(value: number, flag: bool):" \
    "        if flag:" \
    "            temporary = value.Clone()" \
    "            Keep(temporary)" \
    "        Return value"

write_case fixed_storage_change_without_else.ct \
    "Contract FixedStorageChangeWithoutElse:" \
    "    def main(flag: bool):" \
    "        value: number = 1" \
    "        if flag:" \
    "            value = 2" \
    "        Return value"

write_case local_fixed_without_else.ct \
    "Contract LocalFixedWithoutElse:" \
    "    def main(flag: bool):" \
    "        if flag:" \
    "            local: number = 7" \
    "            EqualVerify(local, 7)" \
    "        Return 1"

write_case alt_restored_without_else.ct \
    "Contract AltRestoredWithoutElse:" \
    "    def main(value: number, flag: bool):" \
    "        if flag:" \
    "            temporary = value.Clone()" \
    "            SetAlt(temporary)" \
    "            SetMain(temporary)" \
    "            Delete(temporary)" \
    "        Return value"

write_case alt_imbalance_without_else.ct \
    "Contract AltImbalanceWithoutElse:" \
    "    def main(value: number, flag: bool):" \
    "        if flag:" \
    "            temporary = value.Clone()" \
    "            SetAlt(temporary)" \
    "            Keep(temporary)" \
    "        Return value"

write_case nested_dependent_range.ct \
    "Contract NestedDependentRange:" \
    "    def main():" \
    "        for i in Range(3):" \
    "            for j in Range(i + 1):" \
    "                EqualVerify(1, 1)" \
    "        Return 1"

write_case static_incremented_array_index.ct \
    "Contract StaticIncrementedArrayIndex:" \
    "    def main(values: number[2], source: number[2]):" \
    "        j: number = 0" \
    "        for i in Range(2):" \
    "            values[j] = source[i].Clone()" \
    "            Keep(values[j])" \
    "            j = j + 1" \
    "        Delete(values)" \
    "        Delete(source)" \
    "        Return(j)"

write_case runtime_scalar_move_once.ct \
    "Contract RuntimeScalarMoveOnce:" \
    "    def main(value: number):" \
    "        next = value + 1" \
    "        Keep(value)" \
    "        Return(next)"

write_case empty_range_preserves_target.ct \
    "Contract EmptyRangePreservesTarget:" \
    "    def main():" \
    "        i: number = 7" \
    "        for i in Range(0):" \
    "            EqualVerify(0, 1)" \
    "        if i == 7:" \
    "            EqualVerify(1, 1)" \
    "        Return 1"

write_case negative_step_range.ct \
    "Contract NegativeStepRange:" \
    "    def main():" \
    "        for i in Range(5, 0, -2):" \
    "            EqualVerify(1, 1)" \
    "        Return 1"

write_case excessive_static_range.ct \
    "Contract ExcessiveStaticRange:" \
    "    def main():" \
    "        for i in Range(100001):" \
    "            EqualVerify(1, 1)" \
    "        Return 1"

write_case zero_step_range.ct \
    "Contract ZeroStepRange:" \
    "    def main():" \
    "        for i in Range(0, 1, 0):" \
    "            EqualVerify(1, 1)" \
    "        Return 1"

write_case overflow_range_expression.ct \
    "Contract OverflowRangeExpression:" \
    "    def main():" \
    "        for i in Range(9223372036854775807 + 1):" \
    "            EqualVerify(1, 1)" \
    "        Return 1"

write_case overflow_subtraction_expression.ct \
    "Contract OverflowSubtractionExpression:" \
    "    def main():" \
    "        Return (-9223372036854775807 - 1) - 1"

write_case overflow_multiplication_expression.ct \
    "Contract OverflowMultiplicationExpression:" \
    "    def main():" \
    "        Return 9223372036854775807 * 2"

write_case overflow_division_expression.ct \
    "Contract OverflowDivisionExpression:" \
    "    def main():" \
    "        Return (-9223372036854775807 - 1) / -1"

write_case overflow_negation_expression.ct \
    "Contract OverflowNegationExpression:" \
    "    def main():" \
    "        Return -(-9223372036854775807 - 1)"

write_case delete_literal_binding.ct \
    "Contract DeleteLiteralBinding:" \
    "    def main():" \
    "        value: number = 7" \
    "        Delete(value)" \
    "        Return 1"

write_case stale_loop_target.ct \
    "Contract StaleLoopTarget:" \
    "    def main():" \
    "        i: number = 7" \
    "        for i in Range(3):" \
    "            if i == 1:" \
    "                EqualVerify(1, 1)" \
    "        Return 1"

write_case non_numeric_loop_target.ct \
    "Contract NonNumericLoopTarget:" \
    "    def main(i: hex20):" \
    "        for i in Range(1):" \
    "            EqualVerify(1, 1)" \
    "        Return 1"

write_case loop_target_runtime_rebind.ct \
    "Contract LoopTargetRuntimeRebind:" \
    "    def main(value: number):" \
    "        i: number = 7" \
    "        for i in Range(2):" \
    "            i = value.Clone()" \
    "        Return i"

write_case early_return_large_range.ct \
    "Contract EarlyReturnLargeRange:" \
    "    def main():" \
    "        for i in Range(100000):" \
    "            Return 1"

write_case range_index_condition_depth.ct \
    "Contract RangeIndexConditionDepth:" \
    "    def main(values: bool[1], other: bool):" \
    "        for i in Range(1):" \
    "            if values[i]:" \
    "                Return 11" \
    "        Return 22"

write_case literal_index_condition_depth.ct \
    "Contract LiteralIndexConditionDepth:" \
    "    def main(values: bool[1], other: bool):" \
    "        if values[0]:" \
    "            Return 11" \
    "        Return 22"

write_case terminal_static_range.ct \
    "Contract TerminalStaticRange:" \
    "    def main(value: number):" \
    "        for i in Range(1):" \
    "            Return(value)" \
    "        Return(value)"

write_case terminal_static_if.ct \
    "Contract TerminalStaticIf:" \
    "    def main(value: number):" \
    "        if 1:" \
    "            Return(value)" \
    "        Return(value)"

expect_success runtime_if_without_else.ct
expect_success runtime_notif_without_else.ct
expect_success static_range_if_without_else.ct
expect_success literal_if_without_else.ct
expect_success terminal_if_without_else.ct
expect_failure main_stack_imbalance_without_else.ct \
    "if without else changes main-stack state"
expect_failure fixed_storage_change_without_else.ct \
    "if without else changes compiler storage state"
expect_success local_fixed_without_else.ct
expect_success alt_restored_without_else.ct --asa
expect_failure alt_imbalance_without_else.ct \
    "if without else changes alternative-stack state" --asa
expect_success nested_dependent_range.ct
expect_success static_incremented_array_index.ct
expect_failure runtime_scalar_move_once.ct \
    "value.*consumed.*move semantics violation"
expect_success empty_range_preserves_target.ct
expect_success negative_step_range.ct
expect_failure excessive_static_range.ct "exceeds limit 100000"
expect_failure zero_step_range.ct "range step must not be zero"
expect_failure overflow_range_expression.ct "integer addition overflow"
expect_failure overflow_subtraction_expression.ct \
    "integer subtraction overflow"
expect_failure overflow_multiplication_expression.ct \
    "integer multiplication overflow"
expect_failure overflow_division_expression.ct "integer division overflow"
expect_failure overflow_negation_expression.ct "integer negation overflow"
expect_success delete_literal_binding.ct
expect_success stale_loop_target.ct
expect_failure non_numeric_loop_target.ct "must be a numeric scalar"
expect_success loop_target_runtime_rebind.ct
expect_success early_return_large_range.ct
expect_success range_index_condition_depth.ct
expect_success literal_index_condition_depth.ct
expect_success terminal_static_range.ct
expect_success terminal_static_if.ct

python3 - "$TMP_DIR" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])

def tokens(name):
    with (root / f"{name}.json").open(encoding="utf-8") as handle:
        return json.load(handle)["lock"]["asm"].split()

runtime_if = tokens("runtime_if_without_else")
assert runtime_if.count("OP_IF") == 1, runtime_if
assert "OP_ELSE" not in runtime_if, runtime_if
assert runtime_if.count("OP_ENDIF") == 1, runtime_if

runtime_notif = tokens("runtime_notif_without_else")
assert runtime_notif.count("OP_NOTIF") == 1, runtime_notif
assert "OP_ELSE" not in runtime_notif, runtime_notif

for name in ("static_range_if_without_else", "literal_if_without_else",
             "empty_range_preserves_target", "stale_loop_target"):
    code = tokens(name)
    for opcode in ("OP_IF", "OP_NOTIF", "OP_ELSE", "OP_ENDIF"):
        assert opcode not in code, (name, code)

assert tokens("static_range_if_without_else").count("OP_EQUALVERIFY") == 1
assert tokens("literal_if_without_else").count("OP_EQUALVERIFY") == 1
assert tokens("nested_dependent_range").count("OP_EQUALVERIFY") == 6
assert "OP_WITHIN" not in tokens("static_incremented_array_index")
assert tokens("empty_range_preserves_target").count("OP_EQUALVERIFY") == 1
assert tokens("negative_step_range").count("OP_EQUALVERIFY") == 3
assert tokens("stale_loop_target").count("OP_EQUALVERIFY") == 1
assert tokens("early_return_large_range").count("OP_RETURN") == 1
range_index = tokens("range_index_condition_depth")
literal_index = tokens("literal_index_condition_depth")
assert range_index[:2] == ["OP_SWAP", "OP_IF"], range_index
assert range_index == literal_index, (range_index, literal_index)
assert tokens("terminal_static_range").count("OP_RETURN") == 1
assert tokens("terminal_static_if").count("OP_RETURN") == 1
PY

if [[ ${APC_BUILD_DEBUGGER:-1} == 1 ]]; then
    for file in range_index_condition_depth.ct \
        literal_index_condition_depth.ct; do
        for scenario in "1 0 11" "0 1 22"; do
            read -r value other expected <<<"$scenario"
            output="$TMP_DIR/${file%.ct}_${value}_${other}.run.out"
            if ! "$COMPILER" -l error run "$TMP_DIR/$file" main \
                "$value" "$other" >"$output" 2>&1; then
                echo "Expected runtime success for $file ($value, $other)" >&2
                sed -n '1,160p' "$output" >&2 || true
                exit 1
            fi
            if ! grep -q "status: finished" "$output" ||
               ! grep -q "int=$expected" "$output"; then
                echo "Expected $file ($value, $other) to return $expected" >&2
                sed -n '1,160p' "$output" >&2 || true
                exit 1
            fi
        done
    done
fi

echo "Range/no-else compiler regression tests passed"

#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)

if [[ $# -gt 1 ]]; then
    echo "Usage: $0 [/path/to/utxo_compiler]" >&2
    exit 2
fi

COMPILER=${1:-${APC_COMPILER:-"$REPO_ROOT/build/bin/utxo_compiler"}}
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

run_compile() {
    local file=$1
    shift
    (
        cd "$TMP_DIR"
        "$COMPILER" "$@" "$file" >"$file.out" 2>"$file.err"
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

expect_success_asa() {
    local file=$1
    local stem=${file%.ct}

    if ! run_compile "$file" --asa; then
        echo "Expected success for $file with --asa" >&2
        print_diagnostics "$file"
        exit 1
    fi

    if [[ ! -s "$TMP_DIR/$stem.json" ]]; then
        echo "Expected JSON output for $file with --asa" >&2
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

write_case size_borrow.ct \
    "Contract SizeBorrow:" \
    "    def main(value: string):" \
    "        length = Size(value)" \
    "        Delete(length)" \
    "        Return(value)"

write_case local_uint64_array.ct \
    "Contract LocalUint64Array:" \
    "    def main(data: string):" \
    "        amount: uint64[6] = data.Slice(0, 48)" \
    "        total = Push(0)" \
    "        for i in Range(5, -1, -1):" \
    "            total = BinToNum(amount[i]) + total" \
    "        Delete(amount)" \
    "        Return(total)"

write_case use_after_delete.ct \
    "Contract UseAfterDelete:" \
    "    def main(data: string):" \
    "        amount: uint64[6] = data.Slice(0, 48)" \
    "        Delete(amount)" \
    "        Return(amount[0])"

write_case size_after_move.ct \
    "Contract SizeAfterMove:" \
    "    def main(value: string):" \
    "        hashed = Sha256(value)" \
    "        Return(Size(value))"

write_case delete_after_use.ct \
    "Contract DeleteAfterUse:" \
    "    def main(value: string):" \
    "        converted = BinToNum(value)" \
    "        Delete(value)" \
    "        Return(converted)"

write_case branch_element_delete.ct \
    "Contract BranchElementDelete:" \
    "    def main(data: string, flag: bool):" \
    "        amount: uint64[2] = data.Slice(0, 16)" \
    "        if flag:" \
    "            Delete(amount[0])" \
    "        Return(amount[0])"

write_case size_after_array_delete.ct \
    "Contract SizeAfterArrayDelete:" \
    "    def main(data: string):" \
    "        amount: uint64[2] = data.Slice(0, 16)" \
    "        Delete(amount)" \
    "        Return(Size(amount[0]))"

write_case keep_borrow.ct \
    "Contract KeepBorrow:" \
    "    def main(value: string):" \
    "        Keep(value)" \
    "        Keep(value)" \
    "        Return(value)"

write_case keep_after_delete.ct \
    "Contract KeepAfterDelete:" \
    "    def main(value: string):" \
    "        Delete(value)" \
    "        Keep(value)" \
    "        Return(1)"

write_case scalar_copy_borrow.ct \
    "Contract ScalarCopyBorrow:" \
    "    def main(source: number, target: number):" \
    "        target = source" \
    "        Return(source)"

write_case field_copy_borrow.ct \
    "Contract FieldCopyBorrow:" \
    "    Struct Data:" \
    "        x: number" \
    "    def main(source: number, data: Data):" \
    "        data.x = source" \
    "        Return(source)"

write_case array_copy_borrow.ct \
    "Contract ArrayCopyBorrow:" \
    "    def main(source: number, values: number[1]):" \
    "        values[0] = source" \
    "        Return(source)"

write_case first_binding_moves.ct \
    "Contract FirstBindingMoves:" \
    "    def main(source: number):" \
    "        target = source" \
    "        Return(source)"

write_case uninitialized_field_moves.ct \
    "Contract UninitializedFieldMoves:" \
    "    Struct Data:" \
    "        x: number" \
    "    def main(source: number):" \
    "        data: Data" \
    "        data.x = source" \
    "        Return(source)"

write_case copy_from_consumed.ct \
    "Contract CopyFromConsumed:" \
    "    def main(source: string, target: string):" \
    "        hashed = Sha256(source)" \
    "        target = source" \
    "        Return(hashed)"

write_case terminal_then_branch.ct \
    "Contract TerminalThenBranch:" \
    "    def main(flag: bool, value: string):" \
    "        if flag:" \
    "            Return(value)" \
    "        else:" \
    "            marker = Push(1)" \
    "            Delete(marker)" \
    "        Return(value)"

write_case terminal_else_branch.ct \
    "Contract TerminalElseBranch:" \
    "    def main(flag: bool, value: string):" \
    "        if flag:" \
    "            marker = Push(1)" \
    "            Delete(marker)" \
    "        else:" \
    "            Return(value)" \
    "        Return(value)"

write_case terminal_both_branches.ct \
    "Contract TerminalBothBranches:" \
    "    def main(flag: bool, value: string):" \
    "        if flag:" \
    "            Return(value)" \
    "        else:" \
    "            Return(value)" \
    "        Return(value)"

write_case terminal_static_loop.ct \
    "Contract TerminalStaticLoop:" \
    "    def main(value: string):" \
    "        for i in Range(2):" \
    "            Return(value)"

write_case zero_iteration_falls_through.ct \
    "Contract ZeroIterationFallsThrough:" \
    "    def main(value: string):" \
    "        for i in Range(0):" \
    "            Return(value)" \
    "        Return(value)"

write_case reachable_branch_consumes.ct \
    "Contract ReachableBranchConsumes:" \
    "    def main(flag: bool, value: string):" \
    "        if flag:" \
    "            Delete(value)" \
    "        else:" \
    "            marker = Push(1)" \
    "            Delete(marker)" \
    "        Return(value)"

write_case if_without_else_alt_restored.ct \
    "Contract IfWithoutElseAltRestored:" \
    "    def main(value: number, flag: bool):" \
    "        if flag:" \
    "            temporary = value.Clone()" \
    "            SetAlt(temporary)" \
    "            SetMain(temporary)" \
    "            Delete(temporary)" \
    "        Return(value)"

write_case if_without_else_alt_imbalance.ct \
    "Contract IfWithoutElseAltImbalance:" \
    "    def main(value: number, flag: bool):" \
    "        if flag:" \
    "            temporary = value.Clone()" \
    "            SetAlt(temporary)" \
    "            Keep(temporary)" \
    "        Return(value)"

write_case if_without_else_lowercase_return.ct \
    "Contract IfWithoutElseLowercaseReturn:" \
    "    def main(value: number, flag: bool):" \
    "        if flag:" \
    "            return value"

write_case if_without_else_private_lowercase_return.ct \
    "Contract IfWithoutElsePrivateLowercaseReturn:" \
    "    def _select(value: number, flag: bool):" \
    "        if flag:" \
    "            return value" \
    "    def main(value: number, flag: bool):" \
    "        result = _select(value, flag)" \
    "        Return(result)"

write_case if_else_lowercase_return_isolated.ct \
    "Contract IfElseLowercaseReturnIsolated:" \
    "    def _select(value: number, flag: bool):" \
    "        if flag:" \
    "            return value" \
    "        else:" \
    "            return value" \
    "    def main(value: number, flag: bool):" \
    "        result = _select(value, flag)" \
    "        Return(result)"

write_case lowercase_literal_return.ct \
    "Contract LowercaseLiteralReturn:" \
    "    def main():" \
    "        return 1"

write_case lowercase_expression_return.ct \
    "Contract LowercaseExpressionReturn:" \
    "    def main(value: number):" \
    "        return value + 1"

write_case lowercase_void_return.ct \
    "Contract LowercaseVoidReturn:" \
    "    def main(value: string, marker: number):" \
    "        return Delete(value)"

write_case lowercase_void_private_return.ct \
    "Contract LowercaseVoidPrivateReturn:" \
    "    def _drop(value: string):" \
    "        Delete(value)" \
    "    def main(value: string, marker: number):" \
    "        return _drop(value)"

write_case struct_return_direct.ct \
    "Contract StructReturnDirect:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def main(value: Pair):" \
    "        Return(value.left - value.right)"

write_case struct_return_same_name.ct \
    "Contract StructReturnSameName:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _identity(data: Pair):" \
    "        return data" \
    "    def main(value: Pair):" \
    "        value = _identity(value)" \
    "        Return(value.left - value.right)"

write_case struct_return_fresh_direct.ct \
    "Contract StructReturnFreshDirect:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def main(source: Pair):" \
    "        target: Pair" \
    "        target = source" \
    "        Return(target.left - target.right)"

write_case struct_return_fresh.ct \
    "Contract StructReturnFresh:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _identity(data: Pair):" \
    "        return data" \
    "    def main(source: Pair):" \
    "        target: Pair" \
    "        target = _identity(source)" \
    "        Return(target.left - target.right)"

write_case struct_return_typed_init.ct \
    "Contract StructReturnTypedInit:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _identity(data: Pair):" \
    "        return data" \
    "    def main(source: Pair):" \
    "        target: Pair = _identity(source)" \
    "        Return(target.left - target.right)"

write_case struct_return_nested_direct.ct \
    "Contract StructReturnNestedDirect:" \
    "    Struct Inner:" \
    "        amount: number" \
    "    Struct Envelope:" \
    "        inner: Inner" \
    "        slots: uint64[2]" \
    "    def main(input: Envelope):" \
    "        length = Size(input.slots)" \
    "        Delete(length)" \
    "        Return(input.inner.amount)"

write_case struct_return_nested.ct \
    "Contract StructReturnNested:" \
    "    Struct Inner:" \
    "        amount: number" \
    "    Struct Envelope:" \
    "        inner: Inner" \
    "        slots: uint64[2]" \
    "    def _inner(payload: Envelope):" \
    "        return payload" \
    "    def _outer(value: Envelope):" \
    "        return _inner(value)" \
    "    def main(input: Envelope):" \
    "        input = _outer(input)" \
    "        length = Size(input.slots)" \
    "        Delete(length)" \
    "        Return(input.inner.amount)"

write_case struct_return_fixed.ct \
    "Contract StructReturnFixed:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _make():" \
    "        value: Pair = {7, 3}" \
    "        return value" \
    "    def main():" \
    "        result = _make()" \
    "        Return(result.left - result.right)"

write_case struct_return_fixed_direct.ct \
    "Contract StructReturnFixedDirect:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def main():" \
    "        result: Pair = {7, 3}" \
    "        Return(result.left - result.right)"

write_case struct_return_typed_same_name.ct \
    "Contract StructReturnTypedSameName:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _make():" \
    "        value: Pair = {7, 3}" \
    "        return value" \
    "    def main():" \
    "        value: Pair = _make()" \
    "        Return(value.left - value.right)"

write_case struct_return_alt.ct \
    "Contract StructReturnAlt:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _roundtrip(value: Pair):" \
    "        return value" \
    "    def main(input: Pair):" \
    "        SetAlt(input.right)" \
    "        marker1 = Push(9)" \
    "        SetAlt(marker1)" \
    "        marker2 = Push(10)" \
    "        SetAlt(marker2)" \
    "        input = _roundtrip(input)" \
    "        SetMain(marker2)" \
    "        Delete(marker2)" \
    "        SetMain(marker1)" \
    "        Delete(marker1)" \
    "        Return(input.left - input.right)"

write_case alt_scope_cleanup.ct \
    "Contract AltScopeCleanup:" \
    "    def _identity(value: number):" \
    "        local = Push(7)" \
    "        SetAlt(local)" \
    "        return value" \
    "    def main():" \
    "        result = _identity(9)" \
    "        Return(result)"

write_case private_alt_param_cleanup.ct \
    "Contract PrivateAltParamCleanup:" \
    "    def _drop(first: number, second: number):" \
    "        SetAlt(second)" \
    "        SetAlt(first)" \
    "    def main():" \
    "        survivor = Push(8)" \
    "        _drop(9, 10)" \
    "        Return(survivor)"

write_case private_alt_struct_param_cleanup.ct \
    "Contract PrivateAltStructParamCleanup:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _drop(value: Pair):" \
    "        SetAlt(value.right)" \
    "        SetAlt(value.left)" \
    "    def main(input: Pair):" \
    "        _drop(input)" \
    "        Return(1)"

write_case private_alt_local_escape.ct \
    "Contract PrivateAltLocalEscape:" \
    "    def _stash():" \
    "        controller = Push(7)" \
    "        SetAlt(controller)" \
    "    def _noop():" \
    "        marker = Push(1)" \
    "        Delete(marker)" \
    "    def main():" \
    "        _stash()" \
    "        _noop()" \
    "        SetMain(controller)" \
    "        Return(controller)"

write_case private_alt_caller_binding.ct \
    "Contract PrivateAltCallerBinding:" \
    "    def _stash(value: number):" \
    "        SetAlt(value)" \
    "    def main(value: number):" \
    "        _stash(value)" \
    "        SetMain(value)" \
    "        Return(value)"

write_case private_alt_struct_array_whole_escape.ct \
    "Contract PrivateAltStructArrayWholeEscape:" \
    "    Struct Input:" \
    "        Data: { txid: hex32, vout: hex4 }" \
    "    def _stash(value: hex36):" \
    "        inputData: Input[1]" \
    "        inputData[0] = value" \
    "        SetAlt(inputData[0])" \
    "    def _restore():" \
    "        SetMain(inputData[0])" \
    "        result = Size(inputData[0])" \
    "        Delete(inputData[0])" \
    "        return result" \
    "    def main(value: hex36):" \
    "        _stash(value)" \
    "        result = _restore()" \
    "        Return(result)"

write_case private_alt_struct_array_flat_escape.ct \
    "Contract PrivateAltStructArrayFlatEscape:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _stash(values: Pair[2]):" \
    "        SetAlt(values[1])" \
    "        marker = Push(9)" \
    "        SetAlt(marker)" \
    "    def _restore():" \
    "        SetMain(values[1])" \
    "        SetMain(marker)" \
    "        Delete(marker)" \
    "        return values[1].left - values[1].right" \
    "    def main(items: Pair[2]):" \
    "        _stash(items)" \
    "        result = _restore()" \
    "        Return(result)"

write_case private_alt_struct_array_repeated_escape.ct \
    "Contract PrivateAltStructArrayRepeatedEscape:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _stash(values: Pair[1]):" \
    "        SetAlt(values[0])" \
    "    def _restore():" \
    "        SetMain(values[0])" \
    "        return values[0].left - values[0].right" \
    "    def main(first: Pair[1], second: Pair[1]):" \
    "        _stash(first)" \
    "        _stash(second)" \
    "        secondResult = _restore()" \
    "        firstResult = _restore()" \
    "        Return(secondResult - firstResult)"

write_case private_alt_struct_array_repeated_same_consumer.ct \
    "Contract PrivateAltStructArrayRepeatedSameConsumer:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _stash(values: Pair[1]):" \
    "        SetAlt(values[0])" \
    "    def _restoreBoth():" \
    "        SetMain(values[0])" \
    "        secondResult = values[0].left - values[0].right" \
    "        SetMain(values[0])" \
    "        firstResult = values[0].left - values[0].right" \
    "        return secondResult - firstResult" \
    "    def main(first: Pair[1], second: Pair[1]):" \
    "        _stash(first)" \
    "        _stash(second)" \
    "        result = _restoreBoth()" \
    "        Return(result)"

write_case private_alt_struct_array_physical_name_collision.ct \
    "Contract PrivateAltStructArrayPhysicalNameCollision:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _stashLocal(firstLeft: number, firstRight: number):" \
    "        values: Pair[1] = [{firstLeft, firstRight}]" \
    "        SetAlt(values[0])" \
    "    def _stash(values: Pair[1]):" \
    "        SetAlt(values[0])" \
    "    def _restoreBoth():" \
    "        SetMain(values[0])" \
    "        secondResult = values[0].left - values[0].right" \
    "        SetMain(values[0])" \
    "        firstResult = values[0].left - values[0].right" \
    "        return secondResult - firstResult" \
    "    def main(firstLeft: number, firstRight: number, second: Pair[1]):" \
    "        _stashLocal(firstLeft, firstRight)" \
    "        _stash(second)" \
    "        result = _restoreBoth()" \
    "        Return(result)"

write_case private_alt_struct_array_partial_latest.ct \
    "Contract PrivateAltStructArrayPartialLatest:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _stash(values: Pair[1]):" \
    "        SetAlt(values[0])" \
    "    def _stashPartial(values: Pair[1]):" \
    "        SetAlt(values[0])" \
    "        SetMain(values[0].left)" \
    "    def _restore():" \
    "        SetMain(values[0])" \
    "        return values[0].left - values[0].right" \
    "    def main(first: Pair[1], second: Pair[1]):" \
    "        _stash(first)" \
    "        _stashPartial(second)" \
    "        result = _restore()" \
    "        Return(result)"

write_case private_alt_struct_array_uncalled.ct \
    "Contract PrivateAltStructArrayUncalled:" \
    "    Struct Input:" \
    "        Data: hex36" \
    "    def _stash(value: hex36):" \
    "        inputData: Input[1]" \
    "        inputData[0] = value" \
    "        SetAlt(inputData[0])" \
    "    def main():" \
    "        SetMain(inputData[0])" \
    "        Return(1)"

write_case private_alt_struct_array_double_restore.ct \
    "Contract PrivateAltStructArrayDoubleRestore:" \
    "    Struct Input:" \
    "        Data: hex36" \
    "    def _stash(value: hex36):" \
    "        inputData: Input[1]" \
    "        inputData[0] = value" \
    "        SetAlt(inputData[0])" \
    "    def _restore():" \
    "        SetMain(inputData[0])" \
    "        SetMain(inputData[0])" \
    "    def main(value: hex36):" \
    "        _stash(value)" \
    "        _restore()" \
    "        Return(1)"

write_case private_alt_struct_array_dynamic_index.ct \
    "Contract PrivateAltStructArrayDynamicIndex:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _stash(values: Pair[2], index: number):" \
    "        SetAlt(values[index])" \
    "    def main(values: Pair[2], index: number):" \
    "        _stash(values, index)" \
    "        Return(1)"

write_case missing_alt_binding.ct \
    "Contract MissingAltBinding:" \
    "    def main(value: number):" \
    "        SetMain(value)" \
    "        Return(value)"

write_case uncalled_alt_producer.ct \
    "Contract UncalledAltProducer:" \
    "    def _stash():" \
    "        controller = Push(7)" \
    "        SetAlt(controller)" \
    "    def main():" \
    "        SetMain(controller)" \
    "        Return(controller)"

write_case misspelled_alt_binding.ct \
    "Contract MisspelledAltBinding:" \
    "    def _stash():" \
    "        controller = Push(7)" \
    "        SetAlt(controller)" \
    "    def main():" \
    "        _stash()" \
    "        SetMain(controler)" \
    "        Return(controler)"

write_case public_alt_across_private_definition.ct \
    "Contract PublicAltAcrossPrivateDefinition:" \
    "    def stage():" \
    "        controller = Push(7)" \
    "        SetAlt(controller)" \
    "    def _noop(value: number):" \
    "        return value" \
    "    def main():" \
    "        SetMain(controller)" \
    "        Return(controller)"

write_case struct_return_then_void_direct.ct \
    "Contract StructReturnThenVoidDirect:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _noop(data: Pair):" \
    "        marker = Push(1)" \
    "        Delete(marker)" \
    "    def main(value: Pair):" \
    "        _noop(value)" \
    "        Return(1)"

write_case struct_return_then_void.ct \
    "Contract StructReturnThenVoid:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _identity(data: Pair):" \
    "        return data" \
    "    def _noop(data: Pair):" \
    "        marker = Push(1)" \
    "        Delete(marker)" \
    "    def main(value: Pair):" \
    "        value = _identity(value)" \
    "        _noop(value)" \
    "        Return(1)"

write_case struct_return_local_cleanup_direct.ct \
    "Contract StructReturnLocalCleanupDirect:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def main(value: Pair):" \
    "        marker = Push(9)" \
    "        Delete(marker)" \
    "        Return(value.left - value.right)"

write_case struct_return_local_cleanup.ct \
    "Contract StructReturnLocalCleanup:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _identity(data: Pair):" \
    "        marker = Push(9)" \
    "        return data" \
    "    def main(value: Pair):" \
    "        value = _identity(value)" \
    "        Return(value.left - value.right)"

write_case struct_return_param_cleanup_direct.ct \
    "Contract StructReturnParamCleanupDirect:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def main(value: Pair, marker: number):" \
    "        Delete(marker)" \
    "        Return(value.left - value.right)"

write_case struct_return_param_cleanup.ct \
    "Contract StructReturnParamCleanup:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _select(data: Pair, marker: number):" \
    "        return data" \
    "    def main(value: Pair, marker: number):" \
    "        value = _select(value, marker)" \
    "        Return(value.left - value.right)"

write_case struct_return_field_update_direct.ct \
    "Contract StructReturnFieldUpdateDirect:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def main(value: Pair):" \
    "        value.left = 7" \
    "        Return(value.left - value.right)"

write_case struct_return_field_update.ct \
    "Contract StructReturnFieldUpdate:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _update(data: Pair):" \
    "        data.left = 7" \
    "        return data" \
    "    def main(value: Pair):" \
    "        value = _update(value)" \
    "        Return(value.left - value.right)"

write_case unused_struct_return.ct \
    "Contract UnusedStructReturn:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _identity(data: Pair):" \
    "        return data" \
    "    def main(value: Pair):" \
    "        _identity(value)" \
    "        Return(1)"

write_case struct_multi_return.ct \
    "Contract StructMultiReturn:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _values(data: Pair):" \
    "        return {data, 1}" \
    "    def main(value: Pair):" \
    "        {result, marker} = _values(value)" \
    "        Return(marker)"

write_case scalar_multi_return.ct \
    "Contract ScalarMultiReturn:" \
    "    def _values():" \
    "        return {7, 3}" \
    "    def main():" \
    "        {left, right} = _values()" \
    "        Return(left - right)"

write_case struct_multiple_returns.ct \
    "Contract StructMultipleReturns:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _values(data: Pair):" \
    "        return 1" \
    "        return data" \
    "    def main(value: Pair):" \
    "        value = _values(value)" \
    "        Return(1)"

write_case struct_return_binding_scope_direct.ct \
    "Contract StructReturnBindingScopeDirect:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def main(first: Pair, data: Pair):" \
    "        Return(data.left - data.right)"

write_case struct_return_binding_scope.ct \
    "Contract StructReturnBindingScope:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _identity(data: Pair):" \
    "        return data" \
    "    def main(first: Pair, data: Pair):" \
    "        first = _identity(first)" \
    "        Return(data.left - data.right)"

write_case incomplete_struct_return.ct \
    "Contract IncompleteStructReturn:" \
    "    Struct Pair:" \
    "        left: string" \
    "        right: string" \
    "    def _consume(value: Pair):" \
    "        digest = Sha256(value.left)" \
    "        Delete(digest)" \
    "        return value" \
    "    def main(input: Pair):" \
    "        input = _consume(input)" \
    "        Return(1)"

write_case struct_argument_below_blocker.ct \
    "Contract StructArgumentBelowBlocker:" \
    "    Struct Triple:" \
    "        first: number" \
    "        second: number" \
    "        third: number" \
    "    def _noop(data: Triple):" \
    "        marker = Push(1)" \
    "        Delete(marker)" \
    "    def main(value: Triple):" \
    "        blocker = Push(9)" \
    "        _noop(value)" \
    "        Return(blocker)"

write_case private_struct_array_argument.ct \
    "Contract PrivateStructArrayArgument:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _consume(items: Pair[2]):" \
    "        sum = items[0].left + items[0].right" \
    "        Delete(sum)" \
    "    def main(values: Pair[2]):" \
    "        blocker = Push(9)" \
    "        _consume(values)" \
    "        Return(blocker)"

write_case dynamic_slice_struct_direct.ct \
    "Contract DynamicSliceStructDirect:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def main(value: Pair, tape: string):" \
    "        tapeSize = Size(tape)" \
    "        suffix = tape.Slice(tapeSize - 9, -1)" \
    "        Delete(suffix)" \
    "        Return(value.left - value.right)"

write_case dynamic_slice_struct_routed.ct \
    "Contract DynamicSliceStructRouted:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _prepare(value: Pair, tape: string):" \
    "        tapeSize = Size(tape)" \
    "        suffix = tape.Slice(tapeSize - 9, -1)" \
    "        Delete(suffix)" \
    "        return value" \
    "    def _verify(value: Pair):" \
    "        return value.left - value.right" \
    "    def main(value: Pair, tape: string):" \
    "        value = _prepare(value, tape)" \
    "        result = _verify(value)" \
    "        Return(result)"

write_case static_loop_index_is_fixed.ct \
    "Contract StaticLoopIndexIsFixed:" \
    "    def main(amount: number[2], amount2: number[2]):" \
    "        for i in Range(2):" \
    "            NumEqualVerify(amount[i], amount2[i])" \
    "        Return(1)"

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

write_case static_computed_initial_index.ct \
    "Contract StaticComputedInitialIndex:" \
    "    def main(values: number[3]):" \
    "        one: number = 1" \
    "        index: number = one + 1" \
    "        Keep(values[index])" \
    "        Keep(values[index])" \
    "        Delete(values)" \
    "        Return(index)"

write_case static_branch_struct_array_index.ct \
    "Contract StaticBranchStructArrayIndex:" \
    "    Struct Input:" \
    "        Data: { txid: hex32, vout: hex4 }" \
    "    def _stash(value: hex36, choose: bool):" \
    "        inputData: Input[2]" \
    "        j: number = 0" \
    "        for i in Range(2):" \
    "            if choose.Clone():" \
    "                inputData[j] = value.Clone()" \
    "            else:" \
    "                inputData[j] = value.Clone()" \
    "            SetAlt(inputData[j])" \
    "            j = j + 1" \
    "    def _restore():" \
    "        SetMain(inputData[1])" \
    "        SetMain(inputData[0])" \
    "        Delete(inputData[1])" \
    "        Delete(inputData[0])" \
    "    def main(value: hex36, choose: bool):" \
    "        _stash(value, choose)" \
    "        _restore()" \
    "        Return(1)"

write_case static_branch_struct_array_missing_else.ct \
    "Contract StaticBranchStructArrayMissingElse:" \
    "    Struct Input:" \
    "        Data: { txid: hex32, vout: hex4 }" \
    "    def main(value: hex36, choose: bool):" \
    "        inputData: Input[1]" \
    "        if choose:" \
    "            inputData[0] = value" \
    "        SetAlt(inputData[0])" \
    "        Return(1)"

write_case runtime_array_write_stays_unsupported.ct \
    "Contract RuntimeArrayWriteStaysUnsupported:" \
    "    def main(values: number[2], index: number, value: number):" \
    "        values[index] = value" \
    "        Return(1)"

write_case immutable_suffix_after_return.ct \
    "Contract ImmutableSuffixAfterReturn:" \
    "    def main():" \
    "        Return(1)" \
    "        Push(0x1234)"

write_case unreachable_before_immutable_suffix.ct \
    "Contract UnreachableBeforeImmutableSuffix:" \
    "    def main():" \
    "        Return(1)" \
    "        marker = Push(7)" \
    "        Push(0x1234)"

write_case symbolic_immutable_suffix.ct \
    "Contract SymbolicImmutableSuffix:" \
    "    def main():" \
    "        Return(1)" \
    "        Push(self.count)"

write_case symbolic_push_before_return.ct \
    "Contract SymbolicPushBeforeReturn:" \
    "    def main():" \
    "        value = Push(self.count)" \
    "        Return(value)"

write_case branch_return_has_no_suffix.ct \
    "Contract BranchReturnHasNoSuffix:" \
    "    def main(flag: bool):" \
    "        if flag:" \
    "            Return(1)" \
    "        else:" \
    "            Return(0)" \
    "        Push(0x1234)"

write_case nested_return_has_no_symbolic_suffix.ct \
    "Contract NestedReturnHasNoSymbolicSuffix:" \
    "    def main(flag: bool):" \
    "        if flag:" \
    "            Return(1)" \
    "            Push(self.count)" \
    "        else:" \
    "            marker = Push(0)" \
    "            Delete(marker)" \
    "        Return(1)"

write_case nonfinal_function_has_no_suffix.ct \
    "Contract NonfinalFunctionHasNoSuffix:" \
    "    def first():" \
    "        Return(1)" \
    "        Push(self.count)" \
    "    def main():" \
    "        Return(1)"

PADDING_COLLISION_SUFFIX=$(printf 'ff%.0s' {1..61})
write_case immutable_suffix_padding_collision.ct \
    "Contract ImmutableSuffixPaddingCollision:" \
    "    def main():" \
    "        Return(1)" \
    "        Push(0x${PADDING_COLLISION_SUFFIX})"

cp "$REPO_ROOT/scrypt_rewrites/accumulatorMultiSig.ct" \
    "$TMP_DIR/accumulatorMultiSig.ct"

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
expect_failure first_binding_moves.ct "source" "consumed more than once"
expect_failure uninitialized_field_moves.ct "source" "consumed more than once"
expect_failure copy_from_consumed.ct "source" "consum|borrow"
expect_success terminal_then_branch.ct
expect_success terminal_else_branch.ct
expect_success terminal_both_branches.ct
expect_success terminal_static_loop.ct
expect_success zero_iteration_falls_through.ct
expect_failure reachable_branch_consumes.ct "value" "consum"
expect_success_asa if_without_else_alt_restored.ct
expect_failure if_without_else_alt_imbalance.ct --asa \
    "if without else changes alternative-stack state"
expect_failure if_without_else_lowercase_return.ct \
    "lowercase return in only one if branch" \
    "has no continuation"
expect_failure if_without_else_private_lowercase_return.ct \
    "lowercase return in only one if branch" \
    "has no continuation"
expect_success if_else_lowercase_return_isolated.ct
expect_success lowercase_literal_return.ct
expect_success lowercase_expression_return.ct
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
expect_failure unused_struct_return.ct "returned struct value is unused"
expect_failure struct_multi_return.ct \
    "struct values cannot participate" "multi-value return"
expect_success scalar_multi_return.ct
expect_failure struct_multiple_returns.ct \
    "struct-returning function" "additional lowercase return"
expect_success struct_return_binding_scope_direct.ct
expect_success struct_return_binding_scope.ct
expect_failure incomplete_struct_return.ct \
    "incomplete struct return" "input.left|value.left"
expect_success struct_argument_below_blocker.ct
expect_success private_struct_array_argument.ct
expect_success dynamic_slice_struct_direct.ct
expect_success dynamic_slice_struct_routed.ct
expect_success static_loop_index_is_fixed.ct
if grep -Eiq -- "Invalid rebinding: variable 'i'|generated script element" \
    "$TMP_DIR/static_loop_index_is_fixed.ct.err"; then
    echo "Static loop binding emitted a user-rebinding warning" >&2
    print_diagnostics static_loop_index_is_fixed.ct
    exit 1
fi
expect_success static_incremented_array_index.ct
if grep -q "OP_WITHIN" "$TMP_DIR/static_incremented_array_index.json"; then
    echo "Static incremented array index used runtime-index bytecode" >&2
    exit 1
fi
expect_success static_computed_initial_index.ct
expect_success_asa static_branch_struct_array_index.ct
if grep -q "OP_WITHIN" "$TMP_DIR/static_branch_struct_array_index.json"; then
    echo "Static branch array index used runtime-index bytecode" >&2
    exit 1
fi
expect_failure static_branch_struct_array_missing_else.ct --asa \
    "if without else changes main-stack state"
expect_failure runtime_array_write_stays_unsupported.ct \
    "Invalid array assignment index"
if ! (
    cd "$TMP_DIR"
    "$COMPILER" --asa accumulatorMultiSig.ct \
        >accumulatorMultiSig.ct.out 2>accumulatorMultiSig.ct.err
); then
    echo "Expected success for accumulatorMultiSig.ct with --asa" >&2
    print_diagnostics accumulatorMultiSig.ct
    exit 1
fi
if [[ ! -s "$TMP_DIR/accumulatorMultiSig.json" ]]; then
    echo "Expected JSON output for accumulatorMultiSig.ct" >&2
    print_diagnostics accumulatorMultiSig.ct
    exit 1
fi
if ! (
    cd "$TMP_DIR"
    "$COMPILER" --asa alt_scope_cleanup.ct \
        >alt_scope_cleanup.ct.out 2>alt_scope_cleanup.ct.err
); then
    echo "Expected success for alt_scope_cleanup.ct with --asa" >&2
    print_diagnostics alt_scope_cleanup.ct
    exit 1
fi
if [[ ! -s "$TMP_DIR/alt_scope_cleanup.json" ]]; then
    echo "Expected JSON output for alt_scope_cleanup.ct" >&2
    print_diagnostics alt_scope_cleanup.ct
    exit 1
fi
for file in private_alt_param_cleanup.ct private_alt_struct_param_cleanup.ct \
    private_alt_local_escape.ct private_alt_caller_binding.ct \
    private_alt_struct_array_whole_escape.ct \
    private_alt_struct_array_flat_escape.ct \
    private_alt_struct_array_repeated_escape.ct \
    private_alt_struct_array_repeated_same_consumer.ct \
    private_alt_struct_array_physical_name_collision.ct; do
    if ! (
        cd "$TMP_DIR"
        "$COMPILER" --asa "$file" >"$file.out" 2>"$file.err"
    ); then
        echo "Expected success for $file with --asa" >&2
        print_diagnostics "$file"
        exit 1
    fi
    stem=${file%.ct}
    if [[ ! -s "$TMP_DIR/$stem.json" ]]; then
        echo "Expected JSON output for $file" >&2
        print_diagnostics "$file"
        exit 1
    fi
done
python3 - "$TMP_DIR/private_alt_local_escape.json" \
    "$TMP_DIR/private_alt_caller_binding.json" \
    "$TMP_DIR/private_alt_struct_array_whole_escape.json" \
    "$TMP_DIR/private_alt_struct_array_flat_escape.json" <<'PY'
import json
import sys

for path in sys.argv[1:3]:
    with open(path, "r", encoding="utf-8") as f:
        tokens = json.load(f)["lock"]["asm"].split()
    assert tokens.count("OP_TOALTSTACK") == 1, (path, tokens)
    assert tokens.count("OP_FROMALTSTACK") == 1, (path, tokens)

with open(sys.argv[3], "r", encoding="utf-8") as f:
    whole_tokens = json.load(f)["lock"]["asm"].split()
assert whole_tokens.count("OP_TOALTSTACK") == 1, whole_tokens
assert whole_tokens.count("OP_FROMALTSTACK") == 1, whole_tokens

with open(sys.argv[4], "r", encoding="utf-8") as f:
    flat_tokens = json.load(f)["lock"]["asm"].split()
assert flat_tokens.count("OP_TOALTSTACK") >= 3, flat_tokens
assert flat_tokens.count("OP_FROMALTSTACK") >= 3, flat_tokens
PY
expect_failure missing_alt_binding.ct "alternate stack|altstack"
expect_failure uncalled_alt_producer.ct --asa "alternate stack|altstack"
expect_failure misspelled_alt_binding.ct --asa "alternate stack|altstack"
expect_failure private_alt_struct_array_uncalled.ct --asa \
    "alternate.stack|altstack|non-array"
expect_failure private_alt_struct_array_double_restore.ct --asa \
    "restore|alternate.stack|residency"
expect_failure private_alt_struct_array_dynamic_index.ct --asa \
    "compile-time index|multi-slot"
expect_failure private_alt_struct_array_partial_latest.ct --asa \
    "partially live|complete struct-array element"
expect_success public_alt_across_private_definition.ct
expect_success immutable_suffix_after_return.ct
expect_success unreachable_before_immutable_suffix.ct
expect_success symbolic_immutable_suffix.ct
expect_failure symbolic_push_before_return.ct "fixed byte length"
expect_success branch_return_has_no_suffix.ct
expect_success nested_return_has_no_symbolic_suffix.ct
expect_success nonfinal_function_has_no_suffix.ct
expect_success immutable_suffix_padding_collision.ct

python3 - "$TMP_DIR/if_without_else_alt_restored.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    tokens = json.load(f)["lock"]["asm"].split()

assert tokens.count("OP_IF") == 1, tokens
assert tokens.count("OP_NOTIF") == 0, tokens
assert tokens.count("OP_ELSE") == 0, tokens
assert tokens.count("OP_ENDIF") == 1, tokens
assert tokens.count("OP_TOALTSTACK") == 1, tokens
assert tokens.count("OP_FROMALTSTACK") == 1, tokens
PY

python3 - "$TMP_DIR/terminal_both_branches.json" \
    "$TMP_DIR/terminal_static_loop.json" \
    "$TMP_DIR/lowercase_literal_return.json" \
    "$TMP_DIR/lowercase_expression_return.json" \
    "$TMP_DIR/immutable_suffix_after_return.json" \
    "$TMP_DIR/unreachable_before_immutable_suffix.json" \
    "$TMP_DIR/symbolic_immutable_suffix.json" \
    "$TMP_DIR/branch_return_has_no_suffix.json" \
    "$TMP_DIR/nested_return_has_no_symbolic_suffix.json" \
    "$TMP_DIR/nonfinal_function_has_no_suffix.json" \
    "$TMP_DIR/immutable_suffix_padding_collision.json" <<'PY'
import json
import sys

def op_return_count(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)["lock"]["asm"].split().count("OP_RETURN")

assert op_return_count(sys.argv[1]) == 2, sys.argv[1]
assert op_return_count(sys.argv[2]) == 1, sys.argv[2]
assert op_return_count(sys.argv[3]) == 0, sys.argv[3]
assert op_return_count(sys.argv[4]) == 0, sys.argv[4]

for path in sys.argv[5:7]:
    with open(path, "r", encoding="utf-8") as f:
        lock = json.load(f)["lock"]
    tokens = lock["asm"].split()
    last_return = max(i for i, token in enumerate(tokens)
                      if token == "OP_RETURN")
    suffix = tokens[last_return + 1:]
    assert suffix and suffix[-1].lower() == "021234", (path, suffix)
    assert "OP_7" not in suffix, (path, suffix)
    assert lock["hex"].lower().endswith("021234"), path

with open(sys.argv[7], "r", encoding="utf-8") as f:
    symbolic_tokens = json.load(f)["lock"]["asm"].split()
symbolic_return = max(i for i, token in enumerate(symbolic_tokens)
                      if token == "OP_RETURN")
assert symbolic_tokens[symbolic_return + 1:], symbolic_tokens
assert symbolic_tokens[-1] == "<self.count>", symbolic_tokens[-10:]

for path in sys.argv[8:11]:
    with open(path, "r", encoding="utf-8") as f:
        tokens = json.load(f)["lock"]["asm"].split()
    assert "1234" not in tokens and "021234" not in tokens, \
        (path, tokens[-20:])
    assert "<self.count>" not in tokens, (path, tokens[-20:])

with open(sys.argv[11], "r", encoding="utf-8") as f:
    collision_lock = json.load(f)["lock"]
collision_payload = "ff" * 61
collision_encoding = "3d" + collision_payload
collision_tokens = collision_lock["asm"].split()
assert collision_tokens[-2:] == [collision_encoding, collision_encoding], \
    collision_tokens[-5:]
assert collision_lock["hex"].lower().endswith(collision_encoding * 2), \
    collision_lock["hex"][-260:]
PY

python3 - "$TMP_DIR/struct_argument_below_blocker.json" \
    "$TMP_DIR/dynamic_slice_struct_direct.json" \
    "$TMP_DIR/dynamic_slice_struct_routed.json" <<'PY'
import json
import sys

def lock(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)["lock"]

fallback = lock(sys.argv[1])
fallback_tokens = fallback["asm"].split()
expected_prefix = [
    "OP_9",
    "OP_SWAP", "OP_ROT", "OP_3", "OP_ROLL",
    "OP_ROT", "OP_DROP", "OP_NIP", "OP_DROP", "OP_RETURN",
]
assert fallback_tokens[:len(expected_prefix)] == expected_prefix, \
    fallback_tokens

direct = lock(sys.argv[2])
routed = lock(sys.argv[3])
assert routed["asm"] == direct["asm"], (direct["asm"], routed["asm"])
assert routed["hex"] == direct["hex"], (direct["hex"], routed["hex"])
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

for direct_index, routed_index in ((1, 2), (3, 4), (5, 6), (7, 8),
                                   (11, 12), (13, 14), (15, 9), (16, 17),
                                   (3, 18), (15, 19)):
    direct = lock(sys.argv[direct_index])
    routed = lock(sys.argv[routed_index])
    assert routed["hex"] == direct["hex"], (sys.argv[direct_index],
                                                sys.argv[routed_index])
    assert routed["asm"] == direct["asm"], (sys.argv[direct_index],
                                                sys.argv[routed_index])

fixed = lock(sys.argv[9])
alt = lock(sys.argv[10])
assert fixed["hex"] and fixed["asm"], sys.argv[9]
alt_tokens = alt["asm"].split()
assert alt["hex"] and alt_tokens.count("OP_FROMALTSTACK") >= 5, sys.argv[10]
PY

python3 - "$TMP_DIR/alt_scope_cleanup.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    lock = json.load(f)["lock"]

tokens = lock["asm"].split()
assert tokens.count("OP_TOALTSTACK") == 1, tokens
assert "OP_FROMALTSTACK" not in tokens, tokens
PY

python3 - "$TMP_DIR/private_alt_param_cleanup.json" \
    "$TMP_DIR/private_alt_struct_param_cleanup.json" <<'PY'
import json
import sys

for path in sys.argv[1:]:
    with open(path, "r", encoding="utf-8") as f:
        tokens = json.load(f)["lock"]["asm"].split()
    assert tokens.count("OP_TOALTSTACK") == 2, (path, tokens)
    assert "OP_FROMALTSTACK" not in tokens, (path, tokens)
PY

echo "Ownership regression checks passed."

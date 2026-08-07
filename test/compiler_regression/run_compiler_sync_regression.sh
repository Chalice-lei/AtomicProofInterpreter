#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
COMPILER=${APC_COMPILER:-"$REPO_ROOT/build/bin/utxo_compiler"}
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
        "$COMPILER" "$file" >"$file.out" 2>"$file.err"
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

write_case self_return_hex20.ct \
    "Contract SelfReturnHex20:" \
    "    def __init__(value: hex20):" \
    "        self.value = value" \
    "    def main():" \
    "        Return self.value"

write_case self_digit_return_hex20.ct \
    "Contract SelfDigitReturnHex20:" \
    "    def __init__(value2: hex20):" \
    "        self.value2 = value2" \
    "    def main():" \
    "        Return self.value2"

write_case push_self.ct \
    "Contract PushSelf:" \
    "    def main():" \
    "        x = Push(self.value)" \
    "        Return x"

write_case push_self_hex20.ct \
    "Contract PushSelfHex20:" \
    "    def __init__(value: hex20):" \
    "        self.value = value" \
    "    def main():" \
    "        x = Push(self.value)" \
    "        Return x"

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

write_case pushdata_asm.ct \
    "Contract PushDataAsm:" \
    "    def main():" \
    "        x = Push(20)" \
    "        Return x"

write_case dynamic_slice_start.ct \
    "Contract DynamicSliceStart:" \
    "    def main(data: string):" \
    "        size = Size(data)" \
    "        suffix = data.Slice(size - 9, -1)" \
    "        Return suffix"

write_case dynamic_slice_length.ct \
    "Contract DynamicSliceLength:" \
    "    def main(data: string):" \
    "        size = Size(data)" \
    "        prefix = data.Slice(0, size - 9)" \
    "        Return prefix"

write_case dynamic_slice_window.ct \
    "Contract DynamicSliceWindow:" \
    "    def main(data: string):" \
    "        size = Size(data)" \
    "        window = data.Slice(size - 9, 3)" \
    "        Return window"

write_case dynamic_slice_both.ct \
    "Contract DynamicSliceBoth:" \
    "    def main(data: string, start: number, length: number):" \
    "        window = data.Slice(start, length)" \
    "        Return window"

write_case duplicate_dynamic_slice_bound.ct \
    "Contract DuplicateDynamicSliceBound:" \
    "    def _slice(data: string, bound: number):" \
    "        return data.Slice(bound, bound)" \
    "    def main(data: string, bound: number):" \
    "        result = _slice(data, bound)" \
    "        Return result"

write_case private_param_destructure_direct.ct \
    "Contract PrivateParamDestructureDirect:" \
    "    def main(input: hex):" \
    "        { prefix, input } = Split(input, 3)" \
    "        { payload, suffix } = Split(input, 2)" \
    "        Delete(prefix)" \
    "        Delete(suffix)" \
    "        Return payload"

write_case private_param_destructure_rebind.ct \
    "Contract PrivateParamDestructureRebind:" \
    "    def _strip(data: hex):" \
    "        { prefix, data } = Split(data, 3)" \
    "        { payload, suffix } = Split(data, 2)" \
    "        Delete(prefix)" \
    "        Delete(suffix)" \
    "        return payload" \
    "    def main(input: hex):" \
    "        result = _strip(input)" \
    "        Return result"

write_case builtin_member_direct.ct \
    "Contract BuiltinMemberDirect:" \
    "    def main():" \
    "        result = Sha256(BVM.unlockingInput)" \
    "        Return result"

write_case builtin_member_private.ct \
    "Contract BuiltinMemberPrivate:" \
    "    def _hash(value: hex):" \
    "        return Sha256(value)" \
    "    def main():" \
    "        result = _hash(BVM.unlockingInput)" \
    "        Return result"

write_case literal_private_direct.ct \
    "Contract LiteralPrivateDirect:" \
    "    def main():" \
    "        Return 7"

write_case literal_private_routed.ct \
    "Contract LiteralPrivateRouted:" \
    "    def _identity(value: number):" \
    "        return value" \
    "    def main():" \
    "        result = _identity(7)" \
    "        Return result"

write_case static_loop_if.ct \
    "Contract StaticLoopIf:" \
    "    def __init__(thenMarker: hex20, elseMarker: hex20):" \
    "        self.thenMarker = thenMarker" \
    "        self.elseMarker = elseMarker" \
    "    def main(value: hex20):" \
    "        for i in Range(3):" \
    "            if i == 1:" \
    "                EqualVerify(value.Clone(), self.thenMarker)" \
    "            else:" \
    "                EqualVerify(value.Clone(), self.elseMarker)" \
    "        Return value"

write_case runtime_if.ct \
    "Contract RuntimeIf:" \
    "    def __init__(thenMarker: hex20, elseMarker: hex20):" \
    "        self.thenMarker = thenMarker" \
    "        self.elseMarker = elseMarker" \
    "    def main(value: hex20, selector: number):" \
    "        if selector == 1:" \
    "            EqualVerify(value.Clone(), self.thenMarker)" \
    "        else:" \
    "            EqualVerify(value.Clone(), self.elseMarker)" \
    "        Return value"

write_case shallow_copy_assignment.ct \
    "Contract ShallowCopyAssignment:" \
    "    def main(b: int, blocker: int, a: int):" \
    "        b = a"

write_case shallow_copy_assignment_use.ct \
    "Contract ShallowCopyAssignmentUse:" \
    "    def main(b: int, blocker: int, a: int):" \
    "        b = a" \
    "        EqualVerify(b, a)" \
    "        Delete(blocker)" \
    "        Return 1"

write_case lifetime_cleanup_profitable.ct \
    "Contract LifetimeCleanupProfitable:" \
    "    def main(x: number, y: number, z: number):" \
    "        dead: number = x + 1" \
    "        Size(dead)" \
    "        kept_a: number = y + 1" \
    "        kept_b: number = z + 1" \
    "        Keep(kept_a)" \
    "        Keep(kept_b)"

write_case shallow_field_copy_assignment.ct \
    "Contract ShallowFieldCopyAssignment:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def main(dst: Pair, blocker: number, src: Pair):" \
    "        dst.left = src.left" \
    "        EqualVerify(dst.left, src.left)" \
    "        Delete(dst.right)" \
    "        Delete(blocker)" \
    "        Delete(src.right)" \
    "        Return 1"

write_case commutative_argument_layout.ct \
    "Contract CommutativeArgumentLayout:" \
    "    def main(b: int, a: int):" \
    "        result = Add(a, b)"

write_case sensitive_argument_layout.ct \
    "Contract SensitiveArgumentLayout:" \
    "    def main(b: int, a: int):" \
    "        result = Sub(a, b)"

write_case planned_ternary_argument_layout.ct \
    "Contract PlannedTernaryArgumentLayout:" \
    "    def main(d3: int, d2: int, d1: int, d0: int):" \
    "        result = Within(d0, d3, d2)" \
    "        final = Sub(d1, result)"

write_case planned_commutative_argument_layout.ct \
    "Contract PlannedCommutativeArgumentLayout:" \
    "    def main(d3: int, d2: int, d1: int, d0: int):" \
    "        result = Add(d3, d2)"

write_case planned_window_eight_layout.ct \
    "Contract PlannedWindowEightLayout:" \
    "    def main(d7: int, d6: int, d5: int, d4: int, d3: int, d2: int, d1: int, d0: int):" \
    "        result = Within(d3, d2, d7)"

write_case fallback_window_nine_layout.ct \
    "Contract FallbackWindowNineLayout:" \
    "    def main(d8: int, d7: int, d6: int, d5: int, d4: int, d3: int, d2: int, d1: int, d0: int):" \
    "        result = Within(d3, d2, d8)"

write_case fallback_script_argument_layout.ct \
    "Contract FallbackScriptArgumentLayout:" \
    "    def main(upper: int, value: int):" \
    "        result = Within(value, 0, upper)"

write_case adjacent_scalar_copy_assignment.ct \
    "Contract AdjacentScalarCopyAssignment:" \
    "    def main(target: number, source: number):" \
    "        target = source" \
    "        EqualVerify(target, source)"

write_case adjacent_field_copy_assignment.ct \
    "Contract AdjacentFieldCopyAssignment:" \
    "    Struct Data:" \
    "        value: number" \
    "    def main(data: Data, source: number):" \
    "        data.value = source" \
    "        EqualVerify(data.value, source)"

write_case deep_scalar_copy_fallback.ct \
    "Contract DeepScalarCopyFallback:" \
    "    def main(source: number, b6: number, b5: number, b4: number, b3: number, b2: number, b1: number, target: number):" \
    "        target = source"

write_case delete_fixed_scalar.ct \
    "Contract DeleteFixedScalar:" \
    "    def main():" \
    "        value: number = 7" \
    "        Delete(value)" \
    "        Return(1)"

write_case delete_fixed_struct_root.ct \
    "Contract DeleteFixedStructRoot:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def main():" \
    "        value: Pair = {7, 3}" \
    "        Delete(value)" \
    "        Return(1)"

write_case delete_fixed_struct_fields.ct \
    "Contract DeleteFixedStructFields:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def main():" \
    "        value: Pair = {7, 3}" \
    "        Delete(value.left)" \
    "        Delete(value.right)" \
    "        Return(1)"

write_case delete_fixed_array_root.ct \
    "Contract DeleteFixedArrayRoot:" \
    "    def main():" \
    "        values: number[2] = [7, 3]" \
    "        Delete(values)" \
    "        Return(1)"

write_case delete_fixed_array_elements.ct \
    "Contract DeleteFixedArrayElements:" \
    "    def main():" \
    "        values: number[2] = [7, 3]" \
    "        Delete(values[0])" \
    "        Delete(values[1])" \
    "        Return(1)"

write_case declared_array_static_expression_size.ct \
    "Contract DeclaredArrayStaticExpressionSize:" \
    "    def main():" \
    "        values: number[1 + 1]" \
    "        Return(1)"

write_case declared_array_true_size.ct \
    "Contract DeclaredArrayTrueSize:" \
    "    def main():" \
    "        values: number[2]" \
    "        SetAlt(values[2])" \
    "        Return(1)"

write_case declared_array_negative_size.ct \
    "Contract DeclaredArrayNegativeSize:" \
    "    def main():" \
    "        values: number[-1]" \
    "        Return(1)"

write_case declared_array_runtime_size.ct \
    "Contract DeclaredArrayRuntimeSize:" \
    "    def main(length: number):" \
    "        values: number[length]" \
    "        Return(length)"

write_case whole_array_identity_transfer.ct \
    "Contract WholeArrayIdentityTransfer:" \
    "    def main(source: number[2]):" \
    "        target = source" \
    "        Keep(target[0])" \
    "        Delete(target)" \
    "        Return(1)"

write_case static_expression_array_index.ct \
    "Contract StaticExpressionArrayIndex:" \
    "    def main(values: number[3]):" \
    "        Keep(values[1 + 1])" \
    "        Delete(values[1 + 0])" \
    "        Return(values[0])"

write_case static_expression_array_assignment.ct \
    "Contract StaticExpressionArrayAssignment:" \
    "    def main(source: number, values: number[3]):" \
    "        values[1 + 1] = source" \
    "        Keep(values[2])" \
    "        Delete(values[1])" \
    "        Return(source)"

write_case private_fixed_array_consumed.ct \
    "Contract PrivateFixedArrayConsumed:" \
    "    def _consume(values: number[2]):" \
    "        marker = values[0] + values[1]" \
    "        Delete(marker)" \
    "    def main(values: number[2]):" \
    "        _consume(values)" \
    "        Return(values[0])"

write_case delete_struct_root_direct.ct \
    "Contract DeleteStructRootDirect:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def main(value: Pair, marker: number):" \
    "        Delete(value)" \
    "        Return(marker)"

write_case delete_struct_root_private.ct \
    "Contract DeleteStructRootPrivate:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _discard(value: Pair):" \
    "        Delete(value)" \
    "    def main(value: Pair, marker: number):" \
    "        _discard(value)" \
    "        Return(marker)"

write_case delete_fixed_struct_root_private.ct \
    "Contract DeleteFixedStructRootPrivate:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def _discard(value: Pair):" \
    "        Delete(value)" \
    "    def main():" \
    "        value: Pair = {7, 3}" \
    "        _discard(value)" \
    "        Return(1)"

write_case nested_delete_binding_frame.ct \
    "Contract NestedDeleteBindingFrame:" \
    "    Struct Outer:" \
    "        oldField: number" \
    "    Struct Inner:" \
    "        newField: number" \
    "    def _inner(value: Inner):" \
    "        Delete(value)" \
    "    def _outer(value: Outer, inner: Inner, expected: number):" \
    "        _inner(inner)" \
    "        EqualVerify(value.oldField, expected)" \
    "    def main(outer: Outer, inner: Inner, expected: number):" \
    "        _outer(outer, inner, expected)" \
    "        Return(1)"

write_case nested_delete_array_frame.ct \
    "Contract NestedDeleteArrayFrame:" \
    "    def _inner(value: number[1]):" \
    "        Delete(value)" \
    "    def _outer(value: number[2], inner: number[1], expected: number):" \
    "        _inner(inner)" \
    "        EqualVerify(value[1], expected)" \
    "    def main(outer: number[2], inner: number[1], expected: number):" \
    "        _outer(outer, inner, expected)" \
    "        Return(1)"

write_case debug_default_pc_drift.ct \
    "Contract DebugDefaultPcDrift:" \
    "    def main(left: int, right: int):" \
    "        Verify(Equal(left, right))"

write_case fixed_hex_if.ct \
    "Contract FixedHexIf:" \
    "    def __init__(thenMarker: hex20, elseMarker: hex20):" \
    "        self.thenMarker = thenMarker" \
    "        self.elseMarker = elseMarker" \
    "    def main(value: hex20):" \
    "        fixedBytes: hex = 0x01" \
    "        if fixedBytes == 0x01:" \
    "            EqualVerify(value.Clone(), self.thenMarker)" \
    "        else:" \
    "            EqualVerify(value.Clone(), self.elseMarker)" \
    "        Return value"

write_case runtime_if_without_else.ct \
    "Contract RuntimeIfWithoutElse:" \
    "    def __init__(marker: hex20):" \
    "        self.marker = marker" \
    "    def main(value: hex20, selector: number):" \
    "        if selector == 1:" \
    "            EqualVerify(value.Clone(), self.marker)" \
    "        Return value"

write_case runtime_notif_without_else.ct \
    "Contract RuntimeNotIfWithoutElse:" \
    "    def __init__(marker: hex20):" \
    "        self.marker = marker" \
    "    def main(value: hex20, selector: number):" \
    "        if selector != 1:" \
    "            EqualVerify(value.Clone(), self.marker)" \
    "        Return value"

write_case runtime_nonzero_if.ct \
    "Contract RuntimeNonzeroIf:" \
    "    def __init__(marker: hex20):" \
    "        self.marker = marker" \
    "    def main(value: hex20, selector: number):" \
    "        if selector != 0:" \
    "            EqualVerify(value.Clone(), self.marker)" \
    "        Return value"

write_case runtime_zero_if.ct \
    "Contract RuntimeZeroIf:" \
    "    def __init__(marker: hex20):" \
    "        self.marker = marker" \
    "    def main(value: hex20, selector: number):" \
    "        if selector == 0:" \
    "            EqualVerify(value.Clone(), self.marker)" \
    "        Return value"

write_case static_range_if_without_else.ct \
    "Contract StaticRangeIfWithoutElse:" \
    "    def __init__(marker: hex20):" \
    "        self.marker = marker" \
    "    def main(value: hex20):" \
    "        for i in Range(2):" \
    "            if i == 0:" \
    "                EqualVerify(value.Clone(), self.marker)" \
    "        Return value"

write_case nested_dependent_range.ct \
    "Contract NestedDependentRange:" \
    "    def __init__(marker: hex20):" \
    "        self.marker = marker" \
    "    def main(value: hex20):" \
    "        for i in Range(3):" \
    "            for j in Range(i + 1):" \
    "                EqualVerify(value.Clone(), self.marker)" \
    "        Return value"

write_case stale_loop_target.ct \
    "Contract StaleLoopTarget:" \
    "    def __init__(marker: hex20):" \
    "        self.marker = marker" \
    "    def main(value: hex20):" \
    "        i: number = 7" \
    "        for i in Range(3):" \
    "            if i == 1:" \
    "                EqualVerify(value.Clone(), self.marker)" \
    "        Return value"

write_case empty_range_preserves_target.ct \
    "Contract EmptyRangePreservesTarget:" \
    "    def __init__(marker: hex20):" \
    "        self.marker = marker" \
    "    def main(value: hex20):" \
    "        i: number = 7" \
    "        for i in Range(0):" \
    "            EqualVerify(value.Clone(), self.marker)" \
    "        if i == 7:" \
    "            EqualVerify(value.Clone(), self.marker)" \
    "        Return value"

write_case loop_target_runtime_rebind.ct \
    "Contract LoopTargetRuntimeRebind:" \
    "    def main(value: number):" \
    "        for i in Range(2):" \
    "            i = value.Clone()" \
    "        Return i"

write_case loop_partial_evaluation.ct \
    "Contract LoopPartialEvaluation:" \
    "    def __init__(marker: hex20):" \
    "        self.marker = marker" \
    "    def main(value: hex20):" \
    "        folded: number = 0" \
    "        for i in Range(3):" \
    "            folded = i * 2 + 1" \
    "            EqualVerify(folded, i * 2 + 1)" \
    "            EqualVerify(value.Clone(), self.marker)" \
    "        Return value"

write_case loop_fixed_one_opcode.ct \
    "Contract LoopFixedOneOpcode:" \
    "    def main(acc: number):" \
    "        for i in Range(1, 2):" \
    "            acc = acc + i" \
    "        Return acc"

write_case loop_placeholder_reuse.ct \
    "Contract LoopPlaceholderReuse:" \
    "    def __init__(marker: hex20):" \
    "        self.marker = marker" \
    "    def main():" \
    "        for i in Range(1):" \
    "            EqualVerify(self.marker, self.marker)" \
    "        Return 1"

write_case excessive_static_range.ct \
    "Contract ExcessiveStaticRange:" \
    "    def main():" \
    "        for i in Range(100001):" \
    "            Verify(1)" \
    "        Return 1"

write_case minimum_range_nested.ct \
    "Contract MinimumRangeNested:" \
    "    def __init__(marker: hex20):" \
    "        self.marker = marker" \
    "    def main(value: hex20):" \
    "        for i in Range(-9223372036854775807 - 1, -9223372036854775807):" \
    "            for j in Range(i - i + 1):" \
    "                EqualVerify(value.Clone(), self.marker)" \
    "        Return value"

write_case non_numeric_loop_target.ct \
    "Contract NonNumericLoopTarget:" \
    "    def main(i: hex20):" \
    "        for i in Range(1):" \
    "            Verify(1)" \
    "        Return 1"

write_case numeric_parameter_loop_target.ct \
    "Contract NumericParameterLoopTarget:" \
    "    def main(i: number):" \
    "        for i in Range(1):" \
    "            Verify(i == 0)" \
    "        Return i"

write_case uint64_parameter_loop_target.ct \
    "Contract Uint64ParameterLoopTarget:" \
    "    def main(i: uint64):" \
    "        for i in Range(1):" \
    "            Verify(i == 0)" \
    "        Return i"

write_case implicit_numeric_loop_target.ct \
    "Contract ImplicitNumericLoopTarget:" \
    "    def main():" \
    "        i = 7" \
    "        for i in Range(1):" \
    "            Verify(i == 0)" \
    "        Return i"

write_case fixed_loop_target_altstack.ct \
    "Contract FixedLoopTargetAltstack:" \
    "    def main():" \
    "        for i in Range(2):" \
    "            SetAlt(i)" \
    "        SetMain(i)" \
    "        Return i"

write_case fixed_loop_target_altstack_clone.ct \
    "Contract FixedLoopTargetAltstackClone:" \
    "    def main():" \
    "        for i in Range(1):" \
    "            SetAlt(i)" \
    "            SetMain(i)" \
    "            EqualVerify(i.Clone(), i)" \
    "        Return 1"

write_case fixed_loop_target_altstack_double_read.ct \
    "Contract FixedLoopTargetAltstackDoubleRead:" \
    "    def main():" \
    "        for i in Range(1):" \
    "            SetAlt(i)" \
    "            SetMain(i)" \
    "            EqualVerify(i, i)" \
    "        Return 1"

write_case deleted_loop_target_use.ct \
    "Contract DeletedLoopTargetUse:" \
    "    def main():" \
    "        for i in Range(1):" \
    "            Delete(i)" \
    "            if i == 0:" \
    "                Verify(1)" \
    "        Return 1"

write_case fixed_loop_target_delete.ct \
    "Contract FixedLoopTargetDelete:" \
    "    def main():" \
    "        for i in Range(2):" \
    "            Delete(i)" \
    "        Return 1"

write_case empty_inner_preserves_outer.ct \
    "Contract EmptyInnerPreservesOuter:" \
    "    def __init__(marker: hex20):" \
    "        self.marker = marker" \
    "    def main(value: hex20):" \
    "        for i in Range(2):" \
    "            for i in Range(0):" \
    "                Verify(0)" \
    "            if i >= 0:" \
    "                EqualVerify(value.Clone(), self.marker)" \
    "        Return value"

write_case negative_step_range.ct \
    "Contract NegativeStepRange:" \
    "    def __init__(marker: hex20):" \
    "        self.marker = marker" \
    "    def main(value: hex20):" \
    "        for i in Range(5, 0, -2):" \
    "            EqualVerify(value.Clone(), self.marker)" \
    "        Return value"

write_case zero_step_range.ct \
    "Contract ZeroStepRange:" \
    "    def main():" \
    "        for i in Range(0, 1, 0):" \
    "            Verify(1)" \
    "        Return 1"

write_case overflow_range_expression.ct \
    "Contract OverflowRangeExpression:" \
    "    def main():" \
    "        for i in Range(9223372036854775807 + 1):" \
    "            Verify(1)" \
    "        Return 1"

write_case early_return_large_range.ct \
    "Contract EarlyReturnLargeRange:" \
    "    def main():" \
    "        for i in Range(100000):" \
    "            Return 1"

write_case literal_if_without_else.ct \
    "Contract LiteralIfWithoutElse:" \
    "    def main():" \
    "        if 0:" \
    "            Verify(0)" \
    "        if 1:" \
    "            Verify(1)" \
    "        Return 1"

write_case nested_if_without_else.ct \
    "Contract NestedIfWithoutElse:" \
    "    def main(outer: bool, inner: bool):" \
    "        if outer.Clone():" \
    "            if inner.Clone():" \
    "                Verify(1)" \
    "        Delete(outer)" \
    "        Delete(inner)" \
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

write_case static_if_dead_ownership.ct \
    "Contract StaticIfDeadOwnership:" \
    "    def __init__(marker: hex20):" \
    "        self.marker = marker" \
    "    def main(value: hex20):" \
    "        for i in Range(1):" \
    "            if i == 0:" \
    "                EqualVerify(value.Clone(), self.marker)" \
    "            else:" \
    "                Delete(value)" \
    "        Return value"

write_case static_if_dead_runtime_index.ct \
    "Contract StaticIfDeadRuntimeIndex:" \
    "    def main(values: number[2], index: number):" \
    "        for i in Range(1):" \
    "            if i == 0:" \
    "                EqualVerify(values[0].Clone(), 7)" \
    "            else:" \
    "                EqualVerify(values[index].Clone(), 7)" \
    "        Delete(index)" \
    "        Return 1"

write_case static_loop_if_return.ct \
    "Contract StaticLoopIfReturn:" \
    "    def __init__(beforeReturn: hex20, unreachable: hex20):" \
    "        self.beforeReturn = beforeReturn" \
    "        self.unreachable = unreachable" \
    "    def main(value: hex20):" \
    "        for i in Range(3):" \
    "            if i == 1:" \
    "                Return 1" \
    "            else:" \
    "                EqualVerify(value.Clone(), self.beforeReturn)" \
    "        EqualVerify(value.Clone(), self.unreachable)" \
    "        Return 1"

write_case static_if_return_dead_runtime_index.ct \
    "Contract StaticIfReturnDeadRuntimeIndex:" \
    "    Struct Pair:" \
    "        left: number" \
    "        right: number" \
    "    def main(index: number):" \
    "        values: Pair[1] = [{1, 2}]" \
    "        for i in Range(1):" \
    "            if i == 0:" \
    "                Return 1" \
    "            else:" \
    "                EqualVerify(index.Clone(), 0)" \
    "        selected = values[index].Clone()"

write_case invalid_static_clone_if.ct \
    "Contract InvalidStaticCloneIf:" \
    "    def main():" \
    "        for i in Range(1):" \
    "            if i.Clone(123) == 0:" \
    "                Return 1" \
    "            else:" \
    "                Return 0"

write_case tbc20_static_loop_if.ct \
    "Contract TBC20StaticLoopIf:" \
    "    def __init__(OriginalUTXO: hex36):" \
    "        self.OriginalUTXO = OriginalUTXO" \
    "    def main(prepreHash: hex20, preHash: hex20, vinData: hex36):" \
    "        for i in Range(5, -1, -1):" \
    "            preTXVinDataSlice = vinData.Clone()" \
    "            if i == 5:" \
    "                if prepreHash.Clone() != preHash.Clone():" \
    "                    EqualVerify(preTXVinDataSlice, self.OriginalUTXO)" \
    "                else:" \
    "                    Delete(preTXVinDataSlice)" \
    "            else:" \
    "                EqualVerify(prepreHash.Clone(), preHash.Clone())" \
    "                Delete(preTXVinDataSlice)" \
    "        Delete(prepreHash)" \
    "        Delete(preHash)" \
    "        Delete(vinData)" \
    "        Return 1"

write_case global_basic.ct \
    "Contract GlobalBasic:" \
    "    global TapeFlag = \"TBC20TAPE\" # tape后缀" \
    "    global TapeFlagSize = 9" \
    "    def main(data: string):" \
    "        EqualVerify(data, TapeFlag)" \
    "        Return TapeFlagSize"

write_case global_private.ct \
    "Contract GlobalPrivate:" \
    "    global TapeFlag = \"TBC20TAPE\"" \
    "    global TapeFlagSize = 9" \
    "    def _verify(data: string):" \
    "        EqualVerify(data, TapeFlag)" \
    "        return TapeFlagSize" \
    "    def main(data: string):" \
    "        result = _verify(data)" \
    "        Return result"

write_case global_unused.ct \
    "Contract GlobalUnused:" \
    "    global Unused = \"not emitted\"" \
    "    def main():" \
    "        Return 1"

write_case global_unused_baseline.ct \
    "Contract GlobalUnusedBaseline:" \
    "    def main():" \
    "        Return 1"

write_case global_literal.ct \
    "Contract GlobalLiteral:" \
    "    global Value = 9" \
    "    def main():" \
    "        Return Value"

write_case global_literal_baseline.ct \
    "Contract GlobalLiteralBaseline:" \
    "    def main():" \
    "        Return 9"

write_case global_call_namespace.ct \
    "Contract GlobalCallNamespace:" \
    "    global Size = 9" \
    "    def main(data: hex):" \
    "        dataSize = Size(data)" \
    "        Return dataSize + Size"

write_case global_call_namespace_baseline.ct \
    "Contract GlobalCallNamespaceBaseline:" \
    "    def main(data: hex):" \
    "        dataSize = Size(data)" \
    "        Return dataSize + 9"

write_case global_library.ct \
    "Library global_library:" \
    "    def globalValue():" \
    "        return TapeFlag"

write_case global_library_user.ct \
    "import \"./global_library.ct\"" \
    "Contract GlobalLibraryUser:" \
    "    global TapeFlag = \"TBC20TAPE\"" \
    "    def main():" \
    "        result = globalValue()" \
    "        Return result"

write_case global_library_baseline.ct \
    "Contract GlobalLibraryBaseline:" \
    "    global TapeFlag = \"TBC20TAPE\"" \
    "    def main():" \
    "        Return TapeFlag"

write_case global_duplicate.ct \
    "Contract GlobalDuplicate:" \
    "    global TapeFlag = \"TBC20TAPE\"" \
    "    global TapeFlag = \"OTHER\"" \
    "    def main():" \
    "        Return 1"

write_case global_member_conflict.ct \
    "Contract GlobalMemberConflict:" \
    "    global main = 9" \
    "    def main():" \
    "        Return 1"

write_case global_parameter_shadow.ct \
    "Contract GlobalParameterShadow:" \
    "    global TapeFlag = \"TBC20TAPE\"" \
    "    def main(TapeFlag: string):" \
    "        Return TapeFlag"

write_case global_local_shadow.ct \
    "Contract GlobalLocalShadow:" \
    "    global TapeFlagSize = 9" \
    "    def main():" \
    "        TapeFlagSize: number = 10" \
    "        Return TapeFlagSize"

write_case global_assignment.ct \
    "Contract GlobalAssignment:" \
    "    global TapeFlagSize = 9" \
    "    def main():" \
    "        TapeFlagSize = 10" \
    "        Return TapeFlagSize"

write_case global_index_assignment.ct \
    "Contract GlobalIndexAssignment:" \
    "    global TapeFlag = 0x01" \
    "    def main():" \
    "        TapeFlag[0] = 1" \
    "        Return 1"

write_case global_destructure_shadow.ct \
    "Contract GlobalDestructureShadow:" \
    "    global TapeFlag = \"TBC20TAPE\"" \
    "    def main(data: hex):" \
    "        { TapeFlag, rest } = Split(data, 1)" \
    "        Delete(rest)" \
    "        Return TapeFlag"

write_case global_loop_shadow.ct \
    "Contract GlobalLoopShadow:" \
    "    global TapeFlagSize = 9" \
    "    def main():" \
    "        for TapeFlagSize in Range(1):" \
    "            Return TapeFlagSize"

write_case global_late.ct \
    "Contract GlobalLate:" \
    "    def main():" \
    "        Return 1" \
    "    global TapeFlagSize = 9"

write_case global_in_function.ct \
    "Contract GlobalInFunction:" \
    "    def main():" \
    "        global TapeFlagSize = 9" \
    "        Return 1"

write_case global_non_literal.ct \
    "Contract GlobalNonLiteral:" \
    "    global TapeFlagSize = Size(0x01)" \
    "    def main():" \
    "        Return 1"

write_case global_invalid_library.ct \
    "Library global_invalid_library:" \
    "    global TapeFlagSize = 9" \
    "    def value():" \
    "        return 1"

write_case global_invalid_library_user.ct \
    "import \"./global_invalid_library.ct\"" \
    "Contract GlobalInvalidLibraryUser:" \
    "    def main():" \
    "        Return 1"

write_case global_unused_invalid_number.ct \
    "Contract GlobalUnusedInvalidNumber:" \
    "    global Bad = 999999999999999999999999999999999999999999999999" \
    "    def main():" \
    "        Return 1"

write_case global_unused_invalid_hex.ct \
    "Contract GlobalUnusedInvalidHex:" \
    "    global Bad = 0x0" \
    "    def main():" \
    "        Return 1"

write_case global_unused_invalid_address.ct \
    "Contract GlobalUnusedInvalidAddress:" \
    "    global Bad = \"1111111111111111111111111111111111\"" \
    "    def main():" \
    "        Return 1"

write_case global_constructor_shadow.ct \
    "Contract GlobalConstructorShadow:" \
    "    global TapeFlagSize = 9" \
    "    def __init__(TapeFlagSize: int):" \
    "        self.value = TapeFlagSize" \
    "    def main():" \
    "        Return 1"

cp "$REPO_ROOT/test/contract_file/op_return_push_padding.ct" \
    "$TMP_DIR/op_return_push_padding.ct"

expect_failure bang.ct "unexpected character '!'"
expect_success notequal.ct
expect_failure two_contracts.ct "unexpected token after contract definition"
expect_failure trailing_garbage.ct "unexpected token after contract definition"
expect_success return_bool_ok.ct
expect_failure return_bool_bad.ct "return type mismatch"
expect_failure bool_two.ct "type mismatch in variable declaration"
expect_failure hex_string.ct "type mismatch in variable declaration"
expect_failure string_num.ct "type mismatch in variable declaration"
expect_failure self_return.ct "fixed byte length declaration"
expect_success self_return_hex20.ct
expect_success self_digit_return_hex20.ct
expect_failure push_self.ct "fixed byte length"
expect_success push_self_hex20.ct
expect_failure no_return.ct "generated bytecode is empty"
expect_failure huge_num.ct "integer literal out of range"
expect_success param_array_loop_index.ct
expect_success pushdata_asm.ct
expect_success dynamic_slice_start.ct
expect_success dynamic_slice_length.ct
expect_success dynamic_slice_window.ct
expect_success dynamic_slice_both.ct
expect_failure duplicate_dynamic_slice_bound.ct "same runtime stack slot"
expect_success private_param_destructure_direct.ct
expect_success private_param_destructure_rebind.ct
expect_success builtin_member_direct.ct
expect_success builtin_member_private.ct
expect_success literal_private_direct.ct
expect_success literal_private_routed.ct
expect_success static_loop_if.ct
expect_success runtime_if.ct
expect_success shallow_copy_assignment.ct
expect_success shallow_copy_assignment_use.ct
expect_success lifetime_cleanup_profitable.ct
expect_success shallow_field_copy_assignment.ct
expect_success commutative_argument_layout.ct
expect_success sensitive_argument_layout.ct
expect_success planned_ternary_argument_layout.ct
expect_success planned_commutative_argument_layout.ct
expect_success planned_window_eight_layout.ct
expect_success fallback_window_nine_layout.ct
expect_success fallback_script_argument_layout.ct
expect_success adjacent_scalar_copy_assignment.ct
expect_success adjacent_field_copy_assignment.ct
expect_success deep_scalar_copy_fallback.ct
expect_success delete_fixed_scalar.ct
expect_success delete_fixed_struct_root.ct
expect_success delete_fixed_struct_fields.ct
expect_success delete_fixed_array_root.ct
expect_success delete_fixed_array_elements.ct
expect_success declared_array_static_expression_size.ct
expect_failure declared_array_true_size.ct "out of bounds for 'values' of length 2"
expect_failure declared_array_negative_size.ct \
    "size must be a non-negative compile-time integer"
expect_failure declared_array_runtime_size.ct \
    "size must be a non-negative compile-time integer"
expect_success whole_array_identity_transfer.ct
expect_success static_expression_array_index.ct
expect_success static_expression_array_assignment.ct
expect_failure private_fixed_array_consumed.ct \
    "Array element 'values\[0\]' has been consumed"
expect_success delete_struct_root_direct.ct
expect_success delete_struct_root_private.ct
expect_success delete_fixed_struct_root_private.ct
expect_success nested_delete_binding_frame.ct
expect_success nested_delete_array_frame.ct
expect_success fixed_hex_if.ct
expect_success runtime_if_without_else.ct
expect_success runtime_notif_without_else.ct
expect_success runtime_nonzero_if.ct
expect_success runtime_zero_if.ct
expect_success static_range_if_without_else.ct
expect_success nested_dependent_range.ct
expect_success stale_loop_target.ct
expect_success empty_range_preserves_target.ct
expect_success loop_target_runtime_rebind.ct
expect_success loop_partial_evaluation.ct
expect_success loop_fixed_one_opcode.ct
expect_success loop_placeholder_reuse.ct
expect_failure excessive_static_range.ct "exceeds limit 100000"
expect_success minimum_range_nested.ct
expect_failure non_numeric_loop_target.ct "must be a numeric scalar"
expect_success numeric_parameter_loop_target.ct
expect_success uint64_parameter_loop_target.ct
expect_success implicit_numeric_loop_target.ct
expect_success fixed_loop_target_altstack.ct
expect_success fixed_loop_target_altstack_clone.ct
expect_failure fixed_loop_target_altstack_double_read.ct "consumed more than once"
expect_failure deleted_loop_target_use.ct "consumed and cannot be used again"
expect_success fixed_loop_target_delete.ct
expect_success empty_inner_preserves_outer.ct
expect_success negative_step_range.ct
expect_failure zero_step_range.ct "range step must not be zero"
expect_failure overflow_range_expression.ct "integer addition overflow"
expect_success early_return_large_range.ct
expect_success literal_if_without_else.ct
expect_success nested_if_without_else.ct
expect_success terminal_if_without_else.ct
expect_failure main_stack_imbalance_without_else.ct \
    "if without else changes main-stack state"
expect_failure fixed_storage_change_without_else.ct \
    "if without else changes compiler storage state"
expect_success local_fixed_without_else.ct
expect_success static_if_dead_ownership.ct
expect_success static_if_dead_runtime_index.ct
expect_success static_loop_if_return.ct
expect_success static_if_return_dead_runtime_index.ct
expect_failure invalid_static_clone_if.ct \
    "Cannot find stack position for object"
expect_success tbc20_static_loop_if.ct
expect_success global_basic.ct
expect_success global_private.ct
expect_success global_unused.ct
expect_success global_unused_baseline.ct
expect_success global_literal.ct
expect_success global_literal_baseline.ct
expect_success global_call_namespace.ct
expect_success global_call_namespace_baseline.ct
expect_success global_library_user.ct
expect_success global_library_baseline.ct
expect_failure global_duplicate.ct "duplicate global constant 'TapeFlag'"
expect_failure global_member_conflict.ct \
    "global constant 'main' conflicts with a function or struct name"
expect_failure global_parameter_shadow.ct \
    "function parameter 'TapeFlag' cannot shadow or assign global constant"
expect_failure global_local_shadow.ct \
    "local variable 'TapeFlagSize' cannot shadow or assign global constant"
expect_failure global_assignment.ct \
    "assignment target 'TapeFlagSize' cannot shadow or assign global constant"
expect_failure global_index_assignment.ct \
    "assignment target 'TapeFlag' cannot shadow or assign global constant"
expect_failure global_destructure_shadow.ct \
    "destructure target 'TapeFlag' cannot shadow or assign global constant"
expect_failure global_loop_shadow.ct \
    "for-loop target 'TapeFlagSize' cannot shadow or assign global constant"
expect_failure global_late.ct "global declarations must appear before"
expect_failure global_in_function.ct \
    "global declarations are only allowed at the beginning of a Contract body"
expect_failure global_non_literal.ct \
    "global constant initializer must be a scalar literal"
expect_failure global_invalid_library_user.ct \
    "global declarations are only allowed at the beginning of a Contract body"
expect_failure global_unused_invalid_number.ct "integer literal out of range"
expect_failure global_unused_invalid_hex.ct "invalid hexadecimal literal"
expect_failure global_unused_invalid_address.ct "invalid P2PKH address literal"
expect_failure global_constructor_shadow.ct \
    "function parameter 'TapeFlagSize' cannot shadow or assign global constant"
if grep -Eq "line 0|:0(:|,)" \
    "$TMP_DIR/global_constructor_shadow.ct.err" \
    "$TMP_DIR/global_constructor_shadow.ct.out"; then
    echo "Constructor/global conflict used an invalid source location" >&2
    exit 1
fi
expect_success op_return_push_padding.ct

if ! grep -Fq \
    "Applied 1 lifetime cleanup(s): locking script bytes 9 -> 8" \
    "$TMP_DIR/lifetime_cleanup_profitable.ct.out" \
    "$TMP_DIR/lifetime_cleanup_profitable.ct.err"; then
    echo "Expected strict lifetime cleanup candidate to be selected" >&2
    exit 1
fi

if [[ ${APC_BUILD_DEBUGGER:-1} == 1 ]]; then
    (
        cd "$TMP_DIR"
        # Exercise the default path as well: later bytecode passes must reuse
        # it when persisting their final PC remapping.
        "$COMPILER" global_basic.ct -d \
            >global_basic.debug.out 2>global_basic.debug.err
    )
    [[ -s "$TMP_DIR/global_basic.debug" ]]

    (
        cd "$TMP_DIR"
        "$COMPILER" debug_default_pc_drift.ct -d \
            >debug_default_pc_drift.debug.out \
            2>debug_default_pc_drift.debug.err
    )
    [[ -s "$TMP_DIR/debug_default_pc_drift.debug" ]]
    [[ -s "$TMP_DIR/debug_default_pc_drift.json" ]]
fi

python3 - "$TMP_DIR/self_return_hex20.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    data = json.load(f)

asm = data["lock"]["asm"].split()
assert asm and asm[0] == "<self.value20>", data["lock"]["asm"]
assert "<self.value20>" in data["lock"]["hex"], data["lock"]["hex"]
assert "<self.value>" not in data["lock"]["hex"], data["lock"]["hex"]
PY

python3 - "$TMP_DIR/lifetime_cleanup_profitable.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    data = json.load(f)

assert data["lock"]["hex"] == "7b8b82777b8b7b8b", data["lock"]
assert data["lock"]["asm"].split().count("OP_NIP") == 1, data["lock"]
PY

python3 - "$TMP_DIR/self_digit_return_hex20.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    data = json.load(f)

asm = data["lock"]["asm"].split()
assert asm and asm[0] == "<self.value220>", data["lock"]["asm"]
assert "<self.value220>" in data["lock"]["hex"], data["lock"]["hex"]
assert "<self.value2>" not in data["lock"]["hex"], data["lock"]["hex"]
PY

python3 - "$TMP_DIR/push_self_hex20.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    data = json.load(f)

asm = data["lock"]["asm"].split()
assert asm and asm[0] == "<self.value20>", data["lock"]["asm"]
assert "<self.value20>" in data["lock"]["hex"], data["lock"]["hex"]
assert "<self.value>" not in data["lock"]["hex"], data["lock"]["hex"]
PY

python3 - "$TMP_DIR/pushdata_asm.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    data = json.load(f)

asm = data["lock"]["asm"]
tokens = asm.split()
# Interpreter's script decoder preserves the direct-push length prefix in ASM.
assert "0114" in tokens, asm
assert "14" not in tokens, asm
assert "OP_PUSHDATA1" not in tokens, asm
assert "OP_PUSHDATA2" not in tokens, asm
assert "OP_PUSHDATA4" not in tokens, asm
PY

python3 - "$TMP_DIR/dynamic_slice_start.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    tokens = json.load(f)["lock"]["asm"].split()

expected = ["OP_SIZE", "OP_9", "OP_SUB", "OP_SPLIT", "OP_NIP"]
assert tokens[:len(expected)] == expected, tokens
assert tokens.count("OP_SUB") == 1, tokens
assert "OP_SWAP" not in tokens, tokens
PY

python3 - "$TMP_DIR/dynamic_slice_length.json" \
    "$TMP_DIR/dynamic_slice_window.json" \
    "$TMP_DIR/dynamic_slice_both.json" <<'PY'
import json
import sys

def tokens(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)["lock"]["asm"].split()

length = tokens(sys.argv[1])
window = tokens(sys.argv[2])
both = tokens(sys.argv[3])

expected_length = [
    "OP_SIZE", "OP_9", "OP_SUB", "OP_SPLIT", "OP_DROP",
]
assert length[:len(expected_length)] == expected_length, length
assert length.count("OP_SUB") == 1, length

expected_window = [
    "OP_SIZE", "OP_9", "OP_SUB", "OP_SPLIT", "OP_NIP",
    "OP_3", "OP_SPLIT", "OP_DROP",
]
assert window[:len(expected_window)] == expected_window, window
assert window.count("OP_SUB") == 1, window

assert both.count("OP_SPLIT") == 2, both
assert both.count("OP_NIP") == 1, both
assert both.count("OP_DROP") == 1, both
PY

python3 - "$TMP_DIR/builtin_member_direct.json" \
    "$TMP_DIR/builtin_member_private.json" \
    "$TMP_DIR/literal_private_direct.json" \
    "$TMP_DIR/literal_private_routed.json" \
    "$TMP_DIR/private_param_destructure_direct.json" \
    "$TMP_DIR/private_param_destructure_rebind.json" <<'PY'
import json
import sys

def lock(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)["lock"]

direct = lock(sys.argv[1])
private = lock(sys.argv[2])
assert private["asm"] == direct["asm"], (direct["asm"], private["asm"])
assert private["hex"] == direct["hex"], (direct["hex"], private["hex"])

literal_direct = lock(sys.argv[3])
literal_routed = lock(sys.argv[4])
assert literal_routed["asm"] == literal_direct["asm"], (
    literal_direct["asm"], literal_routed["asm"]
)
assert literal_routed["hex"] == literal_direct["hex"], (
    literal_direct["hex"], literal_routed["hex"]
)

destructure_direct = lock(sys.argv[5])
destructure_private = lock(sys.argv[6])
assert destructure_private["asm"] == destructure_direct["asm"], (
    destructure_direct["asm"], destructure_private["asm"]
)
assert destructure_private["hex"] == destructure_direct["hex"], (
    destructure_direct["hex"], destructure_private["hex"]
)
PY

python3 - "$TMP_DIR/shallow_copy_assignment.json" \
    "$TMP_DIR/shallow_copy_assignment_use.json" \
    "$TMP_DIR/shallow_field_copy_assignment.json" \
    "$TMP_DIR/commutative_argument_layout.json" \
    "$TMP_DIR/sensitive_argument_layout.json" \
    "$TMP_DIR/planned_ternary_argument_layout.json" \
    "$TMP_DIR/planned_commutative_argument_layout.json" \
    "$TMP_DIR/planned_window_eight_layout.json" \
    "$TMP_DIR/fallback_window_nine_layout.json" \
    "$TMP_DIR/fallback_script_argument_layout.json" \
    "$TMP_DIR/adjacent_scalar_copy_assignment.json" \
    "$TMP_DIR/adjacent_field_copy_assignment.json" \
    "$TMP_DIR/deep_scalar_copy_fallback.json" <<'PY'
import json
import sys

def tokens(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)["lock"]["asm"].split()

# b, blocker, a (a is top) used to take the 10-byte generic altstack path.
# The bounded shortest-path planner proves this 3-byte exact replacement.
copy_tokens = tokens(sys.argv[1])
assert copy_tokens == ["OP_ROT", "OP_DROP", "OP_TUCK"], copy_tokens

# The symbolic scope remains at the same logical positions after the plan, so
# both the copied target and source can be consumed by later expressions.
copy_use = tokens(sys.argv[2])
assert copy_use[:3] == ["OP_ROT", "OP_DROP", "OP_TUCK"], copy_use
assert "OP_EQUALVERIFY" in copy_use, copy_use

# Field-to-field replacement uses the same exact planner and remains usable by
# subsequent field accesses. It must avoid the legacy altstack shuffle.
field_copy = tokens(sys.argv[3])
assert "OP_EQUALVERIFY" in field_copy, field_copy
assert "OP_TOALTSTACK" not in field_copy, field_copy
assert "OP_FROMALTSTACK" not in field_copy, field_copy

# Argument expressions still evaluate in source order. Only a commutative
# opcode may select the zero-move physical layout.
commutative = tokens(sys.argv[4])
sensitive = tokens(sys.argv[5])
assert commutative == ["OP_ADD"], commutative
assert sensitive == ["OP_SWAP", "OP_SUB"], sensitive

# Three order-sensitive arguments occupy depths [0, 3, 2]. The legacy
# argument-by-argument lowering costs four bytes; one 2SWAP reaches the complete
# target permutation. The following SUB consumes the untouched slot and result,
# checking that the symbolic Scope mirrored the plan and opcode consumption.
ternary = tokens(sys.argv[6])
assert ternary == ["OP_2SWAP", "OP_WITHIN", "OP_SUB"], ternary

# The exact analyzer must also drive the commutative-order chooser. Greedy-only
# costs prefer the swapped order here, while the original order has a one-byte
# complete 2SWAP plan.
planned_commutative = tokens(sys.argv[7])
assert planned_commutative == ["OP_2SWAP", "OP_ADD"], planned_commutative

# The bounded planner accepts an eight-slot window, including a depth-7 ROLL,
# but depth 8 forms a nine-slot window and must retain the legacy fallback.
window_eight = tokens(sys.argv[8])
window_nine = tokens(sys.argv[9])
assert window_eight[:4] == [
    "OP_2SWAP", "OP_7", "OP_ROLL", "OP_WITHIN"
], window_eight
assert window_nine[:7] == [
    "OP_3", "OP_ROLL", "OP_3", "OP_ROLL", "OP_8", "OP_ROLL", "OP_WITHIN"
], window_nine

# Inline script arguments create runtime slots and are intentionally excluded
# from the move-only planner.
script_fallback = tokens(sys.argv[10])
assert script_fallback[:3] == ["OP_0", "OP_ROT", "OP_WITHIN"], script_fallback

# Scalar and flattened-field copies share the same canonical adjacent sequence.
# The source remains available for the following comparison and no altstack is
# touched.
adjacent_scalar = tokens(sys.argv[11])
adjacent_field = tokens(sys.argv[12])
assert adjacent_scalar == adjacent_field, (adjacent_scalar, adjacent_field)
for adjacent in (adjacent_scalar, adjacent_field):
    assert adjacent == ["OP_NIP", "OP_DUP", "OP_EQUALVERIFY"], adjacent
    assert "OP_TOALTSTACK" not in adjacent, adjacent
    assert "OP_FROMALTSTACK" not in adjacent, adjacent

# Copy depths beyond the planner cap use the shared canonical legacy emitter.
deep_copy = tokens(sys.argv[13])
assert deep_copy == ["OP_DROP", "OP_6", "OP_PICK"], deep_copy
PY

python3 - "$TMP_DIR/delete_fixed_scalar.json" \
    "$TMP_DIR/delete_fixed_struct_root.json" \
    "$TMP_DIR/delete_fixed_struct_fields.json" \
    "$TMP_DIR/delete_fixed_array_root.json" \
    "$TMP_DIR/delete_fixed_array_elements.json" <<'PY'
import json
import sys

def executable_tokens(path):
    with open(path, "r", encoding="utf-8") as f:
        tokens = json.load(f)["lock"]["asm"].split()
    return tokens[:tokens.index("OP_RETURN")]

for path in sys.argv[1:]:
    actual = executable_tokens(path)
    assert actual == ["OP_1"], (path, actual)
PY

python3 - "$TMP_DIR/delete_fixed_struct_root_private.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    tokens = json.load(f)["lock"]["asm"].split()
actual = tokens[:tokens.index("OP_RETURN")]
# Interpreter's lifetime pass removes the private inline initialization/delete
# pair once it proves that no value escapes the call.
assert actual == ["OP_1"], actual
PY

python3 - "$TMP_DIR/delete_struct_root_direct.json" \
    "$TMP_DIR/delete_struct_root_private.json" <<'PY'
import json
import sys

def lock(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)["lock"]

direct = lock(sys.argv[1])
private = lock(sys.argv[2])
direct_tokens = direct["asm"].split()
private_tokens = private["asm"].split()
assert direct_tokens[:direct_tokens.index("OP_RETURN")] == [
    "OP_NIP", "OP_NIP"
], direct_tokens
# The target planner's equivalent three-slot permutation is SWAP+ROT; after
# OP_2DROP it preserves the same marker as Compiler's ROT+ROT sequence.
assert private_tokens[:private_tokens.index("OP_RETURN")] == [
    "OP_SWAP", "OP_ROT", "OP_2DROP"
], private_tokens
PY

python3 - "$TMP_DIR/static_loop_if.json" \
    "$TMP_DIR/runtime_if.json" \
    "$TMP_DIR/fixed_hex_if.json" \
    "$TMP_DIR/static_if_dead_ownership.json" \
    "$TMP_DIR/static_if_dead_runtime_index.json" \
    "$TMP_DIR/static_loop_if_return.json" \
    "$TMP_DIR/static_if_return_dead_runtime_index.json" \
    "$TMP_DIR/tbc20_static_loop_if.json" <<'PY'
import json
import sys

def tokens(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)["lock"]["asm"].split()

static_loop = tokens(sys.argv[1])
for opcode in ("OP_IF", "OP_NOTIF", "OP_ELSE", "OP_ENDIF"):
    assert opcode not in static_loop, static_loop
assert static_loop.count("<self.thenMarker20>") == 1, static_loop
assert static_loop.count("<self.elseMarker20>") == 2, static_loop

runtime = tokens(sys.argv[2])
assert runtime.count("OP_IF") == 1, runtime
assert runtime.count("OP_NOTIF") == 0, runtime
assert runtime.count("OP_ELSE") == 1, runtime
assert runtime.count("OP_ENDIF") == 1, runtime
assert runtime.count("<self.thenMarker20>") == 1, runtime
assert runtime.count("<self.elseMarker20>") == 1, runtime

fixed_hex = tokens(sys.argv[3])
assert fixed_hex.count("OP_IF") == 1, fixed_hex
assert fixed_hex.count("OP_ELSE") == 1, fixed_hex
assert fixed_hex.count("OP_ENDIF") == 1, fixed_hex
assert fixed_hex.count("<self.thenMarker20>") == 1, fixed_hex
assert fixed_hex.count("<self.elseMarker20>") == 1, fixed_hex

dead_ownership = tokens(sys.argv[4])
assert dead_ownership.count("<self.marker20>") == 1, dead_ownership
for opcode in ("OP_IF", "OP_NOTIF", "OP_ELSE", "OP_ENDIF"):
    assert opcode not in dead_ownership, dead_ownership

dead_runtime_index = tokens(sys.argv[5])
assert "OP_WITHIN" not in dead_runtime_index, dead_runtime_index
for opcode in ("OP_IF", "OP_NOTIF", "OP_ELSE", "OP_ENDIF"):
    assert opcode not in dead_runtime_index, dead_runtime_index

terminal = tokens(sys.argv[6])
assert terminal.count("<self.beforeReturn20>") == 1, terminal
assert "<self.unreachable20>" not in terminal, terminal
assert terminal.count("OP_RETURN") == 1, terminal
for opcode in ("OP_IF", "OP_NOTIF", "OP_ELSE", "OP_ENDIF"):
    assert opcode not in terminal, terminal

terminal_dead_index = tokens(sys.argv[7])
assert "OP_WITHIN" not in terminal_dead_index, terminal_dead_index
for opcode in ("OP_IF", "OP_NOTIF", "OP_ELSE", "OP_ENDIF"):
    assert opcode not in terminal_dead_index, terminal_dead_index

tbc20 = tokens(sys.argv[8])
assert tbc20.count("OP_IF") == 0, tbc20
assert tbc20.count("OP_NOTIF") == 1, tbc20
assert tbc20.count("OP_ELSE") == 1, tbc20
assert tbc20.count("OP_ENDIF") == 1, tbc20
assert tbc20.count("OP_EQUAL") == 1, tbc20
assert tbc20.count("OP_EQUALVERIFY") == 6, tbc20
assert tbc20.count("<self.OriginalUTXO36>") == 1, tbc20
PY

python3 - "$TMP_DIR/runtime_nonzero_if.json" \
    "$TMP_DIR/runtime_zero_if.json" <<'PY'
import json
import sys

def tokens(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)["lock"]["asm"].split()

nonzero = tokens(sys.argv[1])
zero = tokens(sys.argv[2])
assert nonzero.count("OP_IF") == 0, nonzero
assert nonzero.count("OP_NOTIF") == 1, nonzero
# A raw number cannot prove the byte-equality-to-zero rewrite safe, but the
# canonical OP_EQUAL result can carry != through OP_NOTIF directly.
assert "OP_NOT" not in nonzero, nonzero
assert nonzero.count("OP_EQUAL") == 1, nonzero
assert zero.count("OP_IF") == 1, zero
assert zero.count("OP_NOTIF") == 0, zero
assert "OP_NOT" not in zero, zero
assert zero.count("OP_EQUAL") == 1, zero
PY

python3 - "$TMP_DIR/runtime_if_without_else.json" \
    "$TMP_DIR/runtime_notif_without_else.json" \
    "$TMP_DIR/static_range_if_without_else.json" \
    "$TMP_DIR/terminal_if_without_else.json" \
    "$TMP_DIR/local_fixed_without_else.json" \
    "$TMP_DIR/literal_if_without_else.json" \
    "$TMP_DIR/nested_if_without_else.json" <<'PY'
import json
import sys

def tokens(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)["lock"]["asm"].split()

runtime_if = tokens(sys.argv[1])
assert runtime_if.count("OP_IF") == 1, runtime_if
assert runtime_if.count("OP_NOTIF") == 0, runtime_if
assert runtime_if.count("OP_ELSE") == 0, runtime_if
assert runtime_if.count("OP_ENDIF") == 1, runtime_if
assert runtime_if.count("<self.marker20>") == 1, runtime_if

runtime_notif = tokens(sys.argv[2])
assert runtime_notif.count("OP_IF") == 0, runtime_notif
assert runtime_notif.count("OP_NOTIF") == 1, runtime_notif
assert runtime_notif.count("OP_ELSE") == 0, runtime_notif
assert runtime_notif.count("OP_ENDIF") == 1, runtime_notif
assert runtime_notif.count("<self.marker20>") == 1, runtime_notif

static_range = tokens(sys.argv[3])
for opcode in ("OP_IF", "OP_NOTIF", "OP_ELSE", "OP_ENDIF"):
    assert opcode not in static_range, static_range
assert static_range.count("<self.marker20>") == 1, static_range

terminal = tokens(sys.argv[4])
assert terminal.count("OP_IF") == 1, terminal
assert terminal.count("OP_NOTIF") == 0, terminal
assert terminal.count("OP_ELSE") == 0, terminal
assert terminal.count("OP_ENDIF") == 1, terminal
assert terminal.count("OP_RETURN") == 2, terminal

local_fixed = tokens(sys.argv[5])
assert local_fixed.count("OP_IF") == 1, local_fixed
assert local_fixed.count("OP_NOTIF") == 0, local_fixed
assert local_fixed.count("OP_ELSE") == 0, local_fixed
assert local_fixed.count("OP_ENDIF") == 1, local_fixed
assert local_fixed.count("OP_EQUALVERIFY") == 1, local_fixed

literal = tokens(sys.argv[6])
for opcode in ("OP_IF", "OP_NOTIF", "OP_ELSE", "OP_ENDIF"):
    assert opcode not in literal, literal
assert literal.count("OP_VERIFY") == 0, literal

nested = tokens(sys.argv[7])
assert nested.count("OP_IF") == 2, nested
assert nested.count("OP_NOTIF") == 0, nested
assert nested.count("OP_ELSE") == 0, nested
assert nested.count("OP_ENDIF") == 2, nested
PY

python3 - "$TMP_DIR/nested_dependent_range.json" \
    "$TMP_DIR/stale_loop_target.json" \
    "$TMP_DIR/empty_range_preserves_target.json" \
    "$TMP_DIR/loop_partial_evaluation.json" \
    "$TMP_DIR/loop_fixed_one_opcode.json" \
    "$TMP_DIR/loop_placeholder_reuse.json" \
    "$TMP_DIR/minimum_range_nested.json" \
    "$TMP_DIR/empty_inner_preserves_outer.json" \
    "$TMP_DIR/negative_step_range.json" <<'PY'
import json
import sys

def tokens(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)["lock"]["asm"].split()

nested = tokens(sys.argv[1])
stale = tokens(sys.argv[2])
empty = tokens(sys.argv[3])
partial = tokens(sys.argv[4])
fixed_one = tokens(sys.argv[5])
placeholder_reuse = tokens(sys.argv[6])
minimum = tokens(sys.argv[7])
empty_inner = tokens(sys.argv[8])
negative_step = tokens(sys.argv[9])

# Inner counts are 1, 2 and 3 rather than reusing the last inner RangePlan.
assert nested.count("<self.marker20>") == 6, nested
assert stale.count("<self.marker20>") == 1, stale
assert empty.count("<self.marker20>") == 1, empty

for opcode in ("OP_IF", "OP_NOTIF", "OP_ELSE", "OP_ENDIF"):
    assert opcode not in stale, stale
    assert opcode not in empty, empty

# Every i*2+1 expression is lowered to its final fixed value.
assert "OP_MUL" not in partial, partial
assert "OP_ADD" not in partial, partial
assert partial.count("<self.marker20>") == 3, partial

assert fixed_one.count("OP_1ADD") == 1, fixed_one
assert "OP_ADD" not in fixed_one, fixed_one
assert placeholder_reuse.count("<self.marker20>") == 1, placeholder_reuse
assert placeholder_reuse.count("OP_DUP") == 1, placeholder_reuse
assert minimum.count("<self.marker20>") == 1, minimum
assert empty_inner.count("<self.marker20>") == 2, empty_inner
assert negative_step.count("<self.marker20>") == 3, negative_step
PY

python3 - "$TMP_DIR/op_return_push_padding.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    data = json.load(f)

script = bytes.fromhex(data["lock"]["hex"])
op_return = script.rfind(b"\x6a")
expected_padding = bytes([0x1d]) + bytes([0xff]) * 29

assert op_return >= 0, data["lock"]["hex"]
assert script[op_return + 1:] == expected_padding, data["lock"]["hex"]
assert (op_return + 1 + len(expected_padding)) % 64 == 0, data["lock"]["hex"]
PY

python3 - "$TMP_DIR/global_basic.json" \
    "$TMP_DIR/global_private.json" \
    "$TMP_DIR/global_unused.json" \
    "$TMP_DIR/global_unused_baseline.json" \
    "$TMP_DIR/global_literal.json" \
    "$TMP_DIR/global_literal_baseline.json" \
    "$TMP_DIR/global_call_namespace.json" \
    "$TMP_DIR/global_call_namespace_baseline.json" \
    "$TMP_DIR/global_library_user.json" \
    "$TMP_DIR/global_library_baseline.json" <<'PY'
import json
import sys

def load(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)

basic = load(sys.argv[1])
private = load(sys.argv[2])
unused = load(sys.argv[3])
unused_baseline = load(sys.argv[4])
literal = load(sys.argv[5])
literal_baseline = load(sys.argv[6])
call_namespace = load(sys.argv[7])
call_namespace_baseline = load(sys.argv[8])
library = load(sys.argv[9])
library_baseline = load(sys.argv[10])

basic_tokens = basic["lock"]["asm"].split()
# Interpreter ASM includes the nine-byte direct-push length prefix.
assert "09544243323054415045" in basic_tokens, basic["lock"]["asm"]
assert "OP_9" in basic_tokens, basic["lock"]["asm"]
assert "<TapeFlag>" not in basic["lock"]["hex"], basic["lock"]["hex"]
assert "<TapeFlagSize>" not in basic["lock"]["hex"], basic["lock"]["hex"]

assert private["lock"] == basic["lock"], (basic["lock"], private["lock"])
assert unused["lock"] == unused_baseline["lock"], (
    unused["lock"], unused_baseline["lock"]
)
assert literal["lock"] == literal_baseline["lock"], (
    literal["lock"], literal_baseline["lock"]
)
assert call_namespace["lock"] == call_namespace_baseline["lock"], (
    call_namespace["lock"], call_namespace_baseline["lock"]
)
assert library["lock"] == library_baseline["lock"], (
    library["lock"], library_baseline["lock"]
)

for artifact in (basic, private, unused, literal, call_namespace, library):
    assert all(
        param.get("name") not in {"TapeFlag", "TapeFlagSize"}
        for entry in artifact.get("abi", [])
        for param in entry.get("params", [])
    ), artifact.get("abi")
    assert all(
        param.get("name") not in {"TapeFlag", "TapeFlagSize"}
        for param in artifact.get("constructorParams", [])
    ), artifact.get("constructorParams")
PY

if [[ ${APC_BUILD_DEBUGGER:-1} == 1 ]]; then
python3 - "$TMP_DIR/global_basic.debug" \
    "$TMP_DIR/debug_default_pc_drift.debug" \
    "$TMP_DIR/debug_default_pc_drift.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    debug = json.load(f)
with open(sys.argv[2], "r", encoding="utf-8") as f:
    drift_debug = json.load(f)
with open(sys.argv[3], "r", encoding="utf-8") as f:
    drift_bytecode = json.load(f)

debug_names = {
    item.get("name")
    for key in ("variables", "localVars")
    for item in debug.get(key, [])
}
for function in debug.get("functions", []):
    debug_names.update(item.get("name") for item in function.get("localVars", []))
    debug_names.update(item.get("name") for item in function.get("parameters", []))
assert "TapeFlag" not in debug_names, debug_names
assert "TapeFlagSize" not in debug_names, debug_names
assert "2" not in debug.get("lineToPC", {}), debug.get("lineToPC")
assert "3" not in debug.get("lineToPC", {}), debug.get("lineToPC")
assert debug.get("lineToPC", {}).get("5"), debug.get("lineToPC")
assert debug.get("lineToPC", {}).get("6"), debug.get("lineToPC")

# AST emission has OP_EQUAL + OP_VERIFY, while peephole commits one fused
# instruction. A stale initial default-path snapshot has two entries and an
# OP_EQUAL at PC 0; the final persisted mapping must match OP_EQUALVERIFY.
assert drift_bytecode["lock"]["asm"].split() == ["OP_EQUALVERIFY"], (
    drift_bytecode["lock"]
)
drift_instructions = drift_debug.get("instructions", [])
assert len(drift_instructions) == 1, drift_instructions
assert drift_instructions[0]["pc"] == 0, drift_instructions
assert drift_instructions[0]["opcode"].lower() == \
    drift_bytecode["lock"]["hex"].lower(), (
        drift_instructions, drift_bytecode["lock"]
    )
PY
fi

echo "Compiler regression checks passed."

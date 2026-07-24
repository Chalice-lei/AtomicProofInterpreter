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

# if 的一条路径只移动变量位置时，也必须参与分支合并并恢复一致布局。
cp "$SCRIPT_DIR/branch_alt_location_merge.ct" "$TMP_DIR/"
(
    cd "$TMP_DIR"
    "$COMPILER" --asa compile branch_alt_location_merge.ct \
        >branch-alt.out 2>branch-alt.err
)
for flag in 2 -1; do
    output="$TMP_DIR/branch-alt-${flag}.out"
    if ! "$COMPILER" --asa run \
        "$TMP_DIR/branch_alt_location_merge.ct" main "$flag" 7 \
        >"$output" 2>&1; then
        echo "Branch alt-location merge failed for flag=$flag" >&2
        sed -n '1,120p' "$output" >&2 || true
        exit 1
    fi
    if ! grep -q "status: finished" "$output" ||
       ! grep -q "int=1" "$output"; then
        echo "Unexpected branch alt-location result for flag=$flag" >&2
        sed -n '1,120p' "$output" >&2 || true
        exit 1
    fi
done

cp "$SCRIPT_DIR/public_function_alt_handoff.ct" "$TMP_DIR/"
(
    cd "$TMP_DIR"
    "$COMPILER" compile public_function_alt_handoff.ct \
        >compile.out 2>compile.err
)

OUTPUT="$TMP_DIR/public_function_alt_handoff.json"
if [[ ! -s "$OUTPUT" ]]; then
    echo "Expected compiler output for public-function alt handoff case" >&2
    sed -n '1,120p' "$TMP_DIR/compile.err" >&2 || true
    exit 1
fi

# 同一合约的 public 函数按顺序协作，共享副栈是公开支持的中继通道。
if ! grep -q 'OP_FROMALTSTACK' "$OUTPUT"; then
    echo "Alternative stack handoff between public functions was lost" >&2
    sed -n '1,200p' "$OUTPUT" >&2
    exit 1
fi

# runner 的逗号分隔阶段链会在同一个 BVM 会话中保留 alt 状态。
phase_output="$TMP_DIR/public-handoff-run.out"
if ! "$COMPILER" run "$SCRIPT_DIR/public_function_alt_handoff.ct" \
    stage_one,stage_two 7 >"$phase_output" 2>&1; then
    echo "Public-function phase-chain runner failed" >&2
    sed -n '1,120p' "$phase_output" >&2 || true
    exit 1
fi
if ! grep -q "status: finished" "$phase_output" ||
   ! grep -q "int=7" "$phase_output"; then
    echo "Public-function phase-chain did not preserve alt state" >&2
    sed -n '1,120p' "$phase_output" >&2 || true
    exit 1
fi

phase_args_output="$TMP_DIR/public-handoff-args-run.out"
if ! "$COMPILER" run \
    "$SCRIPT_DIR/public_function_alt_handoff_args.ct" \
    stage_one,stage_two 7 5 >"$phase_args_output" 2>&1; then
    echo "Public-function phase-chain argument layout failed" >&2
    sed -n '1,120p' "$phase_args_output" >&2 || true
    exit 1
fi
if ! grep -q "status: finished" "$phase_args_output" ||
   ! grep -q "int=12" "$phase_args_output"; then
    echo "Phase-chain arguments were not assigned in function order" >&2
    sed -n '1,120p' "$phase_args_output" >&2 || true
    exit 1
fi

standalone_output="$TMP_DIR/public-handoff-standalone.out"
set +e
"$COMPILER" run "$SCRIPT_DIR/public_function_alt_handoff.ct" stage_two \
    >"$standalone_output" 2>&1
standalone_rc=$?
set -e
if (( standalone_rc == 0 )) ||
   ! grep -q "缺少前置阶段留下的栈状态" "$standalone_output"; then
    echo "Standalone dependent phase did not report missing phase state" >&2
    sed -n '1,120p' "$standalone_output" >&2 || true
    exit 1
fi

# 真实双阶段合约同时覆盖：跨 public 函数中继、循环内分支 alt 顺序，
# 以及 Return 后 immutable suffix 的保留。
cp "$REPO_ROOT/test/contract_file/price_oracle.ct" "$TMP_DIR/"
(
    cd "$TMP_DIR"
    "$COMPILER" --asa compile price_oracle.ct \
        >price-oracle.out 2>price-oracle.err
)
python3 - "$TMP_DIR/price_oracle.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    tokens = json.load(f)["lock"]["asm"].split()

last_return = max(i for i, token in enumerate(tokens) if token == "OP_RETURN")
assert "<self.oracleKeyHash>" in tokens[last_return + 1:], tokens[-20:]
PY

cp "$SCRIPT_DIR/private_fixed_return_no_crash.ct" "$TMP_DIR/"
set +e
(
    cd "$TMP_DIR"
    "$COMPILER" compile private_fixed_return_no_crash.ct \
        >private-return.out 2>private-return.err
)
PRIVATE_RETURN_RC=$?
set -e

if (( PRIVATE_RETURN_RC != 0 )); then
    echo "Private fixed-value return failed (exit $PRIVATE_RETURN_RC)" >&2
    sed -n '1,120p' "$TMP_DIR/private-return.err" >&2 || true
    exit 1
fi

for mode in ast run; do
    for flag in 1 -1; do
        expected=1
        if [[ "$flag" == "-1" ]]; then
            expected=2
        fi
        output="$TMP_DIR/private-return-${mode}-${flag}.out"
        if ! "$COMPILER" "$mode" \
            "$TMP_DIR/private_fixed_return_no_crash.ct" main "$flag" \
            >"$output" 2>&1; then
            echo "Private fixed-value return failed in $mode mode" >&2
            sed -n '1,120p' "$output" >&2 || true
            exit 1
        fi
        if ! grep -q "status: finished" "$output" ||
           ! grep -q "int=$expected" "$output"; then
            echo "Expected private return int=$expected in $mode mode" >&2
            sed -n '1,120p' "$output" >&2 || true
            exit 1
        fi
    done
done

run_return_case() {
    local source_file=$1
    local expected=$2
    shift 2
    local args=("$@")

    for mode in ast run; do
        local output="$TMP_DIR/${source_file%.ct}-${mode}-${args[*]}.out"
        if ! "$COMPILER" --asa "$mode" \
            "$SCRIPT_DIR/$source_file" main "${args[@]}" \
            >"$output" 2>&1; then
            echo "Return edge case failed: $source_file ($mode)" >&2
            sed -n '1,120p' "$output" >&2 || true
            exit 1
        fi
        if ! grep -q "status: finished" "$output" ||
           ! grep -q "int=$expected" "$output"; then
            echo "Unexpected return edge result: $source_file ($mode)" >&2
            sed -n '1,120p' "$output" >&2 || true
            exit 1
        fi
    done
}

# lowercase return 的三类高风险栈形：多返回值、返回值原先在 alt、
# 以及两层嵌套的单边提前返回。
run_return_case private_multi_one_sided_return.ct 12 1
run_return_case private_multi_one_sided_return.ct 140 -1
run_return_case private_alt_return.ct 9 4
run_return_case private_nested_one_sided_return.ct 7 1 1
run_return_case private_nested_one_sided_return.ct 34 1 -1
run_return_case private_nested_one_sided_return.ct 35 -1 1

echo "Stack safety regression tests passed."

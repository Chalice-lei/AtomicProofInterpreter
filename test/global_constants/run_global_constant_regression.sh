#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
INTERPRETER=${1:-"$REPO_ROOT/build/bin/utxo_Interpreter"}
export APC_STDLIB_PATH=${APC_STDLIB_PATH:-"$REPO_ROOT/stdlib"}

if [[ ! -x "$INTERPRETER" ]]; then
    echo "Interpreter not found or not executable: $INTERPRETER" >&2
    exit 1
fi
INTERPRETER=$(cd "$(dirname "$INTERPRETER")" && pwd)/$(basename "$INTERPRETER")

CASE_TMP=$(mktemp -d)
trap 'rm -rf "$CASE_TMP"' EXIT
cp "$SCRIPT_DIR"/*.ct "$CASE_TMP"/

run_compile() {
    local file=$1
    (
        cd "$CASE_TMP"
        "$INTERPRETER" compile "$file" >"$file.out" 2>"$file.err"
    )
}

expect_success() {
    local file=$1
    local stem=${file%.ct}
    if ! run_compile "$file"; then
        echo "Expected successful compilation: $file" >&2
        sed -n '1,120p' "$CASE_TMP/$file.err" >&2 || true
        sed -n '1,120p' "$CASE_TMP/$file.out" >&2 || true
        exit 1
    fi
    if [[ ! -s "$CASE_TMP/$stem.json" ]]; then
        echo "Expected JSON artifact for: $file" >&2
        exit 1
    fi
}

expect_failure() {
    local file=$1
    local diagnostic=$2
    local stem=${file%.ct}

    set +e
    run_compile "$file"
    local compile_rc=$?
    set -e

    if [[ $compile_rc -eq 0 ]]; then
        echo "Expected compilation failure: $file" >&2
        exit 1
    fi
    if [[ -e "$CASE_TMP/$stem.json" ]]; then
        echo "Failing case unexpectedly produced JSON: $file" >&2
        exit 1
    fi
    if ! grep -Fq -- "$diagnostic" \
        "$CASE_TMP/$file.err" "$CASE_TMP/$file.out"; then
        echo "Missing diagnostic '$diagnostic' for: $file" >&2
        sed -n '1,120p' "$CASE_TMP/$file.err" >&2 || true
        sed -n '1,120p' "$CASE_TMP/$file.out" >&2 || true
        exit 1
    fi
}

expect_success basic.ct
expect_success basic_baseline.ct
expect_success private_usage.ct
expect_success private_baseline.ct
expect_success library_usage.ct
expect_success library_baseline.ct
expect_success unused.ct
expect_success unused_baseline.ct
expect_success runtime.ct
expect_success literal_types.ct
expect_success expression_contexts.ct
expect_success constructor_usage.ct

expect_failure bad_duplicate.ct "duplicate global constant 'TapeFlag'"
expect_failure bad_member_conflict.ct \
    "global constant 'main' conflicts with a function or struct name"
expect_failure bad_parameter_shadow.ct \
    "function parameter 'TapeFlag' cannot shadow or assign global constant"
expect_failure bad_local_shadow.ct \
    "local variable 'TapeFlagSize' cannot shadow or assign global constant"
expect_failure bad_reassignment.ct \
    "assignment target 'TapeFlagSize' cannot shadow or assign global constant"
expect_failure bad_index_assignment.ct \
    "assignment target 'TapeFlag' cannot shadow or assign global constant"
expect_failure bad_destructure_shadow.ct \
    "destructure target 'TapeFlag' cannot shadow or assign global constant"
expect_failure bad_loop_shadow.ct \
    "for-loop target 'TapeFlagSize' cannot shadow or assign global constant"
expect_failure bad_late_declaration.ct \
    "global declarations must appear before"
expect_failure bad_in_function.ct \
    "global declarations are only allowed at the beginning of a Contract body"
expect_failure bad_non_literal.ct \
    "global constant initializer must be a scalar literal"
expect_failure bad_library_user.ct \
    "global declarations are only allowed at the beginning of a Contract body"
expect_failure bad_unused_number.ct "integer literal out of range"
expect_failure bad_unused_hex.ct "invalid hexadecimal literal"
expect_failure bad_unused_address.ct "invalid P2PKH address literal"
expect_failure bad_constructor_shadow.ct \
    "function parameter 'TapeFlagSize' cannot shadow or assign global constant"
if grep -Eq 'line 0|:0(:|,)' \
    "$CASE_TMP/bad_constructor_shadow.ct.err" \
    "$CASE_TMP/bad_constructor_shadow.ct.out"; then
    echo "Constructor/global conflict used an invalid source location" >&2
    exit 1
fi

python3 - \
    "$CASE_TMP/basic.json" \
    "$CASE_TMP/basic_baseline.json" \
    "$CASE_TMP/private_usage.json" \
    "$CASE_TMP/private_baseline.json" \
    "$CASE_TMP/library_usage.json" \
    "$CASE_TMP/library_baseline.json" \
    "$CASE_TMP/unused.json" \
    "$CASE_TMP/unused_baseline.json" <<'PY'
import json
import sys


def load(path):
    with open(path, "r", encoding="utf-8") as stream:
        return json.load(stream)


(
    basic,
    basic_baseline,
    private,
    private_baseline,
    library,
    library_baseline,
    unused,
    unused_baseline,
) = (
    load(path) for path in sys.argv[1:]
)

assert basic["lock"] == basic_baseline["lock"]
assert private["lock"] == private_baseline["lock"]
assert library["lock"] == library_baseline["lock"]
assert unused["lock"] == unused_baseline["lock"]

tokens = basic["lock"]["asm"].split()
assert any(token.endswith("544243323054415045") for token in tokens)
assert "OP_9" in tokens
assert "<TapeFlag>" not in basic["lock"]["hex"]
assert "<TapeFlagSize>" not in basic["lock"]["hex"]

for artifact in (basic, library, unused):
    names = {
        param.get("name")
        for entry in artifact.get("abi", [])
        for param in entry.get("params", [])
    }
    names.update(
        param.get("name")
        for param in artifact.get("constructorParams", [])
    )
    assert not names.intersection(
        {"TapeFlag", "TapeFlagSize", "UnusedText", "UnusedNumber"}
    )
PY

# 公共 frontend：AST interpreter 必须看到已替换的 LiteralNode。
"$INTERPRETER" ast "$CASE_TMP/runtime.ct" main \
    >"$CASE_TMP/ast.out" 2>"$CASE_TMP/ast.err"
grep -Eq '(^|[^0-9])9([^0-9]|$)' "$CASE_TMP/ast.out"

# Bytecode VM、普通 REPL 和 compiler REPL 仅在当前构建支持时校验；
# 不支持时只允许明确的 BUILD_DEBUGGER 诊断。
set +e
"$INTERPRETER" run "$CASE_TMP/runtime.ct" main \
    >"$CASE_TMP/run.out" 2>"$CASE_TMP/run.err"
run_rc=$?
set -e
if [[ $run_rc -eq 0 ]]; then
    grep -Eq '(^|[^0-9])9([^0-9]|$)' "$CASE_TMP/run.out"
elif ! grep -Fq 'requires BUILD_DEBUGGER=ON' \
    "$CASE_TMP/run.out" "$CASE_TMP/run.err"; then
    sed -n '1,120p' "$CASE_TMP/run.err" >&2 || true
    exit "$run_rc"
fi

"$INTERPRETER" shell "$CASE_TMP/runtime.ct" -l none \
    >"$CASE_TMP/shell.out" 2>"$CASE_TMP/shell.err" <<'EOF'
main()
exit
EOF
if ! grep -Eq 'Out\[[0-9]+\]: 9' "$CASE_TMP/shell.out"; then
    if ! grep -Fq 'requires BUILD_DEBUGGER=ON' \
        "$CASE_TMP/shell.out" "$CASE_TMP/shell.err"; then
        sed -n '1,120p' "$CASE_TMP/shell.out" >&2 || true
        sed -n '1,120p' "$CASE_TMP/shell.err" >&2 || true
        exit 1
    fi
fi

"$INTERPRETER" compiler-repl -l none \
    >"$CASE_TMP/compiler-repl.out" 2>"$CASE_TMP/compiler-repl.err" <<'EOF'
Contract ReplGlobalContract:
    global Answer = 9
    def main():
        Return Answer

exit
EOF
if ! grep -Eq 'Out\[[0-9]+\]: 9' "$CASE_TMP/compiler-repl.out"; then
    if ! grep -Fq 'requires BUILD_DEBUGGER=ON' \
        "$CASE_TMP/compiler-repl.out" "$CASE_TMP/compiler-repl.err"; then
        sed -n '1,120p' "$CASE_TMP/compiler-repl.out" >&2 || true
        sed -n '1,120p' "$CASE_TMP/compiler-repl.err" >&2 || true
        exit 1
    fi
fi

echo "Global constant regression tests passed."

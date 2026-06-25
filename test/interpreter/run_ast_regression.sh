#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
COMPILER=${APC_COMPILER:-"$REPO_ROOT/build/bin/utxo_interpreter"}
export APC_STDLIB_PATH=${APC_STDLIB_PATH:-"$REPO_ROOT/stdlib"}

if [[ ! -x "$COMPILER" ]]; then
    echo "Interpreter not found or not executable: $COMPILER" >&2
    exit 1
fi

TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

run_expect_int() {
    local expected=$1
    shift

    local name="ast_case"
    for arg in "$@"; do
        if [[ "$arg" == *.ct ]]; then
            name=$(basename "$arg" .ct)
            break
        fi
    done
    local out="$TMP_DIR/$name.out"

    if ! "$COMPILER" "$@" >"$out" 2>&1; then
        echo "Expected AST run to succeed: $*" >&2
        sed -n '1,160p' "$out" >&2 || true
        exit 1
    fi

    if ! grep -q "status: finished" "$out"; then
        echo "Expected finished status for: $*" >&2
        sed -n '1,160p' "$out" >&2 || true
        exit 1
    fi

    if ! grep -q "int=$expected" "$out"; then
        echo "Expected return int=$expected for: $*" >&2
        sed -n '1,160p' "$out" >&2 || true
        exit 1
    fi
}

run_expect_int 9 \
    ast "$SCRIPT_DIR/ast_minimal_return.ct" main 7

run_expect_int 12 \
    ast "$SCRIPT_DIR/ast_function_loop.ct" main

run_expect_int 3 \
    ast "$SCRIPT_DIR/ast_data_builtins.ct" main

run_expect_int 45 \
    ast "$SCRIPT_DIR/ast_lvalue_assignment.ct" main

run_expect_int 45 \
    ast "$SCRIPT_DIR/ast_tx_context.ct" main \
    --txfile "$SCRIPT_DIR/ast_tx_context.json"

CHAIN_CASE="$TMP_DIR/chained_method.ct"
cat >"$CHAIN_CASE" <<'CT'
Contract ChainedMethod:
    def main():
        packet = 0x010203
        Return(BinToNum(packet.Clone().Slice(1, 1)))
CT

run_expect_int 2 ast "$CHAIN_CASE" main

echo "AST interpreter regression checks passed."

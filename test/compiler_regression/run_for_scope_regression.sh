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

for mode in ast run; do
    output="$TMP_DIR/${mode}.out"
    if ! "$COMPILER" "$mode" \
        "$SCRIPT_DIR/for_scope_redeclare.ct" main >"$output" 2>&1; then
        echo "Expected $mode execution to succeed for for_scope_redeclare.ct" >&2
        sed -n '1,160p' "$output" >&2 || true
        exit 1
    fi

    if ! grep -q "status: finished" "$output" ||
       ! grep -q "int=7" "$output"; then
        echo "Expected $mode result int=7 for for_scope_redeclare.ct" >&2
        sed -n '1,160p' "$output" >&2 || true
        exit 1
    fi
done

for mode in ast run; do
    output="$TMP_DIR/existing_target_${mode}.out"
    if ! "$COMPILER" "$mode" \
        "$SCRIPT_DIR/for_scope_redeclare.ct" existing_target >"$output" 2>&1; then
        echo "Expected $mode execution to succeed for an existing loop target" >&2
        sed -n '1,160p' "$output" >&2 || true
        exit 1
    fi

    if ! grep -q "status: finished" "$output" ||
       ! grep -q "int=2" "$output"; then
        echo "Expected $mode existing loop target result int=2" >&2
        sed -n '1,160p' "$output" >&2 || true
        exit 1
    fi
done

for mode in ast run; do
    output="$TMP_DIR/nested_${mode}.out"
    if ! "$COMPILER" "$mode" \
        "$SCRIPT_DIR/for_scope_redeclare.ct" nested >"$output" 2>&1; then
        echo "Expected $mode execution to succeed for nested for scopes" >&2
        sed -n '1,160p' "$output" >&2 || true
        exit 1
    fi

    if ! grep -q "status: finished" "$output" ||
       ! grep -q "int=10" "$output"; then
        echo "Expected $mode nested for result int=10" >&2
        sed -n '1,160p' "$output" >&2 || true
        exit 1
    fi
done

for mode in ast run; do
    output="$TMP_DIR/target_escape_${mode}.out"
    if ! "$COMPILER" "$mode" \
        "$SCRIPT_DIR/for_scope_target_escape.ct" main >"$output" 2>&1; then
        echo "Expected non-empty loop target to remain visible in $mode" >&2
        sed -n '1,160p' "$output" >&2 || true
        exit 1
    fi

    if ! grep -q "status: finished" "$output" ||
       ! grep -q "int=1" "$output"; then
        echo "Expected non-empty loop target result int=1 in $mode" >&2
        sed -n '1,160p' "$output" >&2 || true
        exit 1
    fi
done

empty_escape_output="$TMP_DIR/empty_target_escape.out"
if "$COMPILER" compile \
    "$SCRIPT_DIR/for_scope_empty_target_escape.ct" \
    >"$empty_escape_output" 2>&1; then
    echo "Expected an empty loop not to introduce its target" >&2
    exit 1
fi

if ! grep -q "Undeclared variable: 'i'" "$empty_escape_output"; then
    echo "Expected an undeclared empty-loop target diagnostic" >&2
    sed -n '1,160p' "$empty_escape_output" >&2 || true
    exit 1
fi

echo "For scope regression checks passed."

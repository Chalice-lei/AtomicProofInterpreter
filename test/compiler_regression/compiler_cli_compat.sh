#!/usr/bin/env bash
set -euo pipefail

INTERPRETER=${APC_INTERPRETER:?APC_INTERPRETER must point to utxo_Interpreter}

prefix=()
while [[ ${1:-} == "--asa" || ${1:-} == "--allow-subscope-altstack" ]]; do
    prefix+=("$1")
    shift
done

exec "$INTERPRETER" "${prefix[@]}" compile "$@"

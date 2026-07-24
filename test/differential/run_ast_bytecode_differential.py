#!/usr/bin/env python3
"""Deterministic AST/bytecode differential regression corpus."""

from __future__ import annotations

import os
import random
import re
import subprocess
import tempfile
from pathlib import Path


SEED = 20260723
ROOT = Path(__file__).resolve().parents[2]
COMPILER = Path(
    os.environ.get("APC_COMPILER", ROOT / "build/bin/utxo_Interpreter")
).resolve()
TOP_VALUE = re.compile(r"^\s*\[\d+\].*?int=(-?\d+)", re.MULTILINE)
STATUS = re.compile(r"status:\s*([a-z_]+)")


def run(mode: str, source: Path, argument: int) -> tuple[int, str | None, int | None, str]:
    process = subprocess.run(
        [str(COMPILER), mode, str(source), "main", str(argument)],
        cwd=source.parent,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=10,
        env={**os.environ, "APC_STDLIB_PATH": str(ROOT / "stdlib")},
        check=False,
    )
    status_match = STATUS.search(process.stdout)
    values = TOP_VALUE.findall(process.stdout)
    return (
        process.returncode,
        status_match.group(1) if status_match else None,
        int(values[-1]) if values else None,
        process.stdout,
    )


def corpus() -> list[tuple[str, str]]:
    rng = random.Random(SEED)
    cases: list[tuple[str, str]] = []

    for index in range(10):
        base, left, right = (rng.randint(1, 30) for _ in range(3))
        cases.append(
            (
                f"branch_fixed_{index}",
                f"""Contract C{index}:
    def main(flag: int):
        value: int = {base}
        if flag.Clone() > 0:
            value = {left}
        else:
            value = {right}
        Return(value)
""",
            )
        )

    for index in range(8):
        base, changed = rng.randint(1, 30), rng.randint(31, 60)
        cases.append(
            (
                f"branch_one_sided_{index}",
                f"""Contract O{index}:
    def main(flag: int):
        value: int = {base}
        if flag.Clone() > 0:
            value = {changed}
        else:
            local_only: int = {rng.randint(61, 90)}
        Return(value)
""",
            )
        )

    for index in range(7):
        values = [rng.randint(1, 9) for _ in range(4)]
        cases.append(
            (
                f"branch_two_values_{index}",
                f"""Contract T{index}:
    def main(flag: int):
        first: int = 0
        second: int = 0
        if flag.Clone() > 0:
            first = {values[0]}
            second = {values[1]}
        else:
            first = {values[2]}
            second = {values[3]}
        Return(first * 10 + second)
""",
            )
        )

    for index in range(7):
        a, b, c = (rng.randint(10, 80) for _ in range(3))
        cases.append(
            (
                f"nested_branch_{index}",
                f"""Contract N{index}:
    def main(flag: int):
        value: int = 0
        if flag.Clone() > 0:
            if flag.Clone() > 1:
                value = {a}
            else:
                value = {b}
        else:
            value = {c}
        Return(value)
""",
            )
        )

    for index in range(5):
        count, base = rng.randint(1, 4), rng.randint(1, 20)
        cases.append(
            (
                f"for_scope_{index}",
                f"""Contract F{index}:
    def main(flag: int):
        value: int = {base}
        for i in Range(0, {count}, 1):
            local: int = i
            value = value + local
        Return(value)
""",
            )
        )

    for index in range(5):
        left, right, temporary = (
            rng.randint(1, 50) for _ in range(3)
        )
        cases.append(
            (
                f"expression_statement_{index}",
                f"""Contract E{index}:
    def main(flag: int):
        value: int = 0
        if flag.Clone() > 0:
            flag.Clone() + {temporary}
            value = {left}
        else:
            flag.Clone() - {temporary}
            value = {right}
        Return(value)
""",
            )
        )

    for index in range(4):
        left, right = rng.randint(1, 30), rng.randint(31, 60)
        cases.append(
            (
                f"private_both_return_{index}",
                f"""Contract P{index}:
    def _pick(flag: int):
        if flag.Clone() > 0:
            left = Push({left})
            return left
        else:
            right = Push({right})
            return right
        unreachable = Push(99)
        return unreachable

    def main(flag: int):
        result = _pick(flag)
        Return(result)
""",
            )
        )

    for index in range(4):
        base, early, step = (
            rng.randint(1, 20),
            rng.randint(21, 40),
            rng.randint(1, 5),
        )
        cases.append(
            (
                f"private_one_sided_return_{index}",
                f"""Contract Q{index}:
    def _pick(flag: int):
        result = Push({base})
        if flag.Clone() > 0:
            early = Push({early})
            return early
        else:
            result = result + {step}
        result = result + {step}
        return result

    def main(flag: int):
        Return(_pick(flag))
""",
            )
        )

    for index in range(5):
        a, b, c = (rng.randint(10, 90) for _ in range(3))
        cases.append(
            (
                f"nested_script_return_{index}",
                f"""Contract R{index}:
    def main(flag: int):
        if flag.Clone() > 0:
            if flag.Clone() > 1:
                Return({a})
            else:
                Return({b})
        else:
            Return({c})
""",
            )
        )

    assert len(cases) == 55
    return cases


def main() -> int:
    if not COMPILER.is_file():
        raise SystemExit(f"compiler not found: {COMPILER}")

    failures: list[str] = []
    executions = 0
    argument_count = 0
    with tempfile.TemporaryDirectory(prefix="apc-differential-") as directory:
        temp = Path(directory)
        for case_index, (name, source_text) in enumerate(corpus()):
            source = temp / f"{name}.ct"
            source.write_text(source_text, encoding="utf-8")
            arguments = (-1, 0, 2) if case_index == 0 else (-1, 2)
            argument_count += len(arguments)
            for argument in arguments:
                ast = run("ast", source, argument)
                bytecode = run("run", source, argument)
                executions += 2
                if ast[:3] != bytecode[:3] or ast[1] != "finished":
                    failures.append(
                        f"{name}({argument}): AST={ast[:3]}, "
                        f"bytecode={bytecode[:3]}\n{source_text}\n"
                        f"bytecode output:\n{bytecode[3][-1200:]}"
                    )

    assert argument_count == 111
    assert executions == 222
    if failures:
        print("\n\n".join(failures[:10]))
        print(
            f"differential failures: {len(failures)} / "
            f"{argument_count} argument sets"
        )
        return 1

    print(
        f"Differential checks passed: seed={SEED}, programs=55, "
        f"arguments={argument_count}, executions={executions}."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

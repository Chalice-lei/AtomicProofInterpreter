#!/usr/bin/env python3
"""Daily monitoring report for AtomicProofCompiler.

The script is intentionally self-contained so it can run both locally and in
GitHub Actions. It collects recent repository changes, builds the compiler,
runs regression checks, records basic performance/artifact metrics, and queries
GitHub for issue, PR, and security activity when a token is available.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import defaultdict
from typing import Any


DEFAULT_REPORT = pathlib.Path("build/monitoring/daily-monitoring-report.md")
DEFAULT_JSON_REPORT = pathlib.Path("build/monitoring/daily-monitoring-report.json")


@dataclasses.dataclass
class CommandResult:
    name: str
    command: list[str]
    returncode: int
    duration_seconds: float
    stdout_tail: str
    stderr_tail: str

    @property
    def ok(self) -> bool:
        return self.returncode == 0


def tail(text: str, max_lines: int = 80) -> str:
    lines = text.strip().splitlines()
    return "\n".join(lines[-max_lines:]) if lines else ""


def run_command(
    name: str,
    command: list[str],
    cwd: pathlib.Path,
    env: dict[str, str] | None = None,
    timeout: int | None = None,
) -> CommandResult:
    start = time.monotonic()
    try:
        completed = subprocess.run(
            command,
            cwd=str(cwd),
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
        returncode = completed.returncode
        stdout = completed.stdout
        stderr = completed.stderr
    except subprocess.TimeoutExpired as exc:
        returncode = 124
        stdout = exc.stdout if isinstance(exc.stdout, str) else ""
        stderr = exc.stderr if isinstance(exc.stderr, str) else ""
        stderr = tail(stderr + f"\nCommand timed out after {timeout} seconds.")
    duration = time.monotonic() - start
    return CommandResult(
        name=name,
        command=command,
        returncode=returncode,
        duration_seconds=duration,
        stdout_tail=tail(stdout),
        stderr_tail=tail(stderr),
    )


def capture_stdout(command: list[str], cwd: pathlib.Path, timeout: int = 60) -> str:
    try:
        completed = subprocess.run(
            command,
            cwd=str(cwd),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return ""
    return completed.stdout.strip() if completed.returncode == 0 else ""


def git_output(repo_root: pathlib.Path, args: list[str]) -> str:
    return capture_stdout(["git", *args], repo_root, timeout=60)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1],
        help="Repository root. Defaults to the parent of scripts/.",
    )
    parser.add_argument(
        "--since-hours",
        type=float,
        default=24.0,
        help="Window for recent commits, issues, and PRs.",
    )
    parser.add_argument(
        "--since",
        default=None,
        help="Optional git date expression. Defaults to '<since-hours> hours ago'.",
    )
    parser.add_argument(
        "--build-dir",
        type=pathlib.Path,
        default=pathlib.Path("build/monitor"),
        help="Build directory relative to the repository root unless absolute.",
    )
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=DEFAULT_REPORT,
        help="Markdown report path.",
    )
    parser.add_argument(
        "--json-output",
        type=pathlib.Path,
        default=DEFAULT_JSON_REPORT,
        help="JSON report path.",
    )
    parser.add_argument(
        "--sample-contract",
        type=pathlib.Path,
        default=pathlib.Path("test/contract_file/counter.ct"),
        help="Contract file used for a simple compile timing metric.",
    )
    parser.add_argument(
        "--sample-arg",
        action="append",
        default=None,
        help="Argument passed before the sample contract. Repeat for multiple arguments.",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=max((os.cpu_count() or 2) - 1, 1),
        help="Parallel build jobs.",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Collect repository/GitHub data without building or testing.",
    )
    parser.add_argument(
        "--skip-github",
        action="store_true",
        help="Skip GitHub API collection even when a token is available.",
    )
    parser.add_argument(
        "--github-repository",
        default=os.environ.get("GITHUB_REPOSITORY", ""),
        help="owner/repo name for GitHub API calls.",
    )
    args = parser.parse_args()
    if args.sample_arg is None:
        args.sample_arg = ["--asa", "compile"]
    return args


def resolve_under_repo(repo_root: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    return path if path.is_absolute() else repo_root / path


def collect_git_changes(
    repo_root: pathlib.Path, since_expr: str
) -> tuple[list[dict[str, str]], list[str], str]:
    pretty = "%H%x1f%h%x1f%an%x1f%ad%x1f%s"
    log_output = git_output(
        repo_root,
        ["log", f"--since={since_expr}", f"--pretty=format:{pretty}", "--date=iso-strict"],
    )
    commits: list[dict[str, str]] = []
    for line in log_output.splitlines():
        parts = line.split("\x1f", 4)
        if len(parts) == 5:
            commits.append(
                {
                    "sha": parts[0],
                    "short_sha": parts[1],
                    "author": parts[2],
                    "date": parts[3],
                    "subject": parts[4],
                }
            )

    base = git_output(repo_root, ["rev-list", "-1", f"--before={since_expr}", "HEAD"])
    head = git_output(repo_root, ["rev-parse", "HEAD"])
    if base and head and base != head:
        files_output = git_output(repo_root, ["diff", "--name-only", f"{base}..HEAD"])
        changed_files = sorted({line for line in files_output.splitlines() if line})
    elif commits:
        files_output = git_output(
            repo_root,
            ["log", f"--since={since_expr}", "--name-only", "--pretty=format:"],
        )
        changed_files = sorted({line for line in files_output.splitlines() if line})
    else:
        changed_files = []

    return commits, changed_files, base


def classify_changed_files(files: list[str]) -> dict[str, list[str]]:
    categories: dict[str, list[str]] = defaultdict(list)
    code_suffixes = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".py", ".sh"}
    doc_suffixes = {".md", ".rst", ".txt"}
    dependency_names = {
        "CMakeLists.txt",
        "package.json",
        "package-lock.json",
        "pnpm-lock.yaml",
        "yarn.lock",
        "Cargo.toml",
        "Cargo.lock",
        "go.mod",
        "go.sum",
        "pyproject.toml",
        "requirements.txt",
        "Dockerfile",
        "docker/Dockerfile",
        ".github/dependabot.yml",
    }
    contract_api_prefixes = (
        "stdlib/",
        "scrypt_rewrites/",
        "test/contract_file/",
        "src/parser/",
        "src/lexer/",
        "src/compiler/",
        "src/bytecode/",
    )
    contract_api_files = {
        "doc/GRAMMAR_SPECIFICATION.md",
        "doc/GRAMMAR_EXAMPLES.md",
        "doc/BUILTIN_FUNCTION_DOC.md",
        "doc/BUILTIN_OBJECT_DOC.md",
        "src/ast_to_bytecode_pass.h",
        "src/export_results_pass.h",
        "src/include/token_type.h",
    }
    performance_prefixes = ("src/compiler/", "src/bytecode/", "src/pass/", "src/util/")
    artifact_prefixes = ("scripts/", ".github/workflows/", "docker/")

    for path in files:
        suffix = pathlib.Path(path).suffix
        if suffix in code_suffixes or path in {"main.cpp", "CMakeLists.txt"}:
            categories["code"].append(path)
        if path.startswith("test/"):
            categories["tests"].append(path)
        if suffix in doc_suffixes or path.startswith(("doc/", "docs/", "project_doc/", "thesis/")):
            categories["docs"].append(path)
        if (
            path in dependency_names
            or path.endswith((".lock", ".toml"))
            or path.startswith(".github/workflows/")
        ):
            categories["dependencies"].append(path)
        if (
            suffix == ".ct"
            or path in contract_api_files
            or path.startswith(contract_api_prefixes)
        ):
            categories["contract_api"].append(path)
        if path == "CMakeLists.txt" or path.startswith(performance_prefixes):
            categories["performance"].append(path)
        if path == "CMakeLists.txt" or path.startswith(artifact_prefixes):
            categories["build_artifacts"].append(path)

    return {key: sorted(values) for key, values in categories.items()}


def semver_key(tag: str) -> tuple[int, int, int, str]:
    match = re.search(r"v?(\d+)\.(\d+)\.(\d+)(.*)$", tag)
    if not match:
        return (-1, -1, -1, tag)
    return (
        int(match.group(1)),
        int(match.group(2)),
        int(match.group(3)),
        match.group(4),
    )


def latest_git_tag(repo_root: pathlib.Path, url: str) -> str:
    output = capture_stdout(
        ["git", "ls-remote", "--tags", "--refs", url, "v[0-9]*"],
        repo_root,
        timeout=30,
    )
    tags = []
    for line in output.splitlines():
        if "refs/tags/" in line:
            tags.append(line.rsplit("refs/tags/", 1)[1])
    stable_tags = [tag for tag in tags if re.fullmatch(r"v?\d+\.\d+\.\d+", tag)]
    return max(stable_tags, key=semver_key) if stable_tags else ""


def collect_dependency_status(repo_root: pathlib.Path) -> list[dict[str, Any]]:
    cmake = repo_root / "CMakeLists.txt"
    if not cmake.exists():
        return []
    text = cmake.read_text(encoding="utf-8", errors="replace")
    dependencies: list[dict[str, Any]] = []

    json_tag = ""
    json_match = re.search(r"FetchContent_Declare\(\s*json\b.*?GIT_TAG\s+([^\s\)]+)", text, re.S)
    if json_match:
        json_tag = json_match.group(1).strip()
    if json_tag:
        latest = latest_git_tag(repo_root, "https://github.com/nlohmann/json.git")
        dependencies.append(
            {
                "name": "nlohmann/json",
                "source": "CMake FetchContent",
                "current": json_tag,
                "latest": latest or "unknown",
                "outdated": bool(latest and semver_key(latest) > semver_key(json_tag)),
            }
        )
    return dependencies


def file_size(path: pathlib.Path) -> int:
    try:
        return path.stat().st_size
    except OSError:
        return 0


def collect_artifacts(build_dir: pathlib.Path) -> list[dict[str, Any]]:
    bin_dir = build_dir / "bin"
    if not bin_dir.exists():
        return []
    artifacts = []
    for path in sorted(bin_dir.iterdir()):
        if path.is_file():
            artifacts.append(
                {
                    "path": str(path),
                    "name": path.name,
                    "bytes": file_size(path),
                    "executable": os.access(path, os.X_OK),
                }
            )
    return artifacts


def run_build_and_tests(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root
    build_dir = resolve_under_repo(repo_root, args.build_dir)
    sample_contract = resolve_under_repo(repo_root, args.sample_contract)
    results: list[CommandResult] = []
    metrics: dict[str, Any] = {}

    build_dir.mkdir(parents=True, exist_ok=True)
    configure = run_command(
        "configure",
        [
            "cmake",
            "-S",
            str(repo_root),
            "-B",
            str(build_dir),
            "-DCMAKE_BUILD_TYPE=Release",
        ],
        repo_root,
        timeout=600,
    )
    results.append(configure)

    if configure.ok:
        build = run_command(
            "build",
            ["cmake", "--build", str(build_dir), "--parallel", str(args.jobs)],
            repo_root,
            timeout=900,
        )
        results.append(build)

    compiler = build_dir / "bin" / "utxo_Interpreter"
    env = os.environ.copy()
    env["APC_COMPILER"] = str(compiler)
    env["APC_STDLIB_PATH"] = str(repo_root / "stdlib")

    if compiler.exists() and os.access(compiler, os.X_OK):
        metrics["compiler_bytes"] = file_size(compiler)
        for name, script in (
            ("compiler regression", repo_root / "test/compiler_regression/run_compiler_regression.sh"),
            ("debugger regression", repo_root / "test/debugger_regression/run_debugger_regression.sh"),
        ):
            if script.exists():
                results.append(
                    run_command(name, ["bash", str(script)], repo_root, env=env, timeout=300)
                )

        if sample_contract.exists():
            output_dir = build_dir / "sample-contract"
            output_dir.mkdir(parents=True, exist_ok=True)
            sample = run_command(
                "sample contract compile",
                [str(compiler), *args.sample_arg, str(sample_contract)],
                output_dir,
                env=env,
                timeout=120,
            )
            results.append(sample)
            metrics["sample_contract"] = str(sample_contract.relative_to(repo_root))
            metrics["sample_compile_seconds"] = round(sample.duration_seconds, 3)
            generated = output_dir / f"{sample_contract.stem}.json"
            if generated.exists():
                metrics["sample_output_bytes"] = file_size(generated)

    artifacts = collect_artifacts(build_dir)
    failed = [result for result in results if not result.ok]
    return {
        "build_dir": str(build_dir),
        "commands": [dataclasses.asdict(result) for result in results],
        "artifacts": artifacts,
        "metrics": metrics,
        "status": "FAIL" if failed else "PASS",
    }


def github_request(
    repository: str,
    token: str,
    endpoint: str,
    params: dict[str, str] | None = None,
) -> tuple[Any, str]:
    query = f"?{urllib.parse.urlencode(params)}" if params else ""
    url = f"https://api.github.com/repos/{repository}/{endpoint}{query}"
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "AtomicProofCompiler-daily-monitor",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.loads(response.read().decode("utf-8")), ""
    except urllib.error.HTTPError as exc:
        return None, f"{exc.code} {exc.reason}"
    except (urllib.error.URLError, TimeoutError) as exc:
        return None, str(exc)


def collect_github_activity(
    repository: str,
    token: str,
    since_iso: str,
    skip: bool,
) -> dict[str, Any]:
    if skip:
        return {"status": "SKIPPED", "reason": "disabled by --skip-github"}
    if not repository:
        return {"status": "SKIPPED", "reason": "GITHUB_REPOSITORY is not set"}
    if not token:
        return {"status": "SKIPPED", "reason": "GITHUB_TOKEN is not set"}

    activity: dict[str, Any] = {"status": "OK", "repository": repository}
    issues_payload, issue_error = github_request(
        repository,
        token,
        "issues",
        {"state": "all", "since": since_iso, "per_page": "100"},
    )
    if issue_error:
        activity["issues_error"] = issue_error
    else:
        issues = []
        pull_requests = []
        for item in issues_payload:
            target = pull_requests if "pull_request" in item else issues
            target.append(
                {
                    "number": item.get("number"),
                    "title": item.get("title"),
                    "state": item.get("state"),
                    "updated_at": item.get("updated_at"),
                    "html_url": item.get("html_url"),
                }
            )
        activity["issues"] = issues
        activity["pull_requests"] = pull_requests

    security_endpoints = {
        "dependabot_alerts": ("dependabot/alerts", {"state": "open", "per_page": "100"}),
        "code_scanning_alerts": ("code-scanning/alerts", {"state": "open", "per_page": "100"}),
        "secret_scanning_alerts": ("secret-scanning/alerts", {"state": "open", "per_page": "100"}),
        "repository_security_advisories": ("security-advisories", {"per_page": "100"}),
    }
    security: dict[str, Any] = {}
    for key, (endpoint, params) in security_endpoints.items():
        payload, error = github_request(repository, token, endpoint, params)
        if error:
            security[key] = {"status": "UNAVAILABLE", "error": error}
        else:
            security[key] = {"status": "OK", "count": len(payload), "items": payload[:10]}
    activity["security"] = security
    return activity


def format_bytes(value: int) -> str:
    units = ["B", "KiB", "MiB", "GiB"]
    size = float(value)
    for unit in units:
        if size < 1024 or unit == units[-1]:
            return f"{size:.1f} {unit}" if unit != "B" else f"{int(size)} B"
        size /= 1024
    return f"{value} B"


def command_line(command: list[str]) -> str:
    return " ".join(command)


def print_failure_summary(build: dict[str, Any]) -> None:
    failures = [
        result
        for result in build.get("commands", [])
        if result.get("returncode", 0) != 0
    ]
    if not failures:
        return

    print("Daily monitor failed because command(s) returned non-zero:", file=sys.stderr)
    for result in failures:
        print(
            f"- {result['name']}: exit {result['returncode']} "
            f"({command_line(result['command'])})",
            file=sys.stderr,
        )
        output = result.get("stderr_tail") or result.get("stdout_tail")
        if output:
            print(tail(output, 20), file=sys.stderr)


def render_report(data: dict[str, Any]) -> str:
    lines: list[str] = []
    lines.append("# AtomicProofCompiler Daily Monitoring")
    lines.append("")
    lines.append(f"- Status: {data['overall_status']}")
    lines.append(f"- Generated at: {data['generated_at']}")
    lines.append(f"- Branch: {data['git'].get('branch') or 'unknown'}")
    lines.append(f"- Head: {data['git'].get('head_short') or 'unknown'}")
    lines.append(f"- Window: last {data['window_hours']} hours")
    lines.append("")

    build = data.get("build", {})
    lines.append("## Build and Tests")
    if build:
        lines.append(f"- Status: {build.get('status', 'UNKNOWN')}")
        for result in build.get("commands", []):
            status = "PASS" if result["returncode"] == 0 else "FAIL"
            seconds = result["duration_seconds"]
            lines.append(f"- {result['name']}: {status} ({seconds:.2f}s)")
    else:
        lines.append("- Status: SKIPPED")
    lines.append("")

    commits = data["git"]["commits"]
    lines.append("## Code Commits")
    lines.append(f"- Recent commits: {len(commits)}")
    for commit in commits[:20]:
        lines.append(
            f"- `{commit['short_sha']}` {commit['date']} {commit['author']}: {commit['subject']}"
        )
    if len(commits) > 20:
        lines.append(f"- ... {len(commits) - 20} more commits omitted")
    lines.append("")

    categories = data["git"]["changed_file_categories"]
    lines.append("## Important Changes")
    for category in (
        "code",
        "tests",
        "dependencies",
        "contract_api",
        "performance",
        "build_artifacts",
        "docs",
    ):
        files = categories.get(category, [])
        lines.append(f"- {category}: {len(files)} file(s)")
        for path in files[:12]:
            lines.append(f"  - `{path}`")
        if len(files) > 12:
            lines.append(f"  - ... {len(files) - 12} more")
    lines.append("")

    dependencies = data.get("dependencies", [])
    lines.append("## Dependency Updates")
    if dependencies:
        for dep in dependencies:
            state = "OUTDATED" if dep.get("outdated") else "OK"
            lines.append(
                f"- {dep['name']} ({dep['source']}): {dep['current']} -> {dep['latest']} [{state}]"
            )
    else:
        lines.append("- No tracked dependencies found.")
    lines.append("")

    metrics = build.get("metrics", {}) if build else {}
    artifacts = build.get("artifacts", []) if build else []
    lines.append("## Performance and Artifacts")
    if metrics:
        for key, value in sorted(metrics.items()):
            if key.endswith("_bytes") and isinstance(value, int):
                lines.append(f"- {key}: {format_bytes(value)}")
            else:
                lines.append(f"- {key}: {value}")
    if artifacts:
        lines.append("- Build artifacts:")
        for artifact in artifacts:
            lines.append(
                f"  - `{artifact['name']}` {format_bytes(artifact['bytes'])}"
            )
    if not metrics and not artifacts:
        lines.append("- No metrics collected.")
    lines.append("")

    github = data.get("github", {})
    lines.append("## Issues and Pull Requests")
    if github.get("status") != "OK":
        lines.append(f"- Status: {github.get('status', 'UNKNOWN')}")
        if github.get("reason"):
            lines.append(f"- Reason: {github['reason']}")
        if github.get("issues_error"):
            lines.append(f"- Issue query error: {github['issues_error']}")
    else:
        issues = github.get("issues", [])
        prs = github.get("pull_requests", [])
        lines.append(f"- Issues updated: {len(issues)}")
        lines.append(f"- Pull requests updated: {len(prs)}")
        for prefix, items in (("Issue", issues), ("PR", prs)):
            for item in items[:10]:
                lines.append(
                    f"- {prefix} #{item['number']} [{item['state']}]: {item['title']} ({item['html_url']})"
                )
    lines.append("")

    lines.append("## Security")
    security = github.get("security", {})
    if not security:
        lines.append("- Security data unavailable.")
    else:
        for key, value in security.items():
            if value.get("status") == "OK":
                lines.append(f"- {key}: {value.get('count', 0)} open item(s)")
            else:
                lines.append(f"- {key}: unavailable ({value.get('error', 'unknown error')})")
    lines.append("")

    failures = [
        result
        for result in build.get("commands", [])
        if result.get("returncode", 0) != 0
    ] if build else []
    if failures:
        lines.append("## Failed Command Output")
        for result in failures:
            lines.append(f"### {result['name']}")
            lines.append(f"- Command: `{command_line(result['command'])}`")
            if result.get("stdout_tail"):
                lines.append("")
                lines.append("stdout tail:")
                lines.append("```")
                lines.append(result["stdout_tail"])
                lines.append("```")
            if result.get("stderr_tail"):
                lines.append("")
                lines.append("stderr tail:")
                lines.append("```")
                lines.append(result["stderr_tail"])
                lines.append("```")
            lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    args = parse_args()
    args.repo_root = args.repo_root.resolve()
    args.output = resolve_under_repo(args.repo_root, args.output)
    args.json_output = resolve_under_repo(args.repo_root, args.json_output)
    args.build_dir = resolve_under_repo(args.repo_root, args.build_dir)

    generated_at = dt.datetime.now(dt.UTC)
    since_dt = generated_at - dt.timedelta(hours=args.since_hours)
    since_expr = args.since or f"{args.since_hours:g} hours ago"
    since_iso = since_dt.isoformat().replace("+00:00", "Z")

    commits, changed_files, base = collect_git_changes(args.repo_root, since_expr)
    dependencies = collect_dependency_status(args.repo_root)
    branch = git_output(args.repo_root, ["rev-parse", "--abbrev-ref", "HEAD"])
    head = git_output(args.repo_root, ["rev-parse", "HEAD"])
    remote = git_output(args.repo_root, ["remote", "get-url", "origin"])

    build: dict[str, Any] = {}
    if not args.skip_build:
        build = run_build_and_tests(args)

    github = collect_github_activity(
        args.github_repository,
        os.environ.get("GITHUB_TOKEN", ""),
        since_iso,
        args.skip_github,
    )

    hard_fail = build.get("status") == "FAIL"
    dependency_warn = any(dep.get("outdated") for dep in dependencies)
    security_warn = any(
        value.get("status") == "OK" and value.get("count", 0) > 0
        for value in github.get("security", {}).values()
        if isinstance(value, dict)
    )
    overall_status = "FAIL" if hard_fail else "WARN" if dependency_warn or security_warn else "PASS"

    data = {
        "overall_status": overall_status,
        "generated_at": generated_at.isoformat().replace("+00:00", "Z"),
        "window_hours": args.since_hours,
        "git": {
            "branch": branch,
            "head": head,
            "head_short": head[:12] if head else "",
            "base": base,
            "remote": remote,
            "commits": commits,
            "changed_files": changed_files,
            "changed_file_categories": classify_changed_files(changed_files),
        },
        "dependencies": dependencies,
        "build": build,
        "github": github,
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.json_output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(render_report(data), encoding="utf-8")
    args.json_output.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")

    print(f"Wrote markdown report: {args.output}")
    print(f"Wrote JSON report: {args.json_output}")
    if hard_fail:
        sys.stdout.flush()
        print_failure_summary(build)
    return 1 if hard_fail else 0


if __name__ == "__main__":
    sys.exit(main())

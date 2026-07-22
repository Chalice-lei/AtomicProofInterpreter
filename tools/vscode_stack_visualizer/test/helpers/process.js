"use strict";

const cp = require("child_process");

function runProcess(file, args, options = {})
{
    const timeoutMs = options.timeoutMs || 15000;
    return new Promise((resolve, reject) => {
        const child = cp.spawn(file, args, {
            cwd: options.cwd,
            env: options.env || process.env,
            detached: Boolean(options.killTree && process.platform !== "win32"),
            stdio: [options.input === undefined ? "ignore" : "pipe", "pipe", "pipe"]
        });
        let stdout = "";
        let stderr = "";
        let timedOut = false;
        const kill = (signal) => {
            if (process.platform === "win32" && options.killTree) {
                cp.spawnSync("taskkill", ["/pid", String(child.pid), "/T", "/F"], {
                    stdio: "ignore"
                });
                return;
            }
            if (options.killTree) {
                try {
                    process.kill(-child.pid, signal);
                    return;
                } catch (_error) {
                    // The process may already have exited; fall back to its direct handle.
                }
            }
            child.kill(signal);
        };
        const hardKill = () => {
            if (child.exitCode === null && child.signalCode === null) {
                kill("SIGKILL");
            }
        };
        const timer = setTimeout(() => {
            timedOut = true;
            kill("SIGTERM");
            setTimeout(hardKill, 1000).unref();
        }, timeoutMs);
        child.stdout.on("data", (chunk) => {
            stdout += chunk;
        });
        child.stderr.on("data", (chunk) => {
            stderr += chunk;
        });
        child.on("error", (error) => {
            clearTimeout(timer);
            reject(error);
        });
        child.on("close", (code, signal) => {
            clearTimeout(timer);
            if (timedOut) {
                reject(new Error(
                    `${file} timed out after ${timeoutMs}ms\nstdout:\n${stdout}\nstderr:\n${stderr}`
                ));
                return;
            }
            resolve({code, signal, stdout, stderr});
        });
        if (options.input !== undefined) {
            child.stdin.end(options.input);
        }
    });
}

module.exports = {runProcess};

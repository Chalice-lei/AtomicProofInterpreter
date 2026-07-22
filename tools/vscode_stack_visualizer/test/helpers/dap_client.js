"use strict";

function attach(adapter)
{
    const messages = [];
    const waiters = new Map();
    const subscription = adapter.onDidSendMessage((message) => {
        messages.push(message);
        if (message.type === "response") {
            const waiter = waiters.get(message.request_seq);
            if (waiter) {
                waiters.delete(message.request_seq);
                clearTimeout(waiter.timer);
                waiter.resolve(message);
            }
        }
    });
    let seq = 1;

    async function request(command, args = {}, timeoutMs = 5000)
    {
        const requestSeq = seq++;
        const response = new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                waiters.delete(requestSeq);
                reject(new Error(`DAP ${command} timed out after ${timeoutMs}ms`));
            }, timeoutMs);
            waiters.set(requestSeq, {resolve, reject, timer});
        });
        adapter.handleMessage({
            type: "request",
            seq: requestSeq,
            command,
            arguments: args
        });
        return response;
    }

    function events(name)
    {
        return messages.filter((message) =>
            message.type === "event" && (!name || message.event === name)
        );
    }

    function dispose()
    {
        subscription.dispose();
        for (const waiter of waiters.values()) {
            clearTimeout(waiter.timer);
            waiter.reject(new Error("DAP client disposed"));
        }
        waiters.clear();
        adapter.dispose();
    }

    return {dispose, events, messages, request};
}

function assertSuccess(assert, response)
{
    assert.strictEqual(response.success, true, response.message || "DAP request failed");
    return response.body || {};
}

module.exports = {assertSuccess, attach};

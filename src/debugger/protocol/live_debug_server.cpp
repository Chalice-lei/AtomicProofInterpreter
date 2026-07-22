#include "live_debug_server.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "../../error/error_manager.h"
#include "../../interpreter/function_selection.h"
#include "../../interpreter/parameter_schema.h"
#include "../../util/string_utils.h"
#include "../breakpoint/breakpoint.h"
#include "../breakpoint/breakpoint_manager.h"
#include "../core/debugger_core.h"
#include "../info/debug_info.h"
#include "../inspector/expression_evaluator.h"
#include "../inspector/variable_inspector.h"
#include "../vm/bvm_simulator.h"
#include "../vm/stack_argument.h"
#include "../vm/stack_state.h"
#include "../vm/transaction_data_loader.h"

namespace apc_debug
{
namespace
{

using nlohmann::json;
using apc::util::trim;

bool isHexString(const std::string& value)
{
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isxdigit(ch) != 0;
           });
}

std::optional<std::pair<std::string, std::string>> parseCompactDirectPush(
    const std::string& instruction
)
{
    if (instruction.size() < 2 || instruction.size() % 2 != 0 ||
        !isHexString(instruction)) {
        return std::nullopt;
    }

    int length = 0;
    try {
        length = std::stoi(instruction.substr(0, 2), nullptr, 16);
    } catch (...) {
        return std::nullopt;
    }

    if (length < 1 || length > 75) {
        return std::nullopt;
    }

    const size_t operandChars = static_cast<size_t>(length) * 2;
    if (instruction.size() != 2 + operandChars) {
        return std::nullopt;
    }

    return std::make_pair(instruction.substr(0, 2), instruction.substr(2));
}

std::optional<std::pair<std::string, std::string>> parseNamedPushData(
    const std::string& instruction
)
{
    struct PushDataSpec
    {
        const char* name;
        size_t lengthChars;
    };

    static constexpr PushDataSpec specs[] = {
        {"OP_PUSHDATA1", 2},
        {"OP_PUSHDATA2", 4},
        {"OP_PUSHDATA4", 8},
    };

    for (const auto& spec : specs) {
        const std::string name(spec.name);
        if (instruction.rfind(name, 0) != 0) {
            continue;
        }

        const std::string remaining = trim(instruction.substr(name.size()));
        if (remaining.size() < spec.lengthChars) {
            return std::make_pair(name, std::string());
        }

        return std::make_pair(
            name,
            trim(remaining.substr(spec.lengthChars))
        );
    }

    return std::nullopt;
}

std::string vmStateToString(VMState state)
{
    switch (state) {
        case VMState::READY:
            return "ready";
        case VMState::RUNNING:
            return "running";
        case VMState::PAUSED:
            return "paused";
        case VMState::STEP_MODE:
            return "step";
        case VMState::FINISHED:
            return "finished";
        case VMState::ERROR:
            return "error";
    }
    return "unknown";
}

std::pair<std::string, std::string> parseInstructionText(
    const std::string& instruction
)
{
    const std::string cleaned = trim(instruction);
    if (auto namedPushData = parseNamedPushData(cleaned)) {
        return *namedPushData;
    }

    const size_t space = cleaned.find_first_of(" \t\r\n");
    if (space == std::string::npos) {
        if (auto compactPush = parseCompactDirectPush(cleaned)) {
            return *compactPush;
        }
        return {cleaned, ""};
    }
    return {cleaned.substr(0, space), trim(cleaned.substr(space + 1))};
}

std::string formatInstructionParts(
    const std::string& opcode,
    const std::string& operand
)
{
    if (operand.empty()) {
        return opcode;
    }
    return opcode + " " + operand;
}

std::string summarizeLineInstructions(const json& instructions)
{
    if (!instructions.is_array()) {
        return "";
    }

    std::string summary;
    for (const auto& item : instructions) {
        if (!summary.empty()) {
            summary += "; ";
        }

        summary += "pc " + std::to_string(item.value("pc", 0));
        if (item.value("current", false)) {
            summary += "*";
        }
        summary += ": ";
        summary += formatInstructionParts(
            item.value("opcode", ""),
            item.value("operand", "")
        );
    }
    return summary;
}

std::string stackArgumentStatusMessage(StackArgumentStatus status)
{
    switch (status) {
        case StackArgumentStatus::Parsed:
            return "";
        case StackArgumentStatus::DefaultEmpty:
            return "empty value";
        case StackArgumentStatus::InvalidAddress:
            return "invalid address";
        case StackArgumentStatus::InvalidHex:
            return "invalid hexadecimal value";
        case StackArgumentStatus::InvalidNumber:
            return "invalid numeric value";
    }
    return "invalid value";
}

std::string sourcePathKey(
    const std::string& filename,
    const std::string& primarySource
)
{
    namespace fs = std::filesystem;
    fs::path path(filename.empty() ? primarySource : filename);
    if (path.is_relative() && !primarySource.empty()) {
        std::error_code primaryError;
        fs::path primaryPath(primarySource);
        fs::path primaryAbsolute = primaryPath.is_absolute()
                                       ? primaryPath
                                       : fs::absolute(
                                             primaryPath,
                                             primaryError
                                         );
        if (primaryError) {
            primaryAbsolute = primaryPath;
        }
        path = path.lexically_normal() == primaryPath.lexically_normal()
                   ? primaryAbsolute
                   : primaryAbsolute.parent_path() / path;
    }

    std::error_code error;
    fs::path normalized = fs::weakly_canonical(path, error);
    if (error) {
        normalized = fs::absolute(path, error);
        if (error) {
            normalized = path;
        }
        normalized = normalized.lexically_normal();
    }

    std::string key = normalized.generic_string();
#ifdef _WIN32
    std::transform(
        key.begin(),
        key.end(),
        key.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); }
    );
#endif
    return key;
}

json stackElementToJson(
    const StackElement& value,
    size_t bottomIndex,
    size_t stackSize
)
{
    json item;
    item["index"] = bottomIndex;
    item["depth"] = stackSize > bottomIndex ? stackSize - bottomIndex - 1 : 0;
    item["hex"] = value.toHexString(true);
    item["byteLength"] = value.data.size();
    if (auto intValue = value.toInt()) {
        item["int"] = *intValue;
        item["intString"] = std::to_string(*intValue);
    }
    return item;
}

json stackToJson(const StackState& stack)
{
    const auto values = stack.getAll();
    json result = json::array();
    for (size_t index = 0; index < values.size(); ++index) {
        result.push_back(stackElementToJson(values[index], index, values.size()));
    }
    return result;
}

json callStackToJson(const std::vector<CallFrame>& frames)
{
    json result = json::array();
    for (size_t index = 0; index < frames.size(); ++index) {
        const auto& frame = frames[index];
        json item;
        item["id"] = frame.frameId;
        item["index"] = index;
        item["functionName"] = frame.functionName;
        item["returnPC"] = frame.returnPC;
        item["stackBase"] = frame.stackBase;
        item["frameStart"] = frame.frameStart;
        item["entryPC"] = frame.entryPC;
        item["instructionCount"] = frame.instructionCount;
        item["suspendedPC"] = frame.suspendedPC;
        const SourceLocation& frameLocation = frame.suspendedLocation.isValid()
                                                  ? frame.suspendedLocation
                                                  : frame.callLocation;
        item["source"] = {
            {"file", frameLocation.filename},
            {"line", frameLocation.line},
            {"column", frameLocation.column}
        };
        result.push_back(std::move(item));
    }
    return result;
}

json makeVariable(
    const std::string& name,
    const std::string& value,
    const std::string& type = ""
)
{
    json item;
    item["name"] = name;
    item["value"] = value;
    if (!type.empty()) {
        item["type"] = type;
    }
    return item;
}

json variableValueToJson(const VariableValue& variable)
{
    json item;
    item["name"] = variable.name;
    item["type"] = variable.type;
    item["value"] = variable.isValid ? variable.value : "<unavailable>";
    item["rawValue"] = variable.rawValue;
    item["available"] = variable.isValid;
    item["stackOffset"] = variable.stackOffset;

    json children = json::array();
    for (const auto& field : variable.fields) {
        children.push_back(variableValueToJson(field));
    }
    for (const auto& element : variable.elements) {
        children.push_back(variableValueToJson(element));
    }
    if (!children.empty()) {
        item["children"] = std::move(children);
    }
    return item;
}

json variableValuesToJson(const std::vector<VariableValue>& variables)
{
    json result = json::array();
    for (const auto& variable : variables) {
        result.push_back(variableValueToJson(variable));
    }
    return result;
}

enum class LiveSessionState {
    Ready,
    Running,
    Paused,
    Finished,
    Error,
    Terminated
};

class LiveDebugSession
{
public:
    using EventSink = std::function<void(const json&)>;

    LiveDebugSession(LiveDebugServerOptions options, EventSink eventSink)
        : m_options(std::move(options)), m_eventSink(std::move(eventSink))
    {}

    ~LiveDebugSession()
    {
        m_disconnectRequested.store(true, std::memory_order_release);
        m_pauseAckCondition.notify_all();
        if (m_vm &&
            m_sessionState.load(std::memory_order_acquire) ==
                LiveSessionState::Running) {
            m_vm->requestTerminate();
        }
        joinExecutionThread();
    }

    bool initialize(std::string& errorMessage)
    {
        ErrorManager::getInstance().clear();
        ErrorManager::getInstance().setSourceContent(
            m_options.sourceFile,
            m_options.sourceCode
        );
        ErrorManager::getInstance().setColorOutput(false);
        ErrorManager::getInstance().setShowContext(false);

        m_compileResult = DebuggerCore::compileSource(
            m_options.sourceFile,
            m_options.sourceCode,
            m_options.allowSubscopeAltstack
        );

        if (!m_compileResult.success) {
            errorMessage = m_compileResult.errorMessage;
            return false;
        }

        m_vm = std::make_shared<BVMSimulator>(
            m_compileResult.bytecodeInstructions,
            m_compileResult.debugInfo
        );
        m_breakpoints =
            std::make_shared<BreakpointManager>(m_compileResult.debugInfo);
        m_vm->setBreakpointManager(m_breakpoints);
        m_vm->setEventCallback(
            [this](VMEvent event, const std::string& message) {
                m_lastEvent = event;
                m_lastEventMessage = message;
            }
        );

        if (!configureFunctionAndInputs(errorMessage)) {
            return false;
        }

        // setExecutionRange 会移动 PC；重置一次以按选定函数入口和初始参数
        // 重建顶层调用帧，确保 ready 阶段的 stackTrace/scopes 一致。
        m_vm->reset();
        m_sessionState.store(LiveSessionState::Ready, std::memory_order_release);
        return true;
    }

    json readyEvent() const
    {
        return makeEvent("ready", {{"snapshot", snapshot()}});
    }

    json handleRequest(
        const json& request,
        std::vector<json>& events,
        bool& shouldExit
    )
    {
        const std::string command = request.value("command", "");

        try {
            if (command == "initialize") {
                if (isRunning()) {
                    return runningRequestError(request);
                }
                return makeResponse(
                    request,
                    true,
                    {
                        {"supportsConfigurationDoneRequest", true},
                        {"supportsEvaluate", true},
                        {"supportsBreakpoints", true},
                        {"snapshot", snapshot()}
                    }
                );
            }

            if (command == "setBreakpoints") {
                if (isRunning()) {
                    return runningRequestError(request);
                }
                return handleSetBreakpoints(request);
            }

            if (command == "continue") {
                if (!canExecute()) {
                    return makeResponse(
                        request,
                        false,
                        json::object(),
                        "program is not paused or ready"
                    );
                }
                joinExecutionThread();
                m_vm->clearExecutionRequests();
                if (m_vm->getState() == VMState::READY) {
                    m_vm->start();
                }
                m_sessionState.store(
                    LiveSessionState::Running,
                    std::memory_order_release
                );
                m_pendingContinue.store(true, std::memory_order_release);

                json runningSnapshot = snapshot();
                runningSnapshot["state"] = "running";
                events.push_back(makeEvent(
                    "continued",
                    {{"threadId", 1}, {"allThreadsContinued", true}}
                ));
                return makeResponse(
                    request,
                    true,
                    {
                        {"allThreadsContinued", true},
                        {"snapshot", std::move(runningSnapshot)}
                    }
                );
            }

            if (command == "next" || command == "stepIn" ||
                command == "stepOut") {
                if (!canExecute()) {
                    return makeResponse(
                        request,
                        false,
                        json::object(),
                        "program is not paused or ready"
                    );
                }
                joinExecutionThread();
                m_vm->clearExecutionRequests();
                if (command == "next") {
                    m_vm->stepOver();
                } else if (command == "stepIn") {
                    m_vm->stepIn();
                } else {
                    m_vm->stepOut();
                }
                syncSessionStateFromVm();
                if (const auto event = stopOrTerminateEvent("step")) {
                    events.push_back(*event);
                }
                return makeResponse(
                    request,
                    true,
                    {{"snapshot", snapshot()}}
                );
            }

            if (command == "pause") {
                if (!isRunning()) {
                    return makeResponse(
                        request,
                        false,
                        json::object(),
                        "program is not running"
                    );
                }
                {
                    std::lock_guard<std::mutex> lock(m_pauseAckMutex);
                    m_pauseResponsePending = true;
                }
                m_vm->requestPause();
                return makeResponse(
                    request,
                    true,
                    {{"pauseRequested", true}}
                );
            }

            if (command == "variables") {
                if (isRunning()) {
                    return runningRequestError(request);
                }
                return makeResponse(
                    request,
                    true,
                    {{"variables", variablesForScope(request)}}
                );
            }

            if (command == "evaluate") {
                if (isRunning()) {
                    return runningRequestError(request);
                }
                const std::string expression =
                    request.value("expression", request.value("expr", ""));
                const json result = evaluate(
                    expression,
                    request.value("frameId", static_cast<uint64_t>(0))
                );
                if (!result.value("success", true)) {
                    return makeResponse(
                        request,
                        false,
                        json::object(),
                        result.value("message", "expression evaluation failed")
                    );
                }
                return makeResponse(
                    request,
                    true,
                    {
                        {"result", result.value("result", "")},
                        {"value", result.value("value", json())},
                        {"type", result.value("type", "")}
                    }
                );
            }

            if (command == "snapshot") {
                if (isRunning()) {
                    return runningRequestError(request);
                }
                return makeResponse(request, true, {{"snapshot", snapshot()}});
            }

            if (command == "stackTrace") {
                if (isRunning()) {
                    return runningRequestError(request);
                }
                return makeResponse(
                    request,
                    true,
                    {{"frames", stackFrames()}}
                );
            }

            if (command == "disconnect" || command == "terminate") {
                shouldExit = true;
                m_disconnectRequested.store(true, std::memory_order_release);
                m_pauseAckCondition.notify_all();
                m_pendingContinue.store(false, std::memory_order_release);
                if (isRunning()) {
                    m_vm->requestTerminate();
                }
                joinExecutionThread();
                m_sessionState.store(
                    LiveSessionState::Terminated,
                    std::memory_order_release
                );
                const json finalSnapshot = snapshot();
                if (claimTerminatedEvent()) {
                    events.push_back(makeEvent(
                        "terminated",
                        {{"snapshot", finalSnapshot}}
                    ));
                }
                return makeResponse(
                    request,
                    true,
                    {{"snapshot", finalSnapshot}}
                );
            }

            return makeResponse(
                request,
                false,
                json::object(),
                "unsupported live debug command: " + command
            );
        } catch (const std::exception& e) {
            return makeResponse(request, false, json::object(), e.what());
        }
    }

    void startPendingExecution()
    {
        if (!m_pendingContinue.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        joinExecutionThread();
        m_executionThread = std::thread([this]() {
            m_vm->run();

            if (m_disconnectRequested.load(std::memory_order_acquire)) {
                m_sessionState.store(
                    LiveSessionState::Terminated,
                    std::memory_order_release
                );
                return;
            }

            syncSessionStateFromVm();
            const auto event = stopOrTerminateEvent("continue");
            if (event && m_eventSink) {
                waitForPauseResponse();
                m_eventSink(*event);
            }
        });
    }

    void acknowledgeRequest(const json& request)
    {
        if (request.value("command", "") != "pause") {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_pauseAckMutex);
            m_pauseResponsePending = false;
        }
        m_pauseAckCondition.notify_all();
    }

private:
    struct FrameInspectionContext
    {
        uint64_t frameId = 0;
        size_t pc = 0;
        StackState mainStack;
        StackState altStack;
        bool current = true;
    };

    std::optional<FrameInspectionContext> inspectionContext(
        uint64_t requestedFrameId
    ) const
    {
        if (!m_vm) {
            return std::nullopt;
        }

        const auto& frames = m_vm->getCallStack();
        if (frames.empty()) {
            if (requestedFrameId != 0) {
                return std::nullopt;
            }
            FrameInspectionContext context;
            context.pc = m_vm->getPC();
            context.mainStack = m_vm->getMainStack();
            context.altStack = m_vm->getAltStack();
            return context;
        }

        const CallFrame* selected = nullptr;
        if (requestedFrameId == 0) {
            selected = &frames.back();
        } else {
            for (const auto& frame : frames) {
                if (frame.frameId == requestedFrameId) {
                    selected = &frame;
                    break;
                }
            }
        }
        if (!selected) {
            return std::nullopt;
        }

        FrameInspectionContext context;
        context.frameId = selected->frameId;
        if (selected == &frames.back()) {
            context.pc = m_vm->getPC();
            context.mainStack = m_vm->getMainStack();
            context.altStack = m_vm->getAltStack();
        } else {
            context.current = false;
            context.pc = selected->suspendedPC;
            context.mainStack.restore(selected->suspendedMainStack);
            context.altStack.restore(selected->suspendedAltStack);
        }
        return context;
    }

    bool configureFunctionAndInputs(std::string& errorMessage)
    {
        const json* structsJson = nullptr;
        if (m_compileResult.jsonData.contains("structs") &&
            m_compileResult.jsonData["structs"].is_array()) {
            structsJson = &m_compileResult.jsonData["structs"];
        }

        const std::optional<std::string> selectedName =
            apc_interpreter::function_selection::chooseFunctionName(
                m_compileResult.debugInfo,
                m_compileResult.jsonData,
                m_options.functionName
            );

        const FunctionDebugInfo* selectedDebugFunction = nullptr;
        const json* selectedFunctionJson = nullptr;

        if (selectedName && !selectedName->empty()) {
            m_functionName = *selectedName;
            selectedFunctionJson =
                apc_interpreter::function_selection::findFunctionJson(
                    m_compileResult.jsonData,
                    m_functionName
                );

            if (m_compileResult.debugInfo) {
                auto it =
                    m_compileResult.debugInfo->functions.find(m_functionName);
                if (it != m_compileResult.debugInfo->functions.end()) {
                    selectedDebugFunction = &it->second;
                }
            }

            if (!selectedDebugFunction) {
                errorMessage =
                    "function '" + m_functionName +
                    "' has no debug range and cannot be live debugged";
                return false;
            }

            m_startPC = selectedDebugFunction->startPC;
            m_endPC = selectedDebugFunction->endPC;
            if (m_endPC == 0 ||
                m_endPC > m_compileResult.bytecodeInstructions.size()) {
                m_endPC = m_compileResult.bytecodeInstructions.size();
            }

            try {
                m_vm->setExecutionRange(m_startPC, m_endPC);
            } catch (const std::exception& e) {
                errorMessage =
                    "failed to set function execution range: " +
                    std::string(e.what());
                return false;
            }
        } else {
            m_startPC = 0;
            m_endPC = m_compileResult.bytecodeInstructions.size();
            m_warnings.push_back(
                "no function selected and no public function found; debugging full bytecode"
            );
        }

        std::vector<std::pair<std::string, std::string>> expectedParams =
            apc_interpreter::parameter_schema::expandFunctionParams(
                selectedFunctionJson,
                structsJson
            );
        if (expectedParams.empty() && selectedDebugFunction) {
            expectedParams =
                apc_interpreter::function_selection::debugInfoParams(
                    *selectedDebugFunction
                );
        }

        if (m_options.args.size() > expectedParams.size()) {
            errorMessage =
                "too many arguments: expected " +
                std::to_string(expectedParams.size()) + ", got " +
                std::to_string(m_options.args.size());
            return false;
        }

        if (!expectedParams.empty()) {
            StackState mainStack;
            StackState altStack;
            for (size_t index = 0; index < expectedParams.size(); ++index) {
                const auto& [name, type] = expectedParams[index];
                if (index >= m_options.args.size()) {
                    m_warnings.push_back(
                        "argument '" + name + "' missing; using 0x00"
                    );
                    mainStack.push(defaultStackArgumentValue(type));
                } else {
                    const StackArgumentParseResult parsed =
                        parseStackArgumentValueDetailed(
                            m_options.args[index],
                            type
                        );
                    if (parsed.status != StackArgumentStatus::Parsed &&
                        parsed.status != StackArgumentStatus::DefaultEmpty) {
                        errorMessage =
                            "invalid argument '" + name + "' for type '" +
                            type + "': " +
                            stackArgumentStatusMessage(parsed.status);
                        return false;
                    }
                    mainStack.push(parsed.value);
                }
            }
            m_vm->setInitialStacks(mainStack, altStack);
        } else if (!m_options.args.empty()) {
            errorMessage =
                "arguments were provided, but selected function has no parameters";
            return false;
        }

        if (!m_options.txFile.empty()) {
            TransactionData txData;
            TransactionDataLoadOptions txOptions;
            txOptions.chineseMessages = false;
            if (!loadTransactionDataFromFile(
                    m_options.txFile,
                    txData,
                    m_warnings,
                    errorMessage,
                    txOptions
                )) {
                return false;
            }
            m_vm->setTransactionData(txData);
        }

        return true;
    }

    bool canExecute() const
    {
        const LiveSessionState state =
            m_sessionState.load(std::memory_order_acquire);
        return m_vm &&
               (state == LiveSessionState::Ready ||
                state == LiveSessionState::Paused);
    }

    bool isRunning() const
    {
        return m_sessionState.load(std::memory_order_acquire) ==
               LiveSessionState::Running;
    }

    void joinExecutionThread()
    {
        if (m_executionThread.joinable()) {
            m_executionThread.join();
        }
    }

    void waitForPauseResponse()
    {
        std::unique_lock<std::mutex> lock(m_pauseAckMutex);
        m_pauseAckCondition.wait(lock, [this]() {
            return !m_pauseResponsePending ||
                   m_disconnectRequested.load(std::memory_order_acquire);
        });
    }

    void syncSessionStateFromVm()
    {
        LiveSessionState state = LiveSessionState::Error;
        switch (m_vm->getState()) {
            case VMState::READY:
                state = LiveSessionState::Ready;
                break;
            case VMState::RUNNING:
            case VMState::STEP_MODE:
                state = LiveSessionState::Running;
                break;
            case VMState::PAUSED:
                state = LiveSessionState::Paused;
                break;
            case VMState::FINISHED:
                state = LiveSessionState::Finished;
                break;
            case VMState::ERROR:
                state = LiveSessionState::Error;
                break;
        }
        m_sessionState.store(state, std::memory_order_release);
    }

    json runningRequestError(const json& request) const
    {
        return makeResponse(
            request,
            false,
            json::object(),
            "program is running; pause it before inspecting or reconfiguring"
        );
    }

    bool claimTerminatedEvent()
    {
        bool expected = false;
        return m_terminatedEventSent.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel
        );
    }

    json handleSetBreakpoints(const json& request)
    {
        const std::string sourcePath =
            request.contains("source") && request["source"].is_object()
                ? request["source"].value("path", m_options.sourceFile)
                : request.value("sourcePath", m_options.sourceFile);
        const std::string sourceKey = sourcePathKey(
            sourcePath,
            m_options.sourceFile
        );
        auto previous = m_sourceBreakpointIds.find(sourceKey);
        if (previous != m_sourceBreakpointIds.end()) {
            for (size_t id : previous->second) {
                m_breakpoints->removeBreakpoint(id);
            }
            m_sourceBreakpointIds.erase(previous);
        }

        json responseBreakpoints = json::array();
        std::vector<size_t> sourceBreakpointIds;
        const json requested =
            request.contains("breakpoints") && request["breakpoints"].is_array()
                ? request["breakpoints"]
                : json::array();

        for (const auto& item : requested) {
            const size_t line =
                item.is_object() ? item.value("line", 0) : item.get<size_t>();
            const std::string condition =
                item.is_object() ? item.value("condition", "") : "";
            const size_t id = condition.empty()
                                  ? m_breakpoints->addLineBreakpoint(
                                        sourcePath, line
                                    )
                                  : m_breakpoints->addConditionalBreakpoint(
                                        sourcePath, line, condition
                                    );

            json bp;
            bp["id"] = id;
            bp["line"] = line;
            bp["verified"] = id != 0;
            if (id == 0) {
                bp["message"] = "no executable AtomicProof instruction is mapped to this line";
            } else {
                sourceBreakpointIds.push_back(id);
                bp["pcs"] = m_compileResult.debugInfo
                                ? m_compileResult.debugInfo->getPCsForSourceLine(
                                      sourcePath,
                                      line
                                  )
                                : std::vector<size_t>{};
            }
            responseBreakpoints.push_back(std::move(bp));
        }
        if (!sourceBreakpointIds.empty()) {
            m_sourceBreakpointIds.emplace(
                sourceKey,
                std::move(sourceBreakpointIds)
            );
        }

        return makeResponse(
            request,
            true,
            {{"breakpoints", responseBreakpoints}, {"snapshot", snapshot()}}
        );
    }

    json snapshot() const
    {
        json body;
        if (!m_vm) {
            body["state"] = "uninitialized";
            return body;
        }

        const SourceLocation location = m_vm->getCurrentLocation();
        const std::string instruction = m_vm->getCurrentInstruction();
        const auto [opcode, operand] = parseInstructionText(instruction);
        const FunctionDebugInfo* function = m_compileResult.debugInfo
                                                ? m_compileResult.debugInfo
                                                      ->getFunctionAtPC(m_vm->getPC())
                                                : nullptr;
        const json lineInstructions = currentLineInstructions(
            location,
            function,
            m_vm->getPC()
        );

        body["pc"] = m_vm->getPC();
        body["state"] = vmStateToString(m_vm->getState());
        body["instruction"] = instruction;
        body["opcode"] = opcode;
        body["operand"] = operand;
        body["lineInstructions"] = lineInstructions;
        body["lineInstructionSummary"] =
            summarizeLineInstructions(lineInstructions);
        body["functionName"] = function ? function->name : m_functionName;
        body["instructionCount"] = m_vm->getInstructionCount();
        body["totalInstructions"] = m_compileResult.bytecodeInstructions.size();
        body["range"] = {{"startPC", m_startPC}, {"endPC", m_endPC}};
        body["source"] = {
            {"file", location.filename.empty() ? m_options.sourceFile
                                                : location.filename},
            {"line", location.line},
            {"column", location.column},
            {"endLine", location.endLine},
            {"endColumn", location.endColumn}
        };
        body["mainStack"] = stackToJson(m_vm->getMainStack());
        body["altStack"] = stackToJson(m_vm->getAltStack());
        body["callStack"] = callStackToJson(m_vm->getCallStack());
        body["warnings"] = m_warnings;
        if (!m_lastEventMessage.empty()) {
            body["lastEventMessage"] = m_lastEventMessage;
        }
        if (!m_vm->getLastError().empty()) {
            body["error"] = m_vm->getLastError();
        }
        return body;
    }

    json snapshotForContext(const FrameInspectionContext& context) const
    {
        if (context.current) {
            return snapshot();
        }

        json body = snapshot();
        const SourceLocation location = m_compileResult.debugInfo
                                            ? m_compileResult.debugInfo
                                                  ->getSourceLocation(context.pc)
                                            : SourceLocation();
        const std::string instruction =
            context.pc < m_compileResult.bytecodeInstructions.size()
                ? m_compileResult.bytecodeInstructions[context.pc]
                : "";
        const auto [opcode, operand] = parseInstructionText(instruction);
        const FunctionDebugInfo* function = m_compileResult.debugInfo
                                                ? m_compileResult.debugInfo
                                                      ->getFunctionAtPC(context.pc)
                                                : nullptr;
        const json lineInstructions = currentLineInstructions(
            location,
            function,
            context.pc
        );

        body["pc"] = context.pc;
        body["instruction"] = instruction;
        body["opcode"] = opcode;
        body["operand"] = operand;
        body["lineInstructions"] = lineInstructions;
        body["lineInstructionSummary"] =
            summarizeLineInstructions(lineInstructions);
        body["functionName"] = function ? function->name : "";
        body["source"] = {
            {"file", location.filename.empty() ? m_options.sourceFile
                                                : location.filename},
            {"line", location.line},
            {"column", location.column},
            {"endLine", location.endLine},
            {"endColumn", location.endColumn}
        };
        body["mainStack"] = stackToJson(context.mainStack);
        body["altStack"] = stackToJson(context.altStack);
        return body;
    }

    json variablesForScope(const json& request) const
    {
        const std::string scope =
            request.value("scope", request.value("name", "instruction"));

        if (scope == "locals" || scope == "globals") {
            const auto context = inspectionContext(
                request.value("frameId", static_cast<uint64_t>(0))
            );
            if (!context) {
                throw std::runtime_error("debug stack frame is no longer available");
            }
            auto inspector = m_vm->getVariableInspector();
            if (!inspector) {
                return json::array();
            }
            return variableValuesToJson(
                scope == "locals"
                    ? inspector->getLocalVariables(
                          context->mainStack,
                          context->pc
                      )
                    : inspector->getGlobalVariables(
                          context->mainStack,
                          context->pc
                      )
            );
        }

        if (scope == "instruction") {
            const auto context = inspectionContext(
                request.value("frameId", static_cast<uint64_t>(0))
            );
            if (!context) {
                throw std::runtime_error("debug stack frame is no longer available");
            }
            const json snap = snapshotForContext(*context);
            const std::string opcodeSummary =
                snap.value("lineInstructionSummary", "");
            const std::string currentOpcode = snap.value("opcode", "");
            return json::array({
                makeVariable("pc", std::to_string(snap.value("pc", 0)), "number"),
                makeVariable("state", snap.value("state", ""), "string"),
                makeVariable(
                    "instructions",
                    opcodeSummary,
                    "line-instructions"
                ),
                makeVariable(
                    "opcode",
                    opcodeSummary.empty() ? currentOpcode : opcodeSummary,
                    "line-instructions"
                ),
                makeVariable("currentOpcode", currentOpcode, "string"),
                makeVariable("operand", snap.value("operand", ""), "string"),
                makeVariable(
                    "function",
                    snap.value("functionName", ""),
                    "string"
                ),
                makeVariable(
                    "source",
                    snap["source"].value("file", "") + ":" +
                        std::to_string(snap["source"].value("line", 0)),
                    "source"
                )
            });
        }

        if (scope == "mainStack" || scope == "main" ||
            scope == "altStack" || scope == "alt") {
            const auto context = inspectionContext(
                request.value("frameId", static_cast<uint64_t>(0))
            );
            if (!context) {
                throw std::runtime_error("debug stack frame is no longer available");
            }
            const StackState& stack =
                (scope == "altStack" || scope == "alt") ? context->altStack
                                                         : context->mainStack;
            const auto values = stack.getAll();
            json result = json::array();
            for (size_t depth = 0; depth < values.size(); ++depth) {
                const size_t index = values.size() - depth - 1;
                const json item = stackElementToJson(
                    values[index],
                    index,
                    values.size()
                );
                std::string summary = item.value("hex", "");
                if (item.contains("intString")) {
                    summary += " int=" + item.value("intString", "");
                }
                result.push_back(makeVariable(
                    depth == 0 ? "[0] top" : "[" + std::to_string(depth) + "]",
                    summary,
                    "stack-item"
                ));
            }
            return result;
        }

        if (scope == "callStack") {
            json result = json::array();
            const auto frames = m_vm->getCallStack();
            for (size_t index = 0; index < frames.size(); ++index) {
                result.push_back(makeVariable(
                    "[" + std::to_string(index) + "]",
                    frames[index].functionName + " returnPC=" +
                        std::to_string(frames[index].returnPC),
                    "frame"
                ));
            }
            return result;
        }

        if (scope == "warnings") {
            json result = json::array();
            for (size_t index = 0; index < m_warnings.size(); ++index) {
                result.push_back(makeVariable(
                    "[" + std::to_string(index) + "]",
                    m_warnings[index],
                    "warning"
                ));
            }
            return result;
        }

        return json::array();
    }

    json currentLineInstructions(
        const SourceLocation& location,
        const FunctionDebugInfo* function,
        size_t currentPC
    ) const
    {
        json result = json::array();
        if (!m_compileResult.debugInfo || location.line == 0) {
            return result;
        }

        const auto pcs = m_compileResult.debugInfo->getPCsForSourceLine(
            location.filename.empty() ? m_options.sourceFile : location.filename,
            location.line
        );
        for (size_t pc : pcs) {
            if (pc >= m_compileResult.bytecodeInstructions.size()) {
                continue;
            }
            if (function && (pc < function->startPC || pc >= function->endPC)) {
                continue;
            }
            if (!function && m_endPC > m_startPC &&
                (pc < m_startPC || pc >= m_endPC)) {
                continue;
            }

            const std::string instruction =
                m_compileResult.bytecodeInstructions[pc];
            const auto [opcode, operand] = parseInstructionText(instruction);
            result.push_back({
                {"pc", pc},
                {"instruction", instruction},
                {"opcode", opcode},
                {"operand", operand},
                {"current", pc == currentPC},
            });
        }

        return result;
    }

    json evaluate(
        const std::string& expression,
        uint64_t frameId = 0
    ) const
    {
        const std::string expr = trim(expression);
        json result;
        result["success"] = true;
        result["type"] = "string";
        result["value"] = "";
        result["result"] = "";

        if (expr.empty() || !m_vm) {
            return result;
        }

        const auto context = inspectionContext(frameId);
        if (!context) {
            result["success"] = false;
            result["message"] = "debug stack frame is no longer available";
            return result;
        }

        const json snap = snapshotForContext(*context);
        if (expr == "pc") {
            result["type"] = "number";
            result["value"] = snap.value("pc", 0);
            result["result"] = std::to_string(snap.value("pc", 0));
            return result;
        }
        if (expr == "opcode" || expr == "instruction" || expr == "operand" ||
            expr == "functionName" || expr == "state") {
            result["value"] = snap.value(expr, "");
            result["result"] = snap.value(expr, "");
            return result;
        }
        if (expr == "instructions" || expr == "lineInstructions") {
            result["type"] = expr == "lineInstructions" ? "object" : "string";
            result["value"] = expr == "lineInstructions"
                                  ? snap.value("lineInstructions", json::array())
                                  : json(snap.value("lineInstructionSummary", ""));
            result["result"] = expr == "lineInstructions"
                                   ? snap["lineInstructions"].dump()
                                   : snap.value("lineInstructionSummary", "");
            return result;
        }
        if (expr == "function") {
            result["value"] = snap.value("functionName", "");
            result["result"] = snap.value("functionName", "");
            return result;
        }
        if (expr == "line") {
            result["type"] = "number";
            result["value"] = snap["source"].value("line", 0);
            result["result"] = std::to_string(snap["source"].value("line", 0));
            return result;
        }
        if (expr == "main.length" || expr == "alt.length") {
            const bool main = expr.rfind("main", 0) == 0;
            const size_t size =
                main ? context->mainStack.size() : context->altStack.size();
            result["type"] = "number";
            result["value"] = size;
            result["result"] = std::to_string(size);
            return result;
        }
        if (expr == "json") {
            result["type"] = "object";
            result["value"] = snap;
            result["result"] = snap.dump();
            return result;
        }

        const std::string mainPrefix = "main[";
        const std::string altPrefix = "alt[";
        if (expr.rfind(mainPrefix, 0) == 0 || expr.rfind(altPrefix, 0) == 0) {
            const bool useMain = expr.rfind(mainPrefix, 0) == 0;
            const std::string prefix = useMain ? mainPrefix : altPrefix;
            const size_t close = expr.find(']', prefix.size());
            if (close != std::string::npos) {
                const std::string depthText =
                    expr.substr(prefix.size(), close - prefix.size());
                try {
                    const size_t depth = std::stoul(depthText);
                    const StackState& stack =
                        useMain ? context->mainStack : context->altStack;
                    const auto values = stack.getAll();
                    if (depth < values.size()) {
                        const size_t index = values.size() - depth - 1;
                        const json item =
                            stackElementToJson(values[index], index, values.size());
                        const std::string suffix = expr.substr(close + 1);
                        if (suffix == ".hex" || suffix.empty()) {
                            result["value"] = item.value("hex", "");
                            result["result"] = item.value("hex", "");
                        } else if (suffix == ".int" ||
                                   suffix == ".intString") {
                            result["type"] = "number";
                            result["value"] = item.value("intString", "");
                            result["result"] = item.value("intString", "");
                        } else if (suffix == ".depth") {
                            result["type"] = "number";
                            result["value"] = item.value("depth", 0);
                            result["result"] =
                                std::to_string(item.value("depth", 0));
                        } else {
                            result["type"] = "object";
                            result["value"] = item;
                            result["result"] = item.dump();
                        }
                    }
                } catch (...) {
                    return result;
                }
            }
            return result;
        }

        auto evaluator = m_vm->getExpressionEvaluator();
        if (!evaluator) {
            result["success"] = false;
            result["message"] = "debug stack frame is no longer available";
            return result;
        }
        const EvaluationResult evaluated = evaluator->evaluate(
            expr,
            context->mainStack,
            context->pc
        );
        if (!evaluated.success) {
            result["success"] = false;
            result["message"] = evaluated.errorMessage;
            return result;
        }
        result["type"] = evaluated.type;
        result["value"] = evaluated.value;
        result["result"] = evaluated.value;
        return result;
    }

    json stackFrames() const
    {
        json frames = json::array();
        const json snap = snapshot();
        if (!m_vm) {
            return frames;
        }

        const auto& callStack = m_vm->getCallStack();
        for (size_t offset = 0; offset < callStack.size(); ++offset) {
            const size_t index = callStack.size() - offset - 1;
            const auto& callFrame = callStack[index];
            const bool current = index == callStack.size() - 1;
            const SourceLocation& location =
                callFrame.suspendedLocation.isValid()
                    ? callFrame.suspendedLocation
                    : callFrame.callLocation;

            json frame;
            frame["id"] = callFrame.frameId;
            frame["name"] = callFrame.functionName;
            frame["pc"] = current ? snap.value("pc", 0)
                                  : callFrame.suspendedPC;
            frame["source"] = current
                                  ? snap["source"]
                                  : json{
                                        {"file", location.filename},
                                        {"line", location.line},
                                        {"column", location.column},
                                    };
            frame["current"] = current;
            frames.push_back(std::move(frame));
        }
        return frames;
    }

    std::string eventReason(const std::string& fallback) const
    {
        if (m_vm && m_vm->getState() == VMState::ERROR) {
            return "exception";
        }
        if (m_lastEvent == VMEvent::BREAKPOINT_HIT) {
            return "breakpoint";
        }
        if (m_lastEvent == VMEvent::PAUSED) {
            return "pause";
        }
        return fallback;
    }

    std::optional<json> stopOrTerminateEvent(const std::string& fallback)
    {
        if (m_vm && m_vm->getState() == VMState::FINISHED) {
            if (!claimTerminatedEvent()) {
                return std::nullopt;
            }
            return makeEvent("terminated", {{"snapshot", snapshot()}});
        }
        return makeStoppedEvent(eventReason(fallback));
    }

    json makeStoppedEvent(const std::string& reason) const
    {
        json body = {
            {"reason", reason},
            {"threadId", 1},
            {"snapshot", snapshot()},
        };
        if (reason == "exception" && m_vm && !m_vm->getLastError().empty()) {
            body["description"] = m_vm->getLastError();
            body["text"] = m_vm->getLastError();
        }
        return makeEvent("stopped", body);
    }

    json makeResponse(
        const json& request,
        bool success,
        const json& body,
        const std::string& message = ""
    ) const
    {
        json response;
        response["type"] = "response";
        response["request_seq"] = request.value("seq", 0);
        response["command"] = request.value("command", "");
        response["success"] = success;
        response["body"] = body;
        if (!message.empty()) {
            response["message"] = message;
        }
        return response;
    }

    json makeEvent(const std::string& event, const json& body) const
    {
        return json{{"type", "event"}, {"event", event}, {"body", body}};
    }

    LiveDebugServerOptions m_options;
    DebuggerCore::CompileResult m_compileResult;
    std::shared_ptr<BVMSimulator> m_vm;
    std::shared_ptr<BreakpointManager> m_breakpoints;
    std::map<std::string, std::vector<size_t>> m_sourceBreakpointIds;
    std::vector<std::string> m_warnings;
    std::string m_functionName;
    size_t m_startPC = 0;
    size_t m_endPC = 0;
    VMEvent m_lastEvent = VMEvent::STARTED;
    std::string m_lastEventMessage;
    EventSink m_eventSink;
    std::thread m_executionThread;
    std::atomic<LiveSessionState> m_sessionState{LiveSessionState::Ready};
    std::atomic<bool> m_pendingContinue{false};
    std::atomic<bool> m_disconnectRequested{false};
    std::atomic<bool> m_terminatedEventSent{false};
    std::mutex m_pauseAckMutex;
    std::condition_variable m_pauseAckCondition;
    bool m_pauseResponsePending = false;
};

void writeJsonLine(std::ostream& output, const json& message)
{
    output << message.dump() << '\n';
    output.flush();
}

json makeServerEvent(const std::string& event, const json& body)
{
    return json{{"type", "event"}, {"event", event}, {"body", body}};
}

} // namespace

int runLiveDebugServer(
    std::istream& input,
    std::ostream& output,
    std::ostream& error,
    const LiveDebugServerOptions& options
)
{
    std::mutex outputMutex;
    const auto emit = [&output, &outputMutex](const json& message) {
        std::lock_guard<std::mutex> lock(outputMutex);
        writeJsonLine(output, message);
    };

    LiveDebugSession session(options, emit);
    std::string initError;
    if (!session.initialize(initError)) {
        emit(makeServerEvent("error", {{"message", initError}}));
        emit(makeServerEvent("terminated", json::object()));
        return 1;
    }

    emit(session.readyEvent());

    std::string line;
    bool shouldExit = false;
    while (!shouldExit && std::getline(input, line)) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        json request;
        try {
            request = json::parse(line);
        } catch (const std::exception& e) {
            error << "live debug protocol parse error: " << e.what() << '\n';
            emit(makeServerEvent(
                "error",
                {{"message", std::string("invalid JSON request: ") + e.what()}}
            ));
            continue;
        }

        if (!request.is_object()) {
            emit(makeServerEvent(
                "error",
                {{"message", "request must be an object"}}
            ));
            continue;
        }

        std::vector<json> events;
        const json response = session.handleRequest(request, events, shouldExit);
        emit(response);
        for (const auto& event : events) {
            emit(event);
        }
        session.acknowledgeRequest(request);
        session.startPendingExecution();
    }

    return 0;
}

} // namespace apc_debug

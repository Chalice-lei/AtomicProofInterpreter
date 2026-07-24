#include "bytecode_runner.h"
#include "function_selection.h"
#include "parameter_schema.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "../debugger/core/debugger_core.h"
#include "../debugger/vm/bvm_simulator.h"
#include "../debugger/vm/stack_argument.h"
#include "../debugger/vm/stack_state.h"
#include "../debugger/vm/transaction_data_loader.h"
#include "../error/error_manager.h"

namespace apc_interpreter
{
namespace
{
using apc_debug::BVMSimulator;
using apc_debug::FunctionDebugInfo;
using apc_debug::StackState;
using apc_debug::TransactionData;
using apc_debug::VMState;
using nlohmann::json;

std::vector<StackValueView> snapshotStack(const StackState& stack)
{
    std::vector<StackValueView> values;
    for (const auto& value : stack.getAll()) {
        StackValueView view;
        view.hex = value.toHexString(true);
        if (auto intValue = value.toInt()) {
            view.intValue = std::to_string(*intValue);
        }
        values.push_back(std::move(view));
    }
    return values;
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

bool containsSignatureCheckInstruction(
    const std::vector<std::string>& instructions,
    std::size_t start,
    std::size_t end
)
{
    end = std::min(end, instructions.size());
    if (start > end) {
        return false;
    }

    for (std::size_t i = start; i < end; ++i) {
        const std::string& instruction = instructions[i];
        if (instruction.find("OP_CHECKSIG") != std::string::npos ||
            instruction.find("OP_CHECKSIGVERIFY") != std::string::npos ||
            instruction.find("OP_CHECKMULTISIG") != std::string::npos ||
            instruction.find("OP_CHECKMULTISIGVERIFY") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> splitFunctionNames(const std::string& requested)
{
    std::vector<std::string> names;
    size_t start = 0;
    while (start <= requested.size()) {
        const size_t comma = requested.find(',', start);
        const size_t end =
            comma == std::string::npos ? requested.size() : comma;
        std::string name = requested.substr(start, end - start);
        name.erase(
            name.begin(),
            std::find_if(name.begin(), name.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            })
        );
        name.erase(
            std::find_if(name.rbegin(), name.rend(), [](unsigned char ch) {
                return !std::isspace(ch);
            }).base(),
            name.end()
        );
        if (!name.empty()) {
            names.push_back(std::move(name));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return names;
}

} // namespace

BytecodeRunResult BytecodeRunner::runSource(
    const std::string& sourceFile,
    const std::string& sourceCode,
    const BytecodeRunOptions& options
) const
{
    BytecodeRunResult result;
    result.status = "not_started";

    ErrorManager::getInstance().clear();
    ErrorManager::getInstance().setSourceContent(sourceFile, sourceCode);
    ErrorManager::getInstance().setColorOutput(false);
    ErrorManager::getInstance().setShowContext(false);

    auto compileResult = apc_debug::DebuggerCore::compileSource(
        sourceFile, sourceCode, options.allowSubscopeAltstack
    );

    result.compileSuccess = compileResult.success;
    result.hexBytecode = compileResult.hexBytecode;
    result.bytecodeInstructions = compileResult.bytecodeInstructions;
    result.totalInstructions = compileResult.bytecodeInstructions.size();

    if (!compileResult.success) {
        result.status = "compile_error";
        result.errorMessage = compileResult.errorMessage;
        return result;
    }

    auto vm = std::make_shared<BVMSimulator>(
        compileResult.bytecodeInstructions, compileResult.debugInfo
    );
    bool emptyExecutionRange = false;
    if (!options.stackTraceOutputFile.empty()) {
        vm->setStackTraceEnabled(true);
    }

    const json* structsJson = nullptr;
    if (compileResult.jsonData.contains("structs") &&
        compileResult.jsonData["structs"].is_array()) {
        structsJson = &compileResult.jsonData["structs"];
    }

    std::vector<std::string> selectedNames =
        splitFunctionNames(options.functionName);
    if (selectedNames.empty()) {
        const std::optional<std::string> selectedName =
            function_selection::chooseFunctionName(
                compileResult.debugInfo,
                compileResult.jsonData,
                ""
            );
        if (selectedName && !selectedName->empty()) {
            selectedNames.push_back(*selectedName);
        }
    }

    std::vector<const FunctionDebugInfo*> selectedDebugFunctions;
    std::vector<const json*> selectedFunctionJsons;

    if (!selectedNames.empty()) {
        for (size_t i = 0; i < selectedNames.size(); ++i) {
            const std::string& name = selectedNames[i];
            const FunctionDebugInfo* debugFunction = nullptr;
            if (compileResult.debugInfo) {
                auto it = compileResult.debugInfo->functions.find(name);
                if (it != compileResult.debugInfo->functions.end()) {
                    debugFunction = &it->second;
                }
            }
            if (!debugFunction) {
                result.status = "function_not_found";
                result.errorMessage =
                    "未找到函数 '" + name + "' 的调试范围";
                return result;
            }
            if (i > 0 &&
                selectedDebugFunctions.back()->startPC >
                    debugFunction->startPC) {
                result.status = "invalid_phase_sequence";
                result.errorMessage =
                    "阶段函数必须按源码/字节码顺序执行: '" +
                    selectedNames[i - 1] + "' 不能位于 '" + name +
                    "' 之后";
                return result;
            }
            selectedDebugFunctions.push_back(debugFunction);
            selectedFunctionJsons.push_back(
                function_selection::findFunctionJson(
                    compileResult.jsonData, name
                )
            );
            if (!result.functionName.empty()) {
                result.functionName += ",";
            }
            result.functionName += name;
        }

        if (selectedDebugFunctions.size() > 1) {
            std::vector<std::string> sourcePublicNames;
            if (compileResult.jsonData.contains("functions") &&
                compileResult.jsonData["functions"].is_array()) {
                for (const auto& function :
                     compileResult.jsonData["functions"]) {
                    if (function.is_object() &&
                        function.value("type", "") == "public") {
                        sourcePublicNames.push_back(
                            function.value("name", "")
                        );
                    }
                }
            }

            auto first = std::find(
                sourcePublicNames.begin(),
                sourcePublicNames.end(),
                selectedNames.front()
            );
            bool isContiguous = first != sourcePublicNames.end() &&
                                static_cast<size_t>(
                                    std::distance(first, sourcePublicNames.end())
                                ) >= selectedNames.size();
            for (size_t i = 0; isContiguous && i < selectedNames.size(); ++i) {
                isContiguous = *(first + static_cast<ptrdiff_t>(i)) ==
                               selectedNames[i];
            }
            if (!isContiguous) {
                result.status = "invalid_phase_sequence";
                result.errorMessage =
                    "阶段链必须按源码顺序列出所选范围内的每个 public "
                    "函数，不能跳过中间阶段";
                return result;
            }
        }

        result.startPC = selectedDebugFunctions.front()->startPC;
        result.endPC = selectedDebugFunctions.back()->endPC;
        if (result.endPC > compileResult.bytecodeInstructions.size()) {
            result.endPC = compileResult.bytecodeInstructions.size();
        }

        emptyExecutionRange = result.startPC == result.endPC;
        if (!emptyExecutionRange) {
            try {
                vm->setExecutionRange(
                    result.startPC,
                    result.endPC,
                    selectedNames.front()
                );
            } catch (const std::exception& e) {
                result.status = "invalid_function_range";
                result.errorMessage =
                    "设置函数执行范围失败: " + std::string(e.what());
                return result;
            }
        }
    } else {
        result.warnings.push_back(
            "未指定 --function 且未找到 public 函数，将执行完整字节码"
        );
        result.endPC = compileResult.bytecodeInstructions.size();
    }

    if (options.checkSigCallback) {
        vm->setCheckSigCallback(options.checkSigCallback);
    } else if (containsSignatureCheckInstruction(
                   compileResult.bytecodeInstructions,
                   result.startPC,
                   result.endPC == 0 ? compileResult.bytecodeInstructions.size()
                                      : result.endPC
               )) {
        vm->setCheckSigCallback(
            [](
                const std::vector<uint8_t>&,
                const std::vector<uint8_t>&,
                const std::vector<uint8_t>&
            ) -> bool {
                throw std::runtime_error(
                    "signature verification callback is not configured"
                );
            }
        );
        result.warnings.push_back(
            "字节码包含 CHECKSIG/CHECKMULTISIG，但未配置真实签名验证回调；"
            "非交互式 runner 将拒绝默认通过"
        );
    }

    std::vector<std::vector<std::pair<std::string, std::string>>>
        expectedParamGroups;
    std::vector<size_t> paramGroupOffsets;
    size_t expectedParamCount = 0;
    for (size_t i = 0; i < selectedDebugFunctions.size(); ++i) {
        auto params = parameter_schema::expandFunctionParams(
            selectedFunctionJsons[i], structsJson
        );
        if (params.empty()) {
            params = function_selection::debugInfoParams(
                *selectedDebugFunctions[i]
            );
        }
        paramGroupOffsets.push_back(expectedParamCount);
        expectedParamCount += params.size();
        expectedParamGroups.push_back(std::move(params));
    }

    if (expectedParamCount != 0 || !options.args.empty()) {
        if (options.args.size() > expectedParamCount) {
            result.status = "argument_error";
            result.errorMessage =
                "参数数量过多: 期望 " +
                std::to_string(expectedParamCount) + " 个，收到 " +
                std::to_string(options.args.size()) + " 个";
            return result;
        }

        StackState mainStack;
        StackState altStack;
        // BVM 最先执行的阶段参数必须位于栈顶，因此按阶段逆序压入；
        // 每个函数内部仍保持声明顺序（最后一个形参位于栈顶）。
        for (size_t group = expectedParamGroups.size(); group-- > 0;) {
            const auto& params = expectedParamGroups[group];
            for (size_t i = 0; i < params.size(); ++i) {
                const auto& [paramName, paramType] = params[i];
                const size_t argIndex = paramGroupOffsets[group] + i;
                if (argIndex >= options.args.size()) {
                    result.warnings.push_back(
                        "参数 '" + selectedNames[group] + "." + paramName +
                        "' 未提供，使用默认值 0x00"
                    );
                    mainStack.push(
                        apc_debug::defaultStackArgumentValue(paramType)
                    );
                } else {
                    mainStack.push(apc_debug::parseStackArgumentValue(
                        options.args[argIndex], paramType
                    ));
                }
            }
        }
        vm->setInitialStacks(mainStack, altStack);
    }

    if (!options.txFile.empty()) {
        TransactionData txData;
        std::string txError;
        if (!apc_debug::loadTransactionDataFromFile(
                options.txFile, txData, result.warnings, txError
            )) {
            result.status = "transaction_error";
            result.errorMessage = txError;
            return result;
        }
        vm->setTransactionData(txData);
    }

    if (emptyExecutionRange) {
        result.status = "finished";
        result.pc = result.endPC;
        result.mainStack = snapshotStack(vm->getMainStack());
        result.altStack = snapshotStack(vm->getAltStack());
        result.success = true;
        if (!options.stackTraceOutputFile.empty()) {
            result.stackTraceOutputFile = options.stackTraceOutputFile;
            result.stackTraceWritten =
                vm->saveStackTrace(options.stackTraceOutputFile, sourceCode);
            if (!result.stackTraceWritten) {
                result.status = "trace_write_error";
                result.errorMessage = "无法写入空函数的栈 trace 文件";
                result.success = false;
            }
        }
        return result;
    }

    vm->run();

    result.status = vmStateToString(vm->getState());
    result.pc = vm->getPC();

    const auto stats = vm->getStatistics();
    result.executedInstructions = stats.executedInstructions;
    result.maxStackSize = stats.maxStackSize;
    result.maxCallDepth = stats.maxCallDepth;
    result.mainStack = snapshotStack(vm->getMainStack());
    result.altStack = snapshotStack(vm->getAltStack());
    result.errorMessage = vm->getLastError();
    if (vm->getState() == VMState::ERROR && selectedNames.size() == 1 &&
        result.errorMessage.find("Stack underflow") != std::string::npos &&
        !selectedDebugFunctions.empty() &&
        selectedDebugFunctions.front()->startPC != 0) {
        result.status = "phase_state_required";
        result.errorMessage =
            "所选函数缺少前置阶段留下的栈状态；请按顺序执行阶段链（例如 "
            "run <file> stage_one,stage_two ...）。原始错误: " +
            result.errorMessage;
    }

    if (!options.stackTraceOutputFile.empty()) {
        result.stackTraceOutputFile = options.stackTraceOutputFile;
        result.stackTraceWritten =
            vm->saveStackTrace(options.stackTraceOutputFile, sourceCode);
        if (!result.stackTraceWritten) {
            result.status = "trace_write_error";
            result.errorMessage =
                "无法写入栈 trace 文件: " + options.stackTraceOutputFile;
            result.success = false;
            return result;
        }
    }

    result.success = vm->getState() == VMState::FINISHED;

    if (!result.success && result.errorMessage.empty()) {
        result.errorMessage = "VM 未以 finished 状态结束";
    }

    return result;
}

} // namespace apc_interpreter

#include "bytecode_runner.h"
#include "function_selection.h"
#include "parameter_schema.h"

#include <algorithm>
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
    if (!options.stackTraceOutputFile.empty()) {
        vm->setStackTraceEnabled(true);
    }

    const json* structsJson = nullptr;
    if (compileResult.jsonData.contains("structs") &&
        compileResult.jsonData["structs"].is_array()) {
        structsJson = &compileResult.jsonData["structs"];
    }

    const std::optional<std::string> selectedName =
        function_selection::chooseFunctionName(
            compileResult.debugInfo,
            compileResult.jsonData,
            options.functionName
        );

    const FunctionDebugInfo* selectedDebugFunction = nullptr;
    const json* selectedFunctionJson = nullptr;

    if (selectedName && !selectedName->empty()) {
        result.functionName = *selectedName;
        selectedFunctionJson =
            function_selection::findFunctionJson(
                compileResult.jsonData,
                result.functionName
            );

        if (compileResult.debugInfo) {
            auto it = compileResult.debugInfo->functions.find(result.functionName);
            if (it != compileResult.debugInfo->functions.end()) {
                selectedDebugFunction = &it->second;
            }
        }

        if (!selectedDebugFunction) {
            result.status = "function_not_found";
            result.errorMessage =
                "未找到函数 '" + result.functionName + "' 的调试范围";
            return result;
        }

        result.startPC = selectedDebugFunction->startPC;
        result.endPC = selectedDebugFunction->endPC;
        if (result.endPC == 0 ||
            result.endPC > compileResult.bytecodeInstructions.size()) {
            result.endPC = compileResult.bytecodeInstructions.size();
        }

        try {
            vm->setExecutionRange(result.startPC, result.endPC);
        } catch (const std::exception& e) {
            result.status = "invalid_function_range";
            result.errorMessage =
                "设置函数执行范围失败: " + std::string(e.what());
            return result;
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

    std::vector<std::pair<std::string, std::string>> expectedParams =
        parameter_schema::expandFunctionParams(selectedFunctionJson, structsJson);
    if (expectedParams.empty() && selectedDebugFunction) {
        expectedParams =
            function_selection::debugInfoParams(*selectedDebugFunction);
    }

    if (!expectedParams.empty() || !options.args.empty()) {
        if (options.args.size() > expectedParams.size()) {
            result.status = "argument_error";
            result.errorMessage =
                "参数数量过多: 期望 " +
                std::to_string(expectedParams.size()) + " 个，收到 " +
                std::to_string(options.args.size()) + " 个";
            return result;
        }

        StackState mainStack;
        StackState altStack;
        for (size_t i = 0; i < expectedParams.size(); ++i) {
            const auto& [paramName, paramType] = expectedParams[i];
            if (i >= options.args.size()) {
                result.warnings.push_back(
                    "参数 '" + paramName + "' 未提供，使用默认值 0x00"
                );
                mainStack.push(apc_debug::defaultStackArgumentValue(paramType));
            } else {
                mainStack.push(apc_debug::parseStackArgumentValue(
                    options.args[i],
                    paramType
                ));
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

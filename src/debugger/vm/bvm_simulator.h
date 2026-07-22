#ifndef BVM_SIMULATOR_H
#define BVM_SIMULATOR_H

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../breakpoint/breakpoint_manager.h"
#include "../info/debug_info.h"
#include "stack_trace.h"
#include "stack_state.h"

namespace apc_debug
{

class VariableInspector;
class ScopeInspector;
class ExpressionEvaluator;

// settxfile 七键：version, locktime, inputCount, outputCount, inputsHash,
// unlockingInput, outputsHash
struct TransactionData
{
    uint32_t version;
    uint32_t lockTime;
    uint32_t inputCount;
    uint32_t outputCount;

    std::vector<uint8_t> inputsHash;
    std::vector<uint8_t> unlockingInput;
    std::vector<uint8_t> outputsHash;

    TransactionData()
        : version(1), lockTime(0),
          inputCount(1), // 兼容旧行为：默认至少 1 个输入/输出
          outputCount(1)
    {}
};

enum class VMState {
    READY,
    RUNNING,
    PAUSED,
    STEP_MODE,
    FINISHED,
    ERROR
};

enum class StepMode {
    NONE,
    STEP_IN,
    STEP_OVER,
    STEP_OUT
};

// 调用栈帧（参考 Python Frame Object）
struct CallFrame
{
    uint64_t frameId;
    std::string functionName;
    size_t returnPC;
    SourceLocation callLocation;
    size_t stackBase;
    size_t frameStart;

    size_t entryPC;
    size_t instructionCount;
    std::shared_ptr<ScopeDebugInfo> scope;
    size_t suspendedPC;
    SourceLocation suspendedLocation;
    std::vector<StackElement> suspendedMainStack;
    std::vector<StackElement> suspendedAltStack;

    CallFrame()
        : frameId(0), returnPC(0), stackBase(0), frameStart(0), entryPC(0),
          instructionCount(0), suspendedPC(0)
    {}

    CallFrame(
        const std::string& func,
        size_t ret,
        const SourceLocation& loc,
        size_t base
    )
        : frameId(0), functionName(func), returnPC(ret), callLocation(loc),
          stackBase(base), frameStart(base), entryPC(0), instructionCount(0),
          suspendedPC(0)
    {}

    CallFrame(
        const std::string& func,
        size_t ret,
        const SourceLocation& loc,
        size_t base,
        size_t entry
    )
        : frameId(0), functionName(func), returnPC(ret), callLocation(loc),
          stackBase(base), frameStart(base), entryPC(entry), instructionCount(0),
          suspendedPC(0)
    {}
};

enum class VMEvent {
    STARTED,
    PAUSED,
    RESUMED,
    STEPPED,
    BREAKPOINT_HIT,
    FINISHED,
    ERROR,
    INSTRUCTION_EXECUTED
};

using VMEventCallback =
    std::function<void(VMEvent event, const std::string& message)>;

class BVMSimulator
{
public:
    BVMSimulator(
        const std::vector<std::string>& bytecode,
        std::shared_ptr<DebugInfo> debugInfo
    );

    void start();
    void reset();

    // 配合 JSON 加载的函数参数等初始状态
    void setInitialStacks(const StackState& main, const StackState& alt);

    // 仅调试特定函数时使用
    void setExecutionRange(size_t startPC, size_t endPC);
    void clearExecutionRange();

    bool hasExecutionRange() const
    {
        return m_hasExecutionRange;
    }
    size_t getExecutionRangeStart() const
    {
        return m_executionRangeStart;
    }
    size_t getExecutionRangeEnd() const
    {
        return m_executionRangeEnd;
    }

    void setTransactionData(const TransactionData& txData);
    bool loadTransactionFromHex(const std::string& hexStr);
    bool loadTransactionFromFile(const std::string& filename);

    const TransactionData* getTransactionData() const
    {
        return m_transactionData ? &(*m_transactionData) : nullptr;
    }

    bool hasTransactionData() const
    {
        return m_transactionData.has_value();
    }

    void clearTransactionData()
    {
        m_transactionData.reset();
    }

    void run();
    void pause();
    void requestPause();
    void requestTerminate();
    void clearExecutionRequests();
    void resume();
    void stepIn();
    void stepOver();
    void stepOut();

    // 返回值：是否继续执行
    bool executeInstruction();

    VMState getState() const
    {
        return m_state;
    }
    size_t getPC() const
    {
        return m_pc;
    }
    SourceLocation getCurrentLocation() const;
    std::string getCurrentInstruction() const;
    size_t getInstructionCount() const
    {
        return m_instructionCount;
    }

    const std::vector<std::string>& getBytecode() const
    {
        return m_bytecode;
    }

    StackState& getMainStack()
    {
        return m_mainStack;
    }
    StackState& getAltStack()
    {
        return m_altStack;
    }
    const StackState& getMainStack() const
    {
        return m_mainStack;
    }
    const StackState& getAltStack() const
    {
        return m_altStack;
    }

    void setStackTraceEnabled(bool enable);
    bool isStackTraceEnabled() const
    {
        return m_stackTraceEnabled;
    }
    void clearStackTrace();
    const StackTraceRecorder& getStackTraceRecorder() const
    {
        return m_stackTrace;
    }
    bool saveStackTrace(
        const std::string& filename,
        const std::string& sourceCode = ""
    ) const;

    const std::vector<CallFrame>& getCallStack() const
    {
        return m_callStack;
    }
    size_t getCallDepth() const
    {
        return m_callStack.size();
    }
    const CallFrame* getCurrentFrame() const;

    // 简单 PC 断点；向后兼容，新代码用 BreakpointManager
    void addBreakpoint(size_t pc);
    void removeBreakpoint(size_t pc);
    void clearBreakpoints();
    bool hasBreakpoint(size_t pc) const;
    const std::vector<size_t>& getBreakpoints() const
    {
        return m_breakpoints;
    }

    void setBreakpointManager(std::shared_ptr<BreakpointManager> mgr)
    {
        m_breakpointMgr = mgr;
    }
    std::shared_ptr<BreakpointManager> getBreakpointManager() const
    {
        return m_breakpointMgr;
    }

    void setEventCallback(VMEventCallback callback)
    {
        m_eventCallback = callback;
    }

    // OP_CHECKSIG 验证回调；未设置时调试模式视为通过
    // 参数：签名（不含 hashtype）、公钥、ForkID sighash
    using CheckSigCallback = std::function<bool(
        const std::vector<uint8_t>& sig,
        const std::vector<uint8_t>& pubkey,
        const std::vector<uint8_t>& sighash
    )>;
    void setCheckSigCallback(CheckSigCallback callback)
    {
        m_checkSigCallback = std::move(callback);
    }

    std::shared_ptr<DebugInfo> getDebugInfo() const
    {
        return m_debugInfo;
    }

    struct Statistics
    {
        size_t totalInstructions;
        size_t executedInstructions;
        size_t maxStackSize;
        size_t maxCallDepth;
    };
    Statistics getStatistics() const;

    std::shared_ptr<VariableInspector> getVariableInspector() const
    {
        return m_varInspector;
    }

    std::shared_ptr<ScopeInspector> getScopeInspector() const
    {
        return m_scopeInspector;
    }

    std::shared_ptr<ExpressionEvaluator> getExpressionEvaluator() const
    {
        return m_exprEvaluator;
    }

    // 默认启用
    void enableVariableInspection(bool enable);
    bool isVariableInspectionEnabled() const
    {
        return m_varInspectionEnabled;
    }

    bool isCurrentBranchExecuting() const
    {
        return isCurrentlyExecuting();
    }

    std::string getLastError() const
    {
        return m_lastError;
    }
    bool hasError() const
    {
        return m_state == VMState::ERROR;
    }

private:
    void executeOpcode(const std::string& opcode, const std::string& operand);
    bool parseInstruction(
        const std::string& instruction,
        std::string& opcode,
        std::string& operand
    );

    // 栈操作
    void op_push(const std::string& operand);
    void op_dup();
    void op_drop();
    void op_2drop();
    void op_2dup();
    void op_3dup();
    void op_over();
    void op_2over();
    void op_rot();
    void op_2rot();
    void op_swap();
    void op_2swap();
    void op_ifdup();
    void op_depth();
    void op_nip();
    void op_tuck();
    void op_pick(const std::string& operand);
    void op_roll(const std::string& operand);
    void op_toaltstack();
    void op_fromaltstack();

    // 算术
    void op_add();
    void op_sub();
    void op_mul();
    void op_div();
    void op_neg();
    void op_abs();
    void op_inc();
    void op_dec();

    // 比较
    void op_equal();
    void op_numequal();
    void op_lessthan();
    void op_greaterthan();
    void op_lessthanorequal();
    void op_greaterthanorequal();
    void op_min();
    void op_max();
    void op_within();

    // 逻辑
    void op_if();
    void op_notif();
    void op_else();
    void op_endif();
    void op_not();
    void op_and();
    void op_or();

    // 控制流
    void op_return();
    void op_verify();

    // 数据 / 位运算 / 其它
    void op_cat();
    void op_split();
    void op_num2bin();
    void op_bin2num();
    void op_size();

    void op_invert();
    void op_bitand();
    void op_bitor();
    void op_bitxor();

    void op_0notequal();
    void op_mod();
    void op_lshift();
    void op_rshift();

    void op_numnotequal();

    // 加密 / 签名
    void op_ripemd160();
    void op_sha1();
    void op_sha256();
    void op_hash160();
    void op_hash256();
    void op_checksig(bool verifyOnly);
    void op_checkmultisig(bool verifyOnly);

    // 项目扩展
    void op_push_meta();
    void op_partial_hash();

    void fireEvent(VMEvent event, const std::string& message = "");
    void setError(const std::string& error);
    bool shouldBreakAtPC(size_t pc);
    void pushCallFrame(const std::string& funcName, size_t returnAddr);
    void popCallFrame();

    bool shouldStopForStep();

    std::vector<std::string> m_bytecode;
    std::shared_ptr<DebugInfo> m_debugInfo;

    VMState m_state;
    size_t m_pc;
    size_t m_instructionCount;

    StackState m_mainStack;
    StackState m_altStack;
    StackTraceRecorder m_stackTrace;
    bool m_stackTraceEnabled;
    // reset 时恢复初始栈
    StackState m_initialMainStack;
    StackState m_initialAltStack;

    std::vector<CallFrame> m_callStack;

    std::vector<size_t> m_breakpoints; // 向后兼容
    std::shared_ptr<BreakpointManager> m_breakpointMgr;
    VMEventCallback m_eventCallback;
    CheckSigCallback m_checkSigCallback;

    StepMode m_stepMode;

    std::shared_ptr<VariableInspector> m_varInspector;
    std::shared_ptr<ScopeInspector> m_scopeInspector;
    std::shared_ptr<ExpressionEvaluator> m_exprEvaluator;
    bool m_varInspectionEnabled;
    size_t m_stepStartLine;
    size_t m_stepStartCallDepth;

    // if/else/endif 用
    std::vector<bool> m_conditionStack;

    std::string m_lastError;

    size_t m_maxStackSize;
    size_t m_maxCallDepth;

    std::optional<TransactionData> m_transactionData;

    // 仅调试某段范围
    bool m_hasExecutionRange;
    size_t m_executionRangeStart;
    size_t m_executionRangeEnd;

    // 断点命中后跳过同一源码行剩余断点，避免 continue 反复停在该行
    bool m_skipBreakpointOnce;
    std::string m_skipBreakpointFile;
    size_t m_skipBreakpointLine;
    uint64_t m_nextFrameId;
    std::atomic<bool> m_pauseRequested;
    std::atomic<bool> m_terminateRequested;

    // 所有条件均 true 才真正执行
    bool isCurrentlyExecuting() const;
};

std::optional<int64_t> parseInteger(const std::string& str);
std::string integerToString(int64_t value);

} // namespace apc_debug

#endif // BVM_SIMULATOR_H

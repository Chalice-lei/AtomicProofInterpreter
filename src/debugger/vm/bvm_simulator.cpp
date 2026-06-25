#include "bvm_simulator.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

#include "../inspector/expression_evaluator.h"
#include "../inspector/scope_inspector.h"
#include "../inspector/variable_inspector.h"
#include "../../crypto/hash_utils.h"

namespace apc_debug
{

std::optional<int64_t> parseInteger(const std::string& str)
{
    try {
        // 空字符串数值上下文视为 0
        if (str.empty())
            return static_cast<int64_t>(0);

        // 显式选 16 进制，避免 base=0 把以 0 开头的当成 8 进制
        int base = 10;
        if (str.size() >= 2 && str[0] == '0' &&
            (str[1] == 'x' || str[1] == 'X')) {
            base = 16;
        } else if (str.size() >= 3 && (str[0] == '+' || str[0] == '-') &&
                   str[1] == '0' && (str[2] == 'x' || str[2] == 'X')) {
            base = 16;
        }
        return std::stoll(str, nullptr, base);
    } catch (...) {
        return std::nullopt;
    }
}

std::string integerToString(int64_t value)
{
    return std::to_string(value);
}

BVMSimulator::BVMSimulator(
    const std::vector<std::string>& bytecode,
    std::shared_ptr<DebugInfo> debugInfo
)
    : m_bytecode(bytecode), m_debugInfo(debugInfo), m_state(VMState::READY),
      m_pc(0), m_instructionCount(0), m_stackTraceEnabled(false),
      m_stepMode(StepMode::NONE), m_varInspectionEnabled(true),
      m_stepStartLine(0), m_stepStartCallDepth(0), m_maxStackSize(0),
      m_maxCallDepth(0), m_hasExecutionRange(false), m_executionRangeStart(0),
      m_executionRangeEnd(0), m_skipBreakpointOnce(false),
      m_skipBreakpointLine(0)
{
    if (m_debugInfo) {
        m_varInspector = std::make_shared<VariableInspector>(m_debugInfo);
        m_scopeInspector = std::make_shared<ScopeInspector>(m_debugInfo);
        m_exprEvaluator = std::make_shared<ExpressionEvaluator>(
            m_varInspector, m_scopeInspector
        );
    }

    // 若构造时 PC 已在函数入口，先推入主函数调用帧（不依赖 reset()）
    if (m_debugInfo && m_pc < m_bytecode.size()) {
        const auto* func = m_debugInfo->getFunctionAtPC(m_pc);
        if (func && m_pc == func->startPC) {
            size_t returnAddr = func->endPC;
            // endPC == bytecode.size() 合法；超过才无效
            if (returnAddr == 0 || returnAddr > m_bytecode.size()) {
                returnAddr = m_bytecode.size();
            }
            pushCallFrame(func->name, returnAddr);
        }
    }
}

void BVMSimulator::start()
{
    reset();
    m_state = VMState::RUNNING;
    fireEvent(VMEvent::STARTED, "VM started");
}

void BVMSimulator::setInitialStacks(
    const StackState& main,
    const StackState& alt
)
{
    m_initialMainStack = main;
    m_initialAltStack = alt;

    // 立即同步到当前栈，让用户在执行前就能看到初始参数
    m_mainStack = main;
    m_altStack = alt;
}

void BVMSimulator::reset()
{
    if (m_hasExecutionRange) {
        m_pc = m_executionRangeStart;
    } else {
        m_pc = 0;
    }

    m_instructionCount = 0;
    m_state = VMState::READY;
    m_stepMode = StepMode::NONE;
    m_lastError.clear();
    m_skipBreakpointOnce = false;
    m_skipBreakpointLine = 0;

    // 恢复初始栈快照
    m_mainStack = m_initialMainStack;
    m_altStack = m_initialAltStack;

    m_callStack.clear();
    m_conditionStack.clear();

    if (m_stackTraceEnabled) {
        m_stackTrace.clear();
    }

    m_maxStackSize = 0;
    m_maxCallDepth = 0;

    // PC 落在函数入口时，初始化主函数调用帧
    if (m_debugInfo && m_pc < m_bytecode.size()) {
        const auto* func = m_debugInfo->getFunctionAtPC(m_pc);
        if (func && m_pc == func->startPC) {
            size_t returnAddr = func->endPC;
            if (m_hasExecutionRange) {
                returnAddr = m_executionRangeEnd;
            } else if (returnAddr == 0 || returnAddr > m_bytecode.size()) {
                // endPC == bytecode.size() 合法；超过才无效，回退到末尾
                returnAddr = m_bytecode.size();
            }
            pushCallFrame(func->name, returnAddr);
        }
    }
}

void BVMSimulator::run()
{
    if (m_state != VMState::RUNNING && m_state != VMState::PAUSED) {
        start();
    }

    m_state = VMState::RUNNING;
    fireEvent(VMEvent::RESUMED, "VM resumed");

    size_t endPC = m_hasExecutionRange ? m_executionRangeEnd
                                       : m_bytecode.size();

    while (m_state == VMState::RUNNING && m_pc < endPC &&
           m_pc < m_bytecode.size()) {
        // 命中断点后跳过同一源码行剩余断点检查，避免 continue 反复停在该行
        if (m_skipBreakpointOnce) {
            size_t curLine = getCurrentLocation().line;
            if (curLine != m_skipBreakpointLine) {
                m_skipBreakpointOnce = false;
            }
        }

        if (!m_skipBreakpointOnce && shouldBreakAtPC(m_pc)) {
            m_state = VMState::PAUSED;
            m_skipBreakpointOnce = true;
            m_skipBreakpointLine = getCurrentLocation().line;
            fireEvent(
                VMEvent::BREAKPOINT_HIT,
                "Breakpoint hit at PC " + std::to_string(m_pc)
            );
            return;
        }

        if (!executeInstruction()) {
            break;
        }
    }

    if ((m_pc >= endPC || m_pc >= m_bytecode.size()) &&
        m_state == VMState::RUNNING) {
        m_state = VMState::FINISHED;
        fireEvent(VMEvent::FINISHED, "VM finished execution");
    }
}

void BVMSimulator::pause()
{
    if (m_state == VMState::RUNNING) {
        m_state = VMState::PAUSED;
        fireEvent(VMEvent::PAUSED, "VM paused");
    }
}

void BVMSimulator::resume()
{
    if (m_state == VMState::PAUSED) {
        run();
    }
}

void BVMSimulator::stepIn()
{
    // 逐行单步，可进入函数
    m_stepMode = StepMode::STEP_IN;
    m_stepStartLine = getCurrentLocation().line;
    m_stepStartCallDepth = m_callStack.size();

    if (m_state != VMState::RUNNING && m_state != VMState::PAUSED &&
        m_state != VMState::STEP_MODE) {
        start();
    }

    m_state = VMState::STEP_MODE;

    while (m_state == VMState::STEP_MODE && m_pc < m_bytecode.size()) {
        if (!executeInstruction()) {
            // 失败/结束时状态由 executeInstruction 更新
            break;
        }

        auto currentLoc = getCurrentLocation();

        // 同行断点忽略，仅新行断点才停止；避免同行多 PC 反复停下
        if (shouldBreakAtPC(m_pc)) {
            if (currentLoc.line == m_stepStartLine) {
                continue;
            }

            m_state = VMState::PAUSED;
            m_stepMode = StepMode::NONE;
            fireEvent(VMEvent::BREAKPOINT_HIT, "命中断点");
            break;
        }

        if (currentLoc.line != m_stepStartLine) {
            m_state = VMState::PAUSED;
            m_stepMode = StepMode::NONE;
            fireEvent(VMEvent::STEPPED, "单步进入（逐行）完成");
            break;
        }
    }

    if (m_pc >= m_bytecode.size() && m_state == VMState::STEP_MODE) {
        m_state = VMState::FINISHED;
        m_stepMode = StepMode::NONE;
        fireEvent(VMEvent::FINISHED, "程序执行完成");
    }
}

void BVMSimulator::stepOver()
{
    // 单步跳过：进入函数则等其执行完
    m_stepMode = StepMode::STEP_OVER;
    m_stepStartLine = getCurrentLocation().line;
    m_stepStartCallDepth = m_callStack.size();

    if (m_state != VMState::RUNNING && m_state != VMState::PAUSED) {
        start();
    }

    m_state = VMState::STEP_MODE;

    // 停止条件：源码行变化 且 调用深度回到起始（或更浅）
    while (m_state == VMState::STEP_MODE && m_pc < m_bytecode.size()) {
        if (!executeInstruction()) {
            break;
        }

        auto currentLoc = getCurrentLocation();
        size_t currentDepth = m_callStack.size();

        // 进入函数后等其返回
        if (currentDepth > m_stepStartCallDepth) {
            continue;
        }

        // 同 stepIn：忽略起始行上的断点，避免同行多 PC 反复停下
        if (shouldBreakAtPC(m_pc)) {
            if (currentLoc.line == m_stepStartLine) {
                continue;
            }

            m_state = VMState::PAUSED;
            m_stepMode = StepMode::NONE;
            fireEvent(VMEvent::BREAKPOINT_HIT, "命中断点");
            break;
        }

        // 停在：行变化 且 深度未加深
        if (currentLoc.line != m_stepStartLine &&
            currentDepth <= m_stepStartCallDepth) {
            m_state = VMState::PAUSED;
            m_stepMode = StepMode::NONE;
            fireEvent(VMEvent::STEPPED, "单步跳过完成");
            break;
        }
    }

    size_t endPC = m_hasExecutionRange ? m_executionRangeEnd
                                       : m_bytecode.size();
    if (m_pc >= endPC && m_state == VMState::STEP_MODE) {
        m_state = VMState::FINISHED;
        m_stepMode = StepMode::NONE;
        fireEvent(VMEvent::FINISHED, "程序执行完成");
    }
}

void BVMSimulator::stepOut()
{
    m_stepMode = StepMode::STEP_OUT;
    m_stepStartCallDepth = m_callStack.size();

    if (m_state != VMState::RUNNING && m_state != VMState::PAUSED) {
        start();
    }

    m_state = VMState::STEP_MODE;

    size_t endPC = m_hasExecutionRange ? m_executionRangeEnd
                                       : m_bytecode.size();

    // 执行直到调用深度减少
    while (m_state == VMState::STEP_MODE && m_pc < endPC &&
           m_pc < m_bytecode.size()) {
        if (!executeInstruction()) {
            break;
        }

        if (m_callStack.size() < m_stepStartCallDepth) {
            m_state = VMState::PAUSED;
            m_stepMode = StepMode::NONE;
            fireEvent(VMEvent::STEPPED, "Step out completed");
            break;
        }

        if (shouldBreakAtPC(m_pc)) {
            m_state = VMState::PAUSED;
            m_stepMode = StepMode::NONE;
            fireEvent(VMEvent::BREAKPOINT_HIT, "Breakpoint hit");
            break;
        }
    }

    if (m_pc >= endPC && m_state == VMState::STEP_MODE) {
        m_state = VMState::FINISHED;
        m_stepMode = StepMode::NONE;
        fireEvent(VMEvent::FINISHED, "程序执行完成");
    }
}

bool BVMSimulator::executeInstruction()
{
    // 仅在主调层级视为结束；否则 stepOver/stepIn 进入子函数会立刻 FINISHED
    if (m_hasExecutionRange && m_pc >= m_executionRangeEnd) {
        if (m_callStack.size() <= 1) {
            m_state = VMState::FINISHED;
            return false;
        }
    }

    if (m_pc >= m_bytecode.size()) {
        m_state = VMState::FINISHED;
        return false;
    }

    const size_t tracePC = m_pc;
    std::string instruction = m_bytecode[tracePC];
    std::string opcode, operand;
    std::vector<StackElement> mainStackBefore;
    std::vector<StackElement> altStackBefore;
    SourceLocation traceLocation;
    std::string traceFunctionName;

    if (m_stackTraceEnabled) {
        mainStackBefore = m_mainStack.snapshot();
        altStackBefore = m_altStack.snapshot();
        if (m_debugInfo) {
            traceLocation = m_debugInfo->getSourceLocation(tracePC);
            if (const auto* func = m_debugInfo->getFunctionAtPC(tracePC)) {
                traceFunctionName = func->name;
            }
        }
        if (traceFunctionName.empty()) {
            if (const auto* frame = getCurrentFrame()) {
                traceFunctionName = frame->functionName;
            }
        }
    }

    auto recordStackTrace = [&](const std::string& errorMessage) {
        if (!m_stackTraceEnabled) {
            return;
        }

        StackTraceStep step;
        step.pc = tracePC;
        step.instruction = instruction;
        step.opcode = opcode;
        step.operand = operand;
        step.location = traceLocation;
        step.functionName = traceFunctionName;
        step.mainStackBefore = mainStackBefore;
        step.mainStackAfter = m_mainStack.snapshot();
        step.altStackBefore = altStackBefore;
        step.altStackAfter = m_altStack.snapshot();
        step.errorMessage = errorMessage;
        m_stackTrace.record(std::move(step));
    };

    try {

        if (!parseInstruction(instruction, opcode, operand)) {
            const std::string error =
                "Failed to parse instruction: " + instruction;
            recordStackTrace(error);
            setError(error);
            return false;
        }

        executeOpcode(opcode, operand);
        recordStackTrace("");

        ++m_instructionCount;
        m_maxStackSize = std::max(m_maxStackSize, m_mainStack.size());
        m_maxCallDepth = std::max(m_maxCallDepth, m_callStack.size());

        if (!m_callStack.empty()) {
            m_callStack.back().instructionCount++;
        }

        size_t currentPC = m_pc;
        ++m_pc;

        // 跳到函数入口（如调用指令）后补一次 pushCallFrame
        if (m_debugInfo && m_pc < m_bytecode.size()) {
            const auto* func = m_debugInfo->getFunctionAtPC(m_pc);
            if (func && m_pc == func->startPC) {
                bool alreadyInStack = false;
                for (const auto& frame : m_callStack) {
                    if (frame.functionName == func->name &&
                        frame.entryPC == m_pc) {
                        alreadyInStack = true;
                        break;
                    }
                }

                if (!alreadyInStack) {
                    size_t returnAddr = func->endPC;
                    if (returnAddr == 0 || returnAddr > m_bytecode.size()) {
                        returnAddr = m_bytecode.size();
                    }
                    pushCallFrame(func->name, returnAddr);
                }
            }
        }

        // 函数出口：m_pc 已自增，>= func->endPC 即视为执行完毕
        if (m_debugInfo && !m_callStack.empty()) {
            const auto* func = m_debugInfo->getFunctionAtPC(currentPC);
            if (func && func->endPC > 0) {
                if (m_pc >= func->endPC) {
                    if (!m_callStack.empty() &&
                        m_callStack.back().functionName == func->name &&
                        m_callStack.back().entryPC == func->startPC) {
                        popCallFrame();
                    }
                }
            }
        }

        fireEvent(VMEvent::INSTRUCTION_EXECUTED, opcode);

        return true;

    } catch (const std::exception& e) {
        const std::string error = std::string("Execution error: ") + e.what();
        recordStackTrace(error);
        setError(error);
        return false;
    }
}

bool BVMSimulator::parseInstruction(
    const std::string& instruction,
    std::string& opcode,
    std::string& operand
)
{
    if (instruction.empty()) {
        return false;
    }

    // 直接 PUSH：前两位为 0x01..0x4b，对应字节数
    if (instruction.length() >= 2) {
        std::string firstTwo = instruction.substr(0, 2);
        try {
            int num = std::stoi(firstTwo, nullptr, 16);
            if (num >= 1 && num <= 75) {
                opcode = firstTwo;
                operand = instruction.length() > 2 ? instruction.substr(2) : "";
                operand.erase(0, operand.find_first_not_of(" \t"));
                return true;
            }
        } catch (...) {
        }
    }

    // OP_PUSHDATA{1,2,4}：去掉长度前缀
    if (instruction.length() >= 12 &&
        instruction.substr(0, 12) == "OP_PUSHDATA1") {
        opcode = "OP_PUSHDATA1";
        std::string remaining = instruction.substr(12);
        remaining.erase(0, remaining.find_first_not_of(" \t"));
        if (remaining.length() >= 2) {
            operand = remaining.length() > 2 ? remaining.substr(2) : "";
            operand.erase(0, operand.find_first_not_of(" \t"));
        } else {
            operand = "";
        }
        return true;
    } else if (instruction.length() >= 12 &&
               instruction.substr(0, 12) == "OP_PUSHDATA2") {
        opcode = "OP_PUSHDATA2";
        std::string remaining = instruction.substr(12);
        remaining.erase(0, remaining.find_first_not_of(" \t"));
        if (remaining.length() >= 4) {
            operand = remaining.length() > 4 ? remaining.substr(4) : "";
            operand.erase(0, operand.find_first_not_of(" \t"));
        } else {
            operand = "";
        }
        return true;
    } else if (instruction.length() >= 12 &&
               instruction.substr(0, 12) == "OP_PUSHDATA4") {
        opcode = "OP_PUSHDATA4";
        std::string remaining = instruction.substr(12);
        remaining.erase(0, remaining.find_first_not_of(" \t"));
        if (remaining.length() >= 8) {
            operand = remaining.length() > 8 ? remaining.substr(8) : "";
            operand.erase(0, operand.find_first_not_of(" \t"));
        } else {
            operand = "";
        }
        return true;
    }

    // 其余：整个 instruction 即操作码
    opcode = instruction;
    operand = "";
    return true;
}

void BVMSimulator::executeOpcode(
    const std::string& opcode,
    const std::string& operand
)
{
    std::string op = opcode;
    std::transform(op.begin(), op.end(), op.begin(), ::toupper);

    // exec=false 时，非控制流指令视为 no-op
    bool isControlFlow =
        (op == "OP_IF" || op == "OP_NOTIF" || op == "OP_ELSE" ||
         op == "OP_ENDIF");
    bool exec = isCurrentlyExecuting();
    if (!exec && !isControlFlow) {
        return;
    }

    // 常量
    if (op == "OP_0" || op == "OP_FALSE") {
        // ScriptNum(0) 约定即空字节数组
        m_mainStack.pushInt(0);
    } else if (op == "OP_1" || op == "OP_TRUE") {
        m_mainStack.pushInt(1);
    } else if (op == "OP_1NEGATE") {
        m_mainStack.pushInt(-1);
    } else if (op.length() > 3 && op.substr(0, 3) == "OP_" &&
               op.length() <= 5 && std::isdigit(op[3])) {
        // OP_2 .. OP_16
        std::string num = op.substr(3);
        auto vOpt = parseInteger(num);
        if (!vOpt) {
            throw std::runtime_error("Invalid OP_n constant: " + op);
        }
        m_mainStack.pushInt(*vOpt);
    } else if (op.size() == 2 && std::isxdigit(op[0]) && std::isxdigit(op[1])) {
        // 直接 PUSH (0x01..0x4b)：操作码即字节数
        int opByte = std::stoi(op, nullptr, 16);
        if (opByte >= 1 && opByte <= 0x4b) {
            op_push(operand);
        } else {
            throw std::runtime_error("Unknown opcode byte: 0x" + op);
        }
    } else if (op == "OP_PUSHDATA1") {
        op_push(operand);
    } else if (op == "OP_PUSHDATA2") {
        op_push(operand);
    } else if (op == "OP_PUSHDATA4") {
        op_push(operand);
    }

    // 栈操作
    else if (op == "OP_DUP")
        op_dup();
    else if (op == "OP_DROP")
        op_drop();
    else if (op == "OP_2DROP")
        op_2drop();
    else if (op == "OP_2DUP")
        op_2dup();
    else if (op == "OP_3DUP")
        op_3dup();
    else if (op == "OP_OVER")
        op_over();
    else if (op == "OP_2OVER")
        op_2over();
    else if (op == "OP_ROT")
        op_rot();
    else if (op == "OP_2ROT")
        op_2rot();
    else if (op == "OP_SWAP")
        op_swap();
    else if (op == "OP_2SWAP")
        op_2swap();
    else if (op == "OP_IFDUP")
        op_ifdup();
    else if (op == "OP_DEPTH")
        op_depth();
    else if (op == "OP_NIP")
        op_nip();
    else if (op == "OP_TUCK")
        op_tuck();
    else if (op == "OP_PICK")
        op_pick(operand);
    else if (op == "OP_ROLL")
        op_roll(operand);
    else if (op == "OP_TOALTSTACK")
        op_toaltstack();
    else if (op == "OP_FROMALTSTACK")
        op_fromaltstack();

    // 数据操作
    else if (op == "OP_CAT")
        op_cat();
    else if (op == "OP_SPLIT")
        op_split();
    else if (op == "OP_NUM2BIN")
        op_num2bin();
    else if (op == "OP_BIN2NUM")
        op_bin2num();
    else if (op == "OP_SIZE")
        op_size();

    // 位运算
    else if (op == "OP_INVERT")
        op_invert();
    else if (op == "OP_AND")
        op_bitand();
    else if (op == "OP_OR")
        op_bitor();
    else if (op == "OP_XOR")
        op_bitxor();

    // 算术
    else if (op == "OP_ADD")
        op_add();
    else if (op == "OP_SUB")
        op_sub();
    else if (op == "OP_MUL")
        op_mul();
    else if (op == "OP_DIV")
        op_div();
    else if (op == "OP_NEGATE")
        op_neg();
    else if (op == "OP_ABS")
        op_abs();
    else if (op == "OP_1ADD")
        op_inc();
    else if (op == "OP_1SUB")
        op_dec();
    else if (op == "OP_0NOTEQUAL")
        op_0notequal();
    else if (op == "OP_MOD")
        op_mod();
    else if (op == "OP_LSHIFT")
        op_lshift();
    else if (op == "OP_RSHIFT")
        op_rshift();

    // 比较
    else if (op == "OP_EQUAL")
        op_equal();
    else if (op == "OP_EQUALVERIFY") {
        op_equal();
        op_verify();
    } else if (op == "OP_NUMEQUAL")
        op_numequal();
    else if (op == "OP_NUMEQUALVERIFY") {
        op_numequal();
        op_verify();
    } else if (op == "OP_NUMNOTEQUAL")
        op_numnotequal();
    else if (op == "OP_LESSTHAN")
        op_lessthan();
    else if (op == "OP_GREATERTHAN")
        op_greaterthan();
    else if (op == "OP_LESSTHANOREQUAL")
        op_lessthanorequal();
    else if (op == "OP_GREATERTHANOREQUAL")
        op_greaterthanorequal();
    else if (op == "OP_MIN")
        op_min();
    else if (op == "OP_MAX")
        op_max();
    else if (op == "OP_WITHIN")
        op_within();

    // 逻辑
    else if (op == "OP_IF")
        op_if();
    else if (op == "OP_NOTIF")
        op_notif();
    else if (op == "OP_ELSE")
        op_else();
    else if (op == "OP_ENDIF")
        op_endif();
    else if (op == "OP_NOT")
        op_not();
    else if (op == "OP_BOOLAND")
        op_and();
    else if (op == "OP_BOOLOR")
        op_or();

    // 控制流
    else if (op == "OP_NOP") {
    } else if (op == "OP_RETURN")
        op_return();
    else if (op == "OP_VERIFY")
        op_verify();

    // 加密
    else if (op == "OP_RIPEMD160")
        op_ripemd160();
    else if (op == "OP_SHA1")
        op_sha1();
    else if (op == "OP_SHA256")
        op_sha256();
    else if (op == "OP_HASH160")
        op_hash160();
    else if (op == "OP_HASH256")
        op_hash256();
    else if (op == "OP_CODESEPARATOR") {
        // 真实 Script 中影响签名哈希范围；调试 VM 视作 NOP
    } else if (op == "OP_CHECKSIG")
        op_checksig(false);
    else if (op == "OP_CHECKSIGVERIFY")
        op_checksig(true);
    else if (op == "OP_CHECKMULTISIG")
        op_checkmultisig(false);
    else if (op == "OP_CHECKMULTISIGVERIFY")
        op_checkmultisig(true);
    else if (op == "OP_CHECKLOCKTIMEVERIFY" || op == "OP_CHECKSEQUENCEVERIFY") {
        // 时间锁：调试 VM 视作 NOP
    } else if (op == "OP_PUSH_META") {
        op_push_meta();
    } else if (op == "OP_PARTIAL_HASH") {
        op_partial_hash();
    }

    else {
        throw std::runtime_error("Unknown opcode: " + opcode);
    }
}

void BVMSimulator::op_push(const std::string& operand)
{
    if (operand.empty()) {
        throw std::runtime_error("OP_PUSH requires an operand");
    }

    // operand 是十六进制串，按字节数组入栈
    m_mainStack.push(StackElement::fromHexLiteral(operand));
}

void BVMSimulator::op_dup()
{
    m_mainStack.dup();
}

void BVMSimulator::op_drop()
{
    m_mainStack.drop();
}

void BVMSimulator::op_swap()
{
    m_mainStack.swap();
}

void BVMSimulator::op_2drop()
{
    // x1 x2 -> (空)
    m_mainStack.drop(2);
}

void BVMSimulator::op_2dup()
{
    // x1 x2 -> x1 x2 x1 x2
    auto x2 = m_mainStack.pop();
    auto x1 = m_mainStack.pop();
    m_mainStack.push(x1);
    m_mainStack.push(x2);
    m_mainStack.push(x1);
    m_mainStack.push(x2);
}

void BVMSimulator::op_3dup()
{
    // x1 x2 x3 -> x1 x2 x3 x1 x2 x3
    auto x3 = m_mainStack.pop();
    auto x2 = m_mainStack.pop();
    auto x1 = m_mainStack.pop();
    m_mainStack.push(x1);
    m_mainStack.push(x2);
    m_mainStack.push(x3);
    m_mainStack.push(x1);
    m_mainStack.push(x2);
    m_mainStack.push(x3);
}

void BVMSimulator::op_over()
{
    // x1 x2 -> x1 x2 x1
    m_mainStack.pick(1);
}

void BVMSimulator::op_2over()
{
    // x1 x2 x3 x4 -> x1 x2 x3 x4 x1 x2；pick 后 x2 已被推到深度 3
    m_mainStack.pick(3);
    m_mainStack.pick(3);
}

void BVMSimulator::op_rot()
{
    // x1 x2 x3 -> x2 x3 x1
    auto x3 = m_mainStack.pop();
    auto x2 = m_mainStack.pop();
    auto x1 = m_mainStack.pop();
    m_mainStack.push(x2);
    m_mainStack.push(x3);
    m_mainStack.push(x1);
}

void BVMSimulator::op_2rot()
{
    // x1 x2 x3 x4 x5 x6 -> x3 x4 x5 x6 x1 x2
    auto x6 = m_mainStack.pop();
    auto x5 = m_mainStack.pop();
    auto x4 = m_mainStack.pop();
    auto x3 = m_mainStack.pop();
    auto x2 = m_mainStack.pop();
    auto x1 = m_mainStack.pop();
    m_mainStack.push(x3);
    m_mainStack.push(x4);
    m_mainStack.push(x5);
    m_mainStack.push(x6);
    m_mainStack.push(x1);
    m_mainStack.push(x2);
}

void BVMSimulator::op_2swap()
{
    // x1 x2 x3 x4 -> x3 x4 x1 x2
    auto x4 = m_mainStack.pop();
    auto x3 = m_mainStack.pop();
    auto x2 = m_mainStack.pop();
    auto x1 = m_mainStack.pop();
    m_mainStack.push(x3);
    m_mainStack.push(x4);
    m_mainStack.push(x1);
    m_mainStack.push(x2);
}

void BVMSimulator::op_ifdup()
{
    // 栈顶非 0 时复制
    auto top = m_mainStack.peek(0);
    auto val = top.toInt();
    if (val && *val != 0) {
        m_mainStack.dup();
    }
}

void BVMSimulator::op_depth()
{
    m_mainStack.pushInt(static_cast<int64_t>(m_mainStack.size()));
}

void BVMSimulator::op_nip()
{
    // x1 x2 -> x2
    m_mainStack.roll(1);
    m_mainStack.drop();
}

void BVMSimulator::op_tuck()
{
    // x1 x2 -> x2 x1 x2
    auto x2 = m_mainStack.pop();
    auto x1 = m_mainStack.pop();
    m_mainStack.push(x2);
    m_mainStack.push(x1);
    m_mainStack.push(x2);
}

void BVMSimulator::op_pick(const std::string& /*operand*/)
{
    // 弹出 <n>，把第 n 个元素复制到栈顶
    // xn ... x1 x0 <n>  ->  xn ... x1 x0 xn
    auto nElem = m_mainStack.pop();
    auto depth = nElem.toInt();
    if (!depth || *depth < 0) {
        throw std::runtime_error(
            "OP_PICK requires a non-negative numeric operand on stack"
        );
    }

    m_mainStack.pick(static_cast<size_t>(*depth));
}

void BVMSimulator::op_roll(const std::string& /*operand*/)
{
    // 弹出 <n>，把第 n 个元素移到栈顶
    // xn ... x1 x0 <n>  ->  ... x1 x0 xn
    auto nElem = m_mainStack.pop();
    auto depth = nElem.toInt();
    if (!depth || *depth < 0) {
        throw std::runtime_error(
            "OP_ROLL requires a non-negative numeric operand on stack"
        );
    }

    m_mainStack.roll(static_cast<size_t>(*depth));
}

void BVMSimulator::op_toaltstack()
{
    auto elem = m_mainStack.pop();
    m_altStack.push(elem);
}

void BVMSimulator::op_fromaltstack()
{
    auto elem = m_altStack.pop();
    m_mainStack.push(elem);
}

static const std::vector<uint8_t>& asBytes(const StackElement& e)
{
    return e.data;
}

void BVMSimulator::op_cat()
{
    // 拼接：x1 x2 -> x1||x2
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();
    std::vector<uint8_t> out;
    out.reserve(asBytes(a).size() + asBytes(b).size());
    out.insert(out.end(), asBytes(a).begin(), asBytes(a).end());
    out.insert(out.end(), asBytes(b).begin(), asBytes(b).end());
    m_mainStack.push(StackElement(std::move(out)));
}

void BVMSimulator::op_split()
{
    // 按字节位置拆分：x n -> x1 x2
    auto nElem = m_mainStack.pop();
    auto xElem = m_mainStack.pop();
    auto nOpt = nElem.toInt();
    if (!nOpt || *nOpt < 0) {
        throw std::runtime_error("OP_SPLIT requires non-negative index");
    }
    size_t n = static_cast<size_t>(*nOpt);
    const auto& x = asBytes(xElem);
    if (n > x.size()) {
        throw std::runtime_error("OP_SPLIT index out of range");
    }
    std::vector<uint8_t> x1(x.begin(), x.begin() + n);
    std::vector<uint8_t> x2(x.begin() + n, x.end());
    m_mainStack.push(StackElement(std::move(x1)));
    m_mainStack.push(StackElement(std::move(x2)));
}

void BVMSimulator::op_num2bin()
{
    // a (int) b (字节数) -> 大端字节串，左侧补 0
    auto bElem = m_mainStack.pop();
    auto aElem = m_mainStack.pop();
    auto aVal = aElem.toInt();
    auto bVal = bElem.toInt();
    if (!aVal || !bVal || *bVal < 0) {
        throw std::runtime_error("OP_NUM2BIN requires numeric operands");
    }
    size_t len = static_cast<size_t>(*bVal);
    if (len == 0) {
        m_mainStack.push(StackElement(std::vector<uint8_t>{}));
        return;
    }
    int64_t v = *aVal;
    std::vector<uint8_t> bytes;
    while (v != 0 && bytes.size() < len) {
        bytes.insert(bytes.begin(), static_cast<uint8_t>(v & 0xFF));
        v >>= 8;
    }
    if (bytes.size() > len) {
        throw std::runtime_error("OP_NUM2BIN: overflow for target length");
    }
    if (bytes.size() < len) {
        bytes.insert(bytes.begin(), len - bytes.size(), 0x00);
    }
    m_mainStack.push(StackElement(std::move(bytes)));
}

void BVMSimulator::op_bin2num()
{
    auto xElem = m_mainStack.pop();
    const auto& x = asBytes(xElem);
    int64_t v = 0;
    for (uint8_t c : x) {
        v = (v << 8) | static_cast<int64_t>(c);
    }
    m_mainStack.pushInt(v);
}

void BVMSimulator::op_size()
{
    // x -> x len
    auto xElem = m_mainStack.pop();
    const auto& x = asBytes(xElem);
    m_mainStack.push(xElem);
    m_mainStack.pushInt(static_cast<int64_t>(x.size()));
}

void BVMSimulator::op_invert()
{
    auto xElem = m_mainStack.pop();
    std::vector<uint8_t> x = asBytes(xElem);
    for (uint8_t& c : x) {
        c = static_cast<uint8_t>(~c);
    }
    m_mainStack.push(StackElement(std::move(x)));
}

void BVMSimulator::op_bitand()
{
    auto bElem = m_mainStack.pop();
    auto aElem = m_mainStack.pop();
    const auto& a = asBytes(aElem);
    const auto& b = asBytes(bElem);
    size_t n = std::min(a.size(), b.size());
    std::vector<uint8_t> out(n, 0x00);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>(a[i] & b[i]);
    }
    m_mainStack.push(StackElement(std::move(out)));
}

void BVMSimulator::op_bitor()
{
    auto bElem = m_mainStack.pop();
    auto aElem = m_mainStack.pop();
    const auto& a = asBytes(aElem);
    const auto& b = asBytes(bElem);
    size_t n = std::min(a.size(), b.size());
    std::vector<uint8_t> out(n, 0x00);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>(a[i] | b[i]);
    }
    m_mainStack.push(StackElement(std::move(out)));
}

void BVMSimulator::op_bitxor()
{
    auto bElem = m_mainStack.pop();
    auto aElem = m_mainStack.pop();
    const auto& a = asBytes(aElem);
    const auto& b = asBytes(bElem);
    size_t n = std::min(a.size(), b.size());
    std::vector<uint8_t> out(n, 0x00);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>(a[i] ^ b[i]);
    }
    m_mainStack.push(StackElement(std::move(out)));
}

void BVMSimulator::op_add()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();

    auto aVal = a.toInt();
    auto bVal = b.toInt();

    if (!aVal || !bVal) {
        throw std::runtime_error("OP_ADD requires numeric operands");
    }

    m_mainStack.pushInt(*aVal + *bVal);
}

void BVMSimulator::op_sub()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();

    auto aVal = a.toInt();
    auto bVal = b.toInt();

    if (!aVal || !bVal) {
        throw std::runtime_error("OP_SUB requires numeric operands");
    }

    m_mainStack.pushInt(*aVal - *bVal);
}

void BVMSimulator::op_mul()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();

    auto aVal = a.toInt();
    auto bVal = b.toInt();

    if (!aVal || !bVal) {
        throw std::runtime_error("OP_MUL requires numeric operands");
    }

    m_mainStack.pushInt(*aVal * *bVal);
}

void BVMSimulator::op_div()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();

    auto aVal = a.toInt();
    auto bVal = b.toInt();

    if (!aVal || !bVal) {
        throw std::runtime_error("OP_DIV requires numeric operands");
    }

    if (*bVal == 0) {
        throw std::runtime_error("Division by zero");
    }

    m_mainStack.pushInt(*aVal / *bVal);
}

void BVMSimulator::op_neg()
{
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();

    if (!aVal) {
        throw std::runtime_error("OP_NEG requires numeric operand");
    }

    m_mainStack.pushInt(-(*aVal));
}

void BVMSimulator::op_abs()
{
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();

    if (!aVal) {
        throw std::runtime_error("OP_ABS requires numeric operand");
    }

    m_mainStack.pushInt(std::abs(*aVal));
}

void BVMSimulator::op_inc()
{
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();

    if (!aVal) {
        throw std::runtime_error("OP_1ADD requires numeric operand");
    }

    m_mainStack.pushInt(*aVal + 1);
}

void BVMSimulator::op_dec()
{
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();

    if (!aVal) {
        throw std::runtime_error("OP_1SUB requires numeric operand");
    }

    m_mainStack.pushInt(*aVal - 1);
}

void BVMSimulator::op_0notequal()
{
    // x -> (x != 0 ? 1 : 0)
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();
    m_mainStack.pushInt((aVal && (*aVal != 0)) ? 1 : 0);
}

void BVMSimulator::op_mod()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();
    auto bVal = b.toInt();
    if (!aVal || !bVal) {
        throw std::runtime_error("OP_MOD requires numeric operands");
    }
    if (*bVal == 0) {
        throw std::runtime_error("Modulo by zero");
    }
    m_mainStack.pushInt(*aVal % *bVal);
}

void BVMSimulator::op_lshift()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();
    auto bVal = b.toInt();
    if (!aVal || !bVal || *bVal < 0) {
        throw std::runtime_error("OP_LSHIFT requires non-negative shift");
    }
    m_mainStack.pushInt(*aVal << *bVal);
}

void BVMSimulator::op_rshift()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();
    auto bVal = b.toInt();
    if (!aVal || !bVal || *bVal < 0) {
        throw std::runtime_error("OP_RSHIFT requires non-negative shift");
    }
    m_mainStack.pushInt(*aVal >> *bVal);
}

void BVMSimulator::op_equal()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();

    m_mainStack.pushInt((a.data == b.data) ? 1 : 0);
}

void BVMSimulator::op_numequal()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();

    auto aVal = a.toInt();
    auto bVal = b.toInt();

    if (!aVal || !bVal) {
        throw std::runtime_error("OP_NUMEQUAL requires numeric operands");
    }

    m_mainStack.pushInt((*aVal == *bVal) ? 1 : 0);
}

void BVMSimulator::op_numnotequal()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();
    auto bVal = b.toInt();
    if (!aVal || !bVal) {
        throw std::runtime_error("OP_NUMNOTEQUAL requires numeric operands");
    }
    m_mainStack.pushInt((*aVal != *bVal) ? 1 : 0);
}

void BVMSimulator::op_lessthan()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();

    auto aVal = a.toInt();
    auto bVal = b.toInt();

    if (!aVal || !bVal) {
        throw std::runtime_error("OP_LESSTHAN requires numeric operands");
    }

    m_mainStack.pushInt((*aVal < *bVal) ? 1 : 0);
}

void BVMSimulator::op_greaterthan()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();

    auto aVal = a.toInt();
    auto bVal = b.toInt();

    if (!aVal || !bVal) {
        throw std::runtime_error("OP_GREATERTHAN requires numeric operands");
    }

    m_mainStack.pushInt((*aVal > *bVal) ? 1 : 0);
}

void BVMSimulator::op_lessthanorequal()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();
    auto bVal = b.toInt();
    if (!aVal || !bVal) {
        throw std::runtime_error("OP_LESSTHANOREQUAL requires numeric operands"
        );
    }
    m_mainStack.pushInt((*aVal <= *bVal) ? 1 : 0);
}

void BVMSimulator::op_greaterthanorequal()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();
    auto bVal = b.toInt();
    if (!aVal || !bVal) {
        throw std::runtime_error(
            "OP_GREATERTHANOREQUAL requires numeric operands"
        );
    }
    m_mainStack.pushInt((*aVal >= *bVal) ? 1 : 0);
}

void BVMSimulator::op_min()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();
    auto bVal = b.toInt();
    if (!aVal || !bVal) {
        throw std::runtime_error("OP_MIN requires numeric operands");
    }
    m_mainStack.pushInt((*aVal <= *bVal) ? *aVal : *bVal);
}

void BVMSimulator::op_max()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();
    auto bVal = b.toInt();
    if (!aVal || !bVal) {
        throw std::runtime_error("OP_MAX requires numeric operands");
    }
    m_mainStack.pushInt((*aVal >= *bVal) ? *aVal : *bVal);
}

void BVMSimulator::op_within()
{
    // x min max -> 1 if min <= x < max
    auto maxElem = m_mainStack.pop();
    auto minElem = m_mainStack.pop();
    auto xElem = m_mainStack.pop();
    auto xVal = xElem.toInt();
    auto minVal = minElem.toInt();
    auto maxVal = maxElem.toInt();
    if (!xVal || !minVal || !maxVal) {
        throw std::runtime_error("OP_WITHIN requires numeric operands");
    }
    bool inside = (*xVal >= *minVal) && (*xVal < *maxVal);
    m_mainStack.pushInt(inside ? 1 : 0);
}

void BVMSimulator::op_if()
{
    // 仅在父执行状态为 true 时才消费栈顶作为条件
    bool parentExec = isCurrentlyExecuting();
    bool condTrue = false;

    if (parentExec) {
        auto condition = m_mainStack.pop();
        auto condVal = condition.toInt();
        condTrue = (condVal && (*condVal != 0));
    }

    m_conditionStack.push_back(parentExec && condTrue);
}

void BVMSimulator::op_notif()
{
    // OP_IF 的反向
    bool parentExec = isCurrentlyExecuting();
    bool condTrue = false;

    if (parentExec) {
        auto condition = m_mainStack.pop();
        auto condVal = condition.toInt();
        condTrue = (condVal && (*condVal != 0));
    }

    m_conditionStack.push_back(parentExec && !condTrue);
}

void BVMSimulator::op_else()
{
    if (m_conditionStack.empty()) {
        throw std::runtime_error("OP_ELSE without matching OP_IF");
    }

    // 父执行状态：除当前层外的所有条件均 true
    bool parentExec = true;
    if (m_conditionStack.size() > 1) {
        parentExec = std::all_of(
            m_conditionStack.begin(),
            m_conditionStack.end() - 1,
            [](bool v) { return v; }
        );
    }

    bool current = m_conditionStack.back();
    m_conditionStack.back() = parentExec && !current;
}

void BVMSimulator::op_endif()
{
    if (m_conditionStack.empty()) {
        throw std::runtime_error("OP_ENDIF without matching OP_IF");
    }

    m_conditionStack.pop_back();
}

void BVMSimulator::op_not()
{
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();

    // Bitcoin Script 语义：0->1，其他（含非法值）->0
    if (!aVal) {
        m_mainStack.pushInt(0);
        return;
    }

    if (*aVal == 0) {
        m_mainStack.pushInt(1);
    } else if (*aVal == 1) {
        m_mainStack.pushInt(0);
    } else {
        m_mainStack.pushInt(0);
    }
}

void BVMSimulator::op_and()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();

    auto aVal = a.toInt();
    auto bVal = b.toInt();

    if (!aVal || !bVal) {
        throw std::runtime_error("OP_BOOLAND requires numeric operands");
    }

    m_mainStack.pushInt((*aVal != 0 && *bVal != 0) ? 1 : 0);
}

void BVMSimulator::op_or()
{
    auto b = m_mainStack.pop();
    auto a = m_mainStack.pop();

    auto aVal = a.toInt();
    auto bVal = b.toInt();

    if (!aVal || !bVal) {
        throw std::runtime_error("OP_BOOLOR requires numeric operands");
    }

    m_mainStack.pushInt((*aVal != 0 || *bVal != 0) ? 1 : 0);
}


void BVMSimulator::op_return()
{
    // 顶层（栈空或只剩调试入口帧）直接标记完成
    if (m_callStack.size() <= 1) {
        m_state = VMState::FINISHED;
        fireEvent(VMEvent::FINISHED, "程序执行完成");
        return;
    }
    // 嵌套调用：立即返回当前函数，跳过 OP_RETURN 之后的字节码
    popCallFrame();
    // popCallFrame 把 m_pc 设为 returnPC，executeInstruction 会再 ++m_pc，故此处先减
    if (m_pc > 0) {
        m_pc--;
    }
}

void BVMSimulator::op_verify()
{
    auto a = m_mainStack.pop();
    auto aVal = a.toInt();

    if (!aVal || *aVal == 0) {
        throw std::runtime_error("OP_VERIFY failed: value is false or zero");
    }
}

void BVMSimulator::op_ripemd160()
{
    auto x = m_mainStack.pop();
    m_mainStack.push(StackElement(apc_crypto::ripemd160Digest(asBytes(x))));
}

void BVMSimulator::op_sha1()
{
    auto x = m_mainStack.pop();
    m_mainStack.push(StackElement(apc_crypto::sha1Digest(asBytes(x))));
}

void BVMSimulator::op_sha256()
{
    auto x = m_mainStack.pop();
    m_mainStack.push(StackElement(apc_crypto::sha256Digest(asBytes(x))));
}

void BVMSimulator::op_hash160()
{
    auto x = m_mainStack.pop();
    m_mainStack.push(StackElement(apc_crypto::hash160Digest(asBytes(x))));
}

void BVMSimulator::op_hash256()
{
    auto x = m_mainStack.pop();
    m_mainStack.push(StackElement(apc_crypto::hash256Digest(asBytes(x))));
}

void BVMSimulator::op_checksig(bool verifyOnly)
{
    // 栈顶: sig pubkey；未配置回调时保留调试 VM 默认通过语义
    auto pubkey = m_mainStack.pop();
    auto sig = m_mainStack.pop();
    std::vector<uint8_t> sigBytes = asBytes(sig);
    std::vector<uint8_t> pubkeyBytes = asBytes(pubkey);
    std::vector<uint8_t> sighash;

    // Bitcoin/TBC 脚本签名末字节通常是 sighash type，回调接收纯 DER 签名。
    if (!sigBytes.empty()) {
        sigBytes.pop_back();
    }
    if (m_transactionData && !m_transactionData->inputsHash.empty()) {
        sighash = m_transactionData->inputsHash;
    }

    bool ok = true;
    if (m_checkSigCallback) {
        ok = m_checkSigCallback(sigBytes, pubkeyBytes, sighash);
    }

    if (verifyOnly) {
        if (!ok) {
            throw std::runtime_error("OP_CHECKSIGVERIFY failed");
        }
    } else {
        m_mainStack.pushInt(ok ? 1 : 0);
    }
}

void BVMSimulator::op_checkmultisig(bool verifyOnly)
{
    (void)verifyOnly;
    throw std::runtime_error(
        "OP_CHECKMULTISIG is not implemented in BVMSimulator; "
        "real multisig verification requires full stack-layout support"
    );
}

SourceLocation BVMSimulator::getCurrentLocation() const
{
    if (m_debugInfo) {
        return m_debugInfo->getSourceLocation(m_pc);
    }
    return SourceLocation();
}

std::string BVMSimulator::getCurrentInstruction() const
{
    if (m_pc < m_bytecode.size()) {
        return m_bytecode[m_pc];
    }
    return "";
}

const CallFrame* BVMSimulator::getCurrentFrame() const
{
    if (m_callStack.empty()) {
        return nullptr;
    }
    return &m_callStack.back();
}

void BVMSimulator::addBreakpoint(size_t pc)
{
    if (std::find(m_breakpoints.begin(), m_breakpoints.end(), pc) ==
        m_breakpoints.end()) {
        m_breakpoints.push_back(pc);
    }
}

void BVMSimulator::removeBreakpoint(size_t pc)
{
    m_breakpoints.erase(
        std::remove(m_breakpoints.begin(), m_breakpoints.end(), pc),
        m_breakpoints.end()
    );
}

void BVMSimulator::clearBreakpoints()
{
    m_breakpoints.clear();
}

bool BVMSimulator::hasBreakpoint(size_t pc) const
{
    return std::find(m_breakpoints.begin(), m_breakpoints.end(), pc) !=
           m_breakpoints.end();
}

BVMSimulator::Statistics BVMSimulator::getStatistics() const
{
    Statistics stats;
    stats.totalInstructions = m_bytecode.size();
    stats.executedInstructions = m_instructionCount;
    stats.maxStackSize = m_maxStackSize;
    stats.maxCallDepth = m_maxCallDepth;
    return stats;
}

void BVMSimulator::setStackTraceEnabled(bool enable)
{
    m_stackTraceEnabled = enable;
    m_stackTrace.clear();
}

void BVMSimulator::clearStackTrace()
{
    m_stackTrace.clear();
}

bool BVMSimulator::saveStackTrace(
    const std::string& filename,
    const std::string& sourceCode
) const
{
    return m_stackTrace.save(filename, m_bytecode, m_debugInfo, sourceCode);
}

void BVMSimulator::fireEvent(VMEvent event, const std::string& message)
{
    if (m_eventCallback) {
        m_eventCallback(event, message);
    }
}

void BVMSimulator::setError(const std::string& error)
{
    m_lastError = error;
    m_state = VMState::ERROR;
    fireEvent(VMEvent::ERROR, error);
}

bool BVMSimulator::shouldBreakAtPC(size_t pc)
{
    // 简单 PC 断点（向后兼容）优先
    if (hasBreakpoint(pc)) {
        return true;
    }

    if (m_breakpointMgr) {
        return m_breakpointMgr->shouldBreakAtPC(pc, *this);
    }

    return false;
}

void BVMSimulator::pushCallFrame(const std::string& funcName, size_t returnAddr)
{
    CallFrame frame(
        funcName, returnAddr, getCurrentLocation(), m_mainStack.size(), m_pc
    );

    // 关联作用域信息（参考 Python Frame.f_locals）
    if (m_debugInfo) {
        auto funcInfo = m_debugInfo->functions.find(funcName);
        if (funcInfo != m_debugInfo->functions.end()) {
            frame.scope = funcInfo->second.scope;
        }
    }

    m_callStack.push_back(frame);
}

void BVMSimulator::popCallFrame()
{
    if (!m_callStack.empty()) {
        auto frame = m_callStack.back();
        m_callStack.pop_back();
        m_pc = frame.returnPC;
    }
}

bool BVMSimulator::shouldStopForStep()
{
    auto currentLoc = getCurrentLocation();
    size_t currentLine = currentLoc.line;
    size_t currentDepth = m_callStack.size();

    switch (m_stepMode) {
        case StepMode::STEP_IN:
            return currentLine != m_stepStartLine;

        case StepMode::STEP_OVER:
            // 行变 且 深度未加深
            return (currentDepth <= m_stepStartCallDepth) &&
                   (currentLine != m_stepStartLine);

        case StepMode::STEP_OUT:
            return currentDepth < m_stepStartCallDepth;

        default:
            return false;
    }
}

void BVMSimulator::enableVariableInspection(bool enable)
{
    m_varInspectionEnabled = enable;

    if (enable && m_debugInfo && !m_varInspector) {
        m_varInspector = std::make_shared<VariableInspector>(m_debugInfo);
        m_scopeInspector = std::make_shared<ScopeInspector>(m_debugInfo);
        m_exprEvaluator = std::make_shared<ExpressionEvaluator>(
            m_varInspector, m_scopeInspector
        );
    }
}

bool BVMSimulator::isCurrentlyExecuting() const
{
    if (m_conditionStack.empty()) {
        return true;
    }
    return std::all_of(
        m_conditionStack.begin(),
        m_conditionStack.end(),
        [](bool v) { return v; }
    );
}

void BVMSimulator::setTransactionData(const TransactionData& txData)
{
    m_transactionData = txData;
}

bool BVMSimulator::loadTransactionFromHex(const std::string& hexStr)
{
    // TODO: 按交易二进制格式解析；当前仅做格式校验
    std::string cleanHex;
    for (char c : hexStr) {
        if (!std::isspace(c)) {
            cleanHex += c;
        }
    }

    if (cleanHex.length() % 2 != 0) {
        return false;
    }

    return false;
}

bool BVMSimulator::loadTransactionFromFile(const std::string& filename)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        return false;
    }

    std::string hexStr;
    std::string line;
    while (std::getline(file, line)) {
        hexStr += line;
    }

    return loadTransactionFromHex(hexStr);
}

// 根据栈顶 1..7 的条件推送对应交易元数据
void BVMSimulator::op_push_meta()
{
    if (m_mainStack.size() < 1) {
        throw std::runtime_error(
            "OP_PUSH_META requires at least 1 element on stack"
        );
    }

    auto vchElem = m_mainStack.pop();
    const auto& vch = asBytes(vchElem);

    if (vch.size() != 1) {
        throw std::runtime_error("OP_PUSH_META: condition must be 1 byte");
    }

    uint8_t condition = vch[0];
    if (condition < 1 || condition > 7) {
        throw std::runtime_error(
            "OP_PUSH_META: condition must be between 1 and 7"
        );
    }

    switch (condition) {
        case 1: {
            // nVersion (4 字节)
            std::vector<uint8_t> nVersionBytes(4);
            uint32_t version = m_transactionData ? m_transactionData->version
                                                 : 1;
            std::memcpy(nVersionBytes.data(), &version, 4);
            m_mainStack.push(StackElement(std::move(nVersionBytes)));
            break;
        }
        case 2: {
            // nLockTime (4 字节)
            std::vector<uint8_t> nLockTimeBytes(4);
            uint32_t lockTime = m_transactionData ? m_transactionData->lockTime
                                                  : 0;
            std::memcpy(nLockTimeBytes.data(), &lockTime, 4);
            m_mainStack.push(StackElement(std::move(nLockTimeBytes)));
            break;
        }
        case 3: {
            // vin.size()
            std::vector<uint8_t> vinSizeBytes(4);
            uint32_t vinSize = m_transactionData ? m_transactionData->inputCount
                                                 : 1;
            std::memcpy(vinSizeBytes.data(), &vinSize, 4);
            m_mainStack.push(StackElement(std::move(vinSizeBytes)));
            break;
        }
        case 4: {
            // vout.size()
            std::vector<uint8_t> voutSizeBytes(4);
            uint32_t voutSize = m_transactionData
                                    ? m_transactionData->outputCount
                                    : 1;
            std::memcpy(voutSizeBytes.data(), &voutSize, 4);
            m_mainStack.push(StackElement(std::move(voutSizeBytes)));
            break;
        }
        case 5: {
            // inputsHash
            std::vector<uint8_t> data;
            if (m_transactionData && !m_transactionData->inputsHash.empty()) {
                data = m_transactionData->inputsHash;
            } else {
                data.assign(32, 0x00);
            }
            m_mainStack.push(StackElement(std::move(data)));
            break;
        }
        case 6: {
            // unlockingInput；默认 40 字节兼容 txid(32)+vout(4)+sequence(4)
            std::vector<uint8_t> data;
            if (m_transactionData &&
                !m_transactionData->unlockingInput.empty()) {
                data = m_transactionData->unlockingInput;
            } else {
                data.assign(40, 0x00);
            }
            m_mainStack.push(StackElement(std::move(data)));
            break;
        }
        case 7: {
            // outputsHash
            std::vector<uint8_t> data;
            if (m_transactionData && !m_transactionData->outputsHash.empty()) {
                data = m_transactionData->outputsHash;
            } else {
                data.assign(32, 0x00);
            }
            m_mainStack.push(StackElement(std::move(data)));
            break;
        }
        default:
            throw std::runtime_error("OP_PUSH_META: invalid condition value");
    }
}

// 增量 SHA256
// 栈输入（从底到顶）：vch、vchPartHash、vchSize
// 栈输出：vchHash（32 字节）
void BVMSimulator::op_partial_hash()
{
    if (m_mainStack.size() < 3) {
        throw std::runtime_error(
            "OP_PARTIAL_HASH requires at least 3 elements on stack"
        );
    }

    auto vchSizeElem = m_mainStack.pop();
    auto vchPartHashElem = m_mainStack.pop();
    auto vchElem = m_mainStack.pop();

    const auto& vch = asBytes(vchElem);
    const auto& vchPartHash = asBytes(vchPartHashElem);
    const auto& vchSize = asBytes(vchSizeElem);

    // vchSize 按小端解析为 uint64
    uint64_t vchSizeUint64 = 0;
    for (size_t i = 0; i < vchSize.size() && i < 8; ++i) {
        vchSizeUint64 |= static_cast<uint64_t>(vchSize[i]) << (8 * i);
    }

    uint64_t remainVchSizeUint64 = vch.size();
    std::vector<uint8_t> vchHash(32);

    if (vchPartHash.size() == 0) {
        // 无前置哈希，直接 SHA256(vch)
        if (vchSizeUint64 != remainVchSizeUint64) {
            throw std::runtime_error(
                "OP_PARTIAL_HASH: size mismatch when vchPartHash is empty"
            );
        }
        vchHash = apc_crypto::sha256Digest(vch);
    } else if (vchPartHash.size() == 32) {
        // 增量：SHA256(vchPartHash || partHashSize_LE8 || vch)
        if (vchSizeUint64 < remainVchSizeUint64) {
            throw std::runtime_error(
                "OP_PARTIAL_HASH: size mismatch when vchPartHash is present"
            );
        }

        uint64_t partHashSize = vchSizeUint64 - remainVchSizeUint64;

        uint8_t partHashSizeArray[8];
        for (int i = 0; i < 8; ++i) {
            partHashSizeArray[i] = static_cast<uint8_t>(
                (partHashSize >> (8 * i)) & 0xFF
            );
        }

        std::vector<uint8_t> dataToHash;
        dataToHash.reserve(32 + 8 + vch.size());
        dataToHash
            .insert(dataToHash.end(), vchPartHash.begin(), vchPartHash.end());
        dataToHash
            .insert(dataToHash.end(), partHashSizeArray, partHashSizeArray + 8);
        dataToHash.insert(dataToHash.end(), vch.begin(), vch.end());

        vchHash = apc_crypto::sha256Digest(dataToHash);
    } else {
        throw std::runtime_error(
            "OP_PARTIAL_HASH: invalid vchPartHash size (must be 0 or 32)"
        );
    }

    m_mainStack.push(StackElement(std::move(vchHash)));
}

void BVMSimulator::setExecutionRange(size_t startPC, size_t endPC)
{
    if (startPC >= m_bytecode.size()) {
        throw std::runtime_error("执行范围起始位置超出字节码范围");
    }
    if (endPC > m_bytecode.size()) {
        throw std::runtime_error("执行范围结束位置超出字节码范围");
    }
    if (startPC >= endPC) {
        throw std::runtime_error("执行范围起始位置必须小于结束位置");
    }

    m_hasExecutionRange = true;
    m_executionRangeStart = startPC;
    m_executionRangeEnd = endPC;

    m_pc = startPC;
}

void BVMSimulator::clearExecutionRange()
{
    m_hasExecutionRange = false;
    m_executionRangeStart = 0;
    m_executionRangeEnd = 0;
}

} // namespace apc_debug

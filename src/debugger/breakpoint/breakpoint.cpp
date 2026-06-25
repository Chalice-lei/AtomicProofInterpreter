#include "breakpoint.h"
#include "../info/debug_info.h"
#include "../vm/bvm_simulator.h"
#include <sstream>

namespace apc_debug {

bool LineBreakpoint::shouldBreak(const BVMSimulator& vm) const {
    if (!isEnabled()) {
        return false;
    }
    if (m_hitCount < m_ignoreCount) {
        return false;
    }

    size_t currentPC = vm.getPC();
    return m_resolvedPCs.find(currentPC) != m_resolvedPCs.end();
}

bool FunctionBreakpoint::shouldBreak(const BVMSimulator& vm) const {
    if (!isEnabled()) {
        return false;
    }
    if (m_hitCount < m_ignoreCount) {
        return false;
    }

    auto debugInfo = vm.getDebugInfo();
    if (!debugInfo) {
        return false;
    }

    size_t currentPC = vm.getPC();
    const FunctionDebugInfo* func = debugInfo->getFunctionAtPC(currentPC);

    if (!func) {
        return false;
    }

    // 仅在函数入口处命中
    if (currentPC == func->startPC) {
        return func->name == m_functionName;
    }

    return false;
}

bool ConditionalBreakpoint::shouldBreak(const BVMSimulator& vm) const {
    if (!LineBreakpoint::shouldBreak(vm)) {
        return false;
    }

    // TODO: 表达式求值；目前命中行断点即触发
    if (m_condition.empty()) {
        return true;
    }
    return true;
}

bool DataBreakpoint::shouldBreak(const BVMSimulator& /* vm */) const {
    if (!isEnabled()) {
        return false;
    }

    // TODO: 读取变量值并与上次比较
    return false;
}

std::string breakpointTypeToString(BreakpointType type) {
    switch (type) {
        case BreakpointType::LINE: return "line";
        case BreakpointType::FUNCTION: return "function";
        case BreakpointType::CONDITIONAL: return "conditional";
        case BreakpointType::DATA: return "data";
        default: return "unknown";
    }
}

BreakpointType stringToBreakpointType(const std::string& str) {
    if (str == "line") return BreakpointType::LINE;
    if (str == "function") return BreakpointType::FUNCTION;
    if (str == "conditional") return BreakpointType::CONDITIONAL;
    if (str == "data") return BreakpointType::DATA;
    return BreakpointType::LINE;
}

std::string breakpointStateToString(BreakpointState state) {
    switch (state) {
        case BreakpointState::ENABLED: return "enabled";
        case BreakpointState::DISABLED: return "disabled";
        case BreakpointState::PENDING: return "pending";
        default: return "unknown";
    }
}

} // namespace apc_debug


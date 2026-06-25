#include "breakpoint_manager.h"
#include "../vm/bvm_simulator.h"
#include <algorithm>

namespace apc_debug {

BreakpointManager::BreakpointManager(std::shared_ptr<DebugInfo> debugInfo)
    : m_debugInfo(debugInfo), m_nextId(1) {
}

size_t BreakpointManager::addLineBreakpoint(const std::string& filename, size_t line) {
    if (!m_debugInfo || m_debugInfo->getPCsForLine(line).empty()) {
        return 0;
    }
    auto bp = std::make_shared<LineBreakpoint>(m_nextId, filename, line);
    m_breakpoints[m_nextId] = bp;

    resolveBreakpoint(m_nextId);

    size_t id = m_nextId;
    ++m_nextId;
    return id;
}

size_t BreakpointManager::addFunctionBreakpoint(const std::string& functionName) {
    if (!m_debugInfo || m_debugInfo->functions.find(functionName) == m_debugInfo->functions.end()) {
        return 0;
    }
    auto bp = std::make_shared<FunctionBreakpoint>(m_nextId, functionName);
    m_breakpoints[m_nextId] = bp;
    
    size_t id = m_nextId;
    ++m_nextId;
    return id;
}

size_t BreakpointManager::addConditionalBreakpoint(const std::string& filename,
                                                   size_t line,
                                                   const std::string& condition) {
    if (!m_debugInfo || m_debugInfo->getPCsForLine(line).empty()) {
        return 0;
    }
    auto bp = std::make_shared<ConditionalBreakpoint>(m_nextId, filename, line, condition);
    m_breakpoints[m_nextId] = bp;

    resolveBreakpoint(m_nextId);

    size_t id = m_nextId;
    ++m_nextId;
    return id;
}

size_t BreakpointManager::addDataBreakpoint(const std::string& variableName) {
    auto bp = std::make_shared<DataBreakpoint>(m_nextId, variableName);
    m_breakpoints[m_nextId] = bp;
    
    size_t id = m_nextId;
    ++m_nextId;
    return id;
}

size_t BreakpointManager::addTemporaryBreakpoint(const std::string& filename, size_t line) {
    if (!m_debugInfo || m_debugInfo->getPCsForLine(line).empty()) {
        return 0;
    }
    auto bp = std::make_shared<LineBreakpoint>(m_nextId, filename, line);
    bp->setTemporary(true);
    m_breakpoints[m_nextId] = bp;

    resolveBreakpoint(m_nextId);

    size_t id = m_nextId;
    ++m_nextId;
    return id;
}

bool BreakpointManager::setBreakpointCondition(size_t id, const std::string& condition) {
    auto bp = getBreakpoint(id);
    if (!bp) {
        return false;
    }
    
    if (auto lineBp = std::dynamic_pointer_cast<LineBreakpoint>(bp)) {
        // TODO: 行断点转条件断点
        (void)condition;
        return false;
    } else if (auto condBp = std::dynamic_pointer_cast<ConditionalBreakpoint>(bp)) {
        condBp->setCondition(condition);
        return true;
    }

    return false;
}

bool BreakpointManager::removeBreakpoint(size_t id) {
    auto it = m_breakpoints.find(id);
    if (it == m_breakpoints.end()) {
        return false;
    }

    removePCToBreakpointMapping(id);

    m_breakpoints.erase(it);
    return true;
}

void BreakpointManager::removeAllBreakpoints() {
    m_breakpoints.clear();
    m_pcToBreakpoints.clear();
}

bool BreakpointManager::enableBreakpoint(size_t id) {
    auto bp = getBreakpoint(id);
    if (!bp) {
        return false;
    }
    
    bp->setState(BreakpointState::ENABLED);
    return true;
}

bool BreakpointManager::disableBreakpoint(size_t id) {
    auto bp = getBreakpoint(id);
    if (!bp) {
        return false;
    }
    
    bp->setState(BreakpointState::DISABLED);
    return true;
}

std::shared_ptr<Breakpoint> BreakpointManager::getBreakpoint(size_t id) const {
    auto it = m_breakpoints.find(id);
    if (it == m_breakpoints.end()) {
        return nullptr;
    }
    return it->second;
}

std::vector<std::shared_ptr<Breakpoint>> BreakpointManager::getAllBreakpoints() const {
    std::vector<std::shared_ptr<Breakpoint>> result;
    result.reserve(m_breakpoints.size());
    
    for (const auto& [id, bp] : m_breakpoints) {
        result.push_back(bp);
    }
    
    return result;
}

std::vector<std::shared_ptr<Breakpoint>> BreakpointManager::getBreakpointsAtLine(
    const std::string& filename, size_t line) const {
    std::vector<std::shared_ptr<Breakpoint>> result;
    
    for (const auto& [id, bp] : m_breakpoints) {
        if (auto lineBp = std::dynamic_pointer_cast<LineBreakpoint>(bp)) {
            if (lineBp->getFilename() == filename && lineBp->getLine() == line) {
                result.push_back(bp);
            }
        } else if (auto condBp = std::dynamic_pointer_cast<ConditionalBreakpoint>(bp)) {
            if (condBp->getFilename() == filename && condBp->getLine() == line) {
                result.push_back(bp);
            }
        }
    }
    
    return result;
}

std::vector<std::shared_ptr<Breakpoint>> BreakpointManager::getBreakpointsAtFunction(
    const std::string& functionName) const {
    std::vector<std::shared_ptr<Breakpoint>> result;
    
    for (const auto& [id, bp] : m_breakpoints) {
        if (auto funcBp = std::dynamic_pointer_cast<FunctionBreakpoint>(bp)) {
            if (funcBp->getFunctionName() == functionName) {
                result.push_back(bp);
            }
        }
    }
    
    return result;
}

void BreakpointManager::resolveBreakpoints() {
    for (const auto& [id, bp] : m_breakpoints) {
        resolveBreakpoint(id);
    }
}

bool BreakpointManager::resolveBreakpoint(size_t id) {
    auto bp = getBreakpoint(id);
    if (!bp) {
        return false;
    }

    removePCToBreakpointMapping(id);

    if (auto lineBp = std::dynamic_pointer_cast<LineBreakpoint>(bp)) {
        size_t line = lineBp->getLine();
        std::string filename = lineBp->getFilename();

        if (!m_debugInfo) {
            lineBp->setState(BreakpointState::PENDING);
            return false;
        }

        auto pcs = m_debugInfo->getPCsForLine(line);

        if (pcs.empty()) {
            lineBp->setState(BreakpointState::PENDING);
            return false;
        }

        lineBp->clearResolvedPCs();
        for (size_t pc : pcs) {
            lineBp->addResolvedPC(pc);
        }

        lineBp->setState(BreakpointState::ENABLED);
        updatePCToBreakpointMapping(id);
        return true;

    } else if (auto funcBp = std::dynamic_pointer_cast<FunctionBreakpoint>(bp)) {
        if (!m_debugInfo) {
            return false;
        }

        // 不预设 PC 映射，命中检查在 shouldBreakAtPC 中完成
        auto it = m_debugInfo->functions.find(funcBp->getFunctionName());
        if (it != m_debugInfo->functions.end()) {
            funcBp->setState(BreakpointState::ENABLED);
            return true;
        }

        funcBp->setState(BreakpointState::PENDING);
        return false;
    }

    return true;
}

bool BreakpointManager::shouldBreakAtPC(size_t pc, const BVMSimulator& vm) const {
    if (!vm.isCurrentBranchExecuting()) {
        return false;
    }

    auto it = m_pcToBreakpoints.find(pc);
    if (it != m_pcToBreakpoints.end()) {
        for (size_t id : it->second) {
            auto bp = getBreakpoint(id);
            if (bp && bp->shouldBreak(vm)) {
                bp->incrementHitCount();
                // 注意：const 方法无法在此删除临时断点，需调用方在外部处理
                return true;
            }
        }
    }

    for (const auto& [id, bp] : m_breakpoints) {
        if (auto funcBp = std::dynamic_pointer_cast<FunctionBreakpoint>(bp)) {
            if (funcBp->shouldBreak(vm)) {
                funcBp->incrementHitCount();
                return true;
            }
        }
    }

    return false;
}

std::vector<std::shared_ptr<Breakpoint>> BreakpointManager::getBreakpointsAtPC(size_t pc) const {
    std::vector<std::shared_ptr<Breakpoint>> result;
    
    auto it = m_pcToBreakpoints.find(pc);
    if (it != m_pcToBreakpoints.end()) {
        for (size_t id : it->second) {
            auto bp = getBreakpoint(id);
            if (bp) {
                result.push_back(bp);
            }
        }
    }
    
    return result;
}

size_t BreakpointManager::getEnabledBreakpoints() const {
    size_t count = 0;
    for (const auto& [id, bp] : m_breakpoints) {
        if (bp->isEnabled()) {
            ++count;
        }
    }
    return count;
}

size_t BreakpointManager::getResolvedBreakpoints() const {
    size_t count = 0;
    for (const auto& [id, bp] : m_breakpoints) {
        if (auto lineBp = std::dynamic_pointer_cast<LineBreakpoint>(bp)) {
            if (lineBp->isResolved()) {
                ++count;
            }
        } else if (std::dynamic_pointer_cast<FunctionBreakpoint>(bp)) {
            // 函数断点视为已解析，由运行时检查
            ++count;
        }
    }
    return count;
}

void BreakpointManager::updatePCToBreakpointMapping(size_t id) {
    auto bp = getBreakpoint(id);
    if (!bp) {
        return;
    }
    
    if (auto lineBp = std::dynamic_pointer_cast<LineBreakpoint>(bp)) {
        for (size_t pc : lineBp->getResolvedPCs()) {
            m_pcToBreakpoints[pc].insert(id);
        }
    }
}

void BreakpointManager::removePCToBreakpointMapping(size_t id) {
    auto bp = getBreakpoint(id);
    if (!bp) {
        return;
    }
    
    if (auto lineBp = std::dynamic_pointer_cast<LineBreakpoint>(bp)) {
        for (size_t pc : lineBp->getResolvedPCs()) {
            auto it = m_pcToBreakpoints.find(pc);
            if (it != m_pcToBreakpoints.end()) {
                it->second.erase(id);
                if (it->second.empty()) {
                    m_pcToBreakpoints.erase(it);
                }
            }
        }
    }
}

std::vector<size_t> BreakpointManager::findNearestValidLine(
    const std::string& /* filename */, size_t line, size_t maxDistance) const {

    if (!m_debugInfo) {
        return std::vector<size_t>();
    }

    auto pcs = m_debugInfo->getPCsForLine(line);
    if (!pcs.empty()) {
        return pcs;
    }

    for (size_t dist = 1; dist <= maxDistance; ++dist) {
        if (line + dist <= 10000) {
            pcs = m_debugInfo->getPCsForLine(line + dist);
            if (!pcs.empty()) {
                return pcs;
            }
        }

        if (line > dist) {
            pcs = m_debugInfo->getPCsForLine(line - dist);
            if (!pcs.empty()) {
                return pcs;
            }
        }
    }

    return std::vector<size_t>();
}

} // namespace apc_debug

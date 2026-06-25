#ifndef BREAKPOINT_MANAGER_H
#define BREAKPOINT_MANAGER_H

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "breakpoint.h"
#include "../info/debug_info.h"

namespace apc_debug {

class BVMSimulator;

class BreakpointManager {
public:
    explicit BreakpointManager(std::shared_ptr<DebugInfo> debugInfo);

    size_t addLineBreakpoint(const std::string& filename, size_t line);
    size_t addFunctionBreakpoint(const std::string& functionName);
    size_t addConditionalBreakpoint(const std::string& filename, size_t line,
                                   const std::string& condition);
    size_t addDataBreakpoint(const std::string& variableName);

    // 命中一次后自动删除
    size_t addTemporaryBreakpoint(const std::string& filename, size_t line);

    bool setBreakpointCondition(size_t id, const std::string& condition);

    bool removeBreakpoint(size_t id);
    void removeAllBreakpoints();

    bool enableBreakpoint(size_t id);
    bool disableBreakpoint(size_t id);

    std::shared_ptr<Breakpoint> getBreakpoint(size_t id) const;
    std::vector<std::shared_ptr<Breakpoint>> getAllBreakpoints() const;

    std::vector<std::shared_ptr<Breakpoint>> getBreakpointsAtLine(
        const std::string& filename, size_t line) const;

    std::vector<std::shared_ptr<Breakpoint>> getBreakpointsAtFunction(
        const std::string& functionName) const;

    // 把源码位置解析为字节码位置
    void resolveBreakpoints();
    bool resolveBreakpoint(size_t id);

    bool shouldBreakAtPC(size_t pc, const BVMSimulator& vm) const;
    std::vector<std::shared_ptr<Breakpoint>> getBreakpointsAtPC(size_t pc) const;

    size_t getTotalBreakpoints() const { return m_breakpoints.size(); }
    size_t getEnabledBreakpoints() const;
    size_t getResolvedBreakpoints() const;

    void setDebugInfo(std::shared_ptr<DebugInfo> debugInfo) {
        m_debugInfo = debugInfo;
        m_pcToBreakpoints.clear();
        resolveBreakpoints();
    }

    std::shared_ptr<DebugInfo> getDebugInfo() const { return m_debugInfo; }

private:
    std::shared_ptr<DebugInfo> m_debugInfo;
    std::map<size_t, std::shared_ptr<Breakpoint>> m_breakpoints;
    size_t m_nextId;

    // PC -> 断点 ID 索引，加速命中查询
    std::map<size_t, std::set<size_t>> m_pcToBreakpoints;

    void updatePCToBreakpointMapping(size_t id);
    void removePCToBreakpointMapping(size_t id);
    std::vector<size_t> findNearestValidLine(const std::string& filename,
                                            size_t line,
                                            size_t maxDistance = 10) const;
};

} // namespace apc_debug

#endif // BREAKPOINT_MANAGER_H

#ifndef BREAKPOINT_H
#define BREAKPOINT_H

#include <memory>
#include <set>
#include <string>
#include <cstddef>

namespace apc_debug {

class BVMSimulator;

enum class BreakpointType {
    LINE,
    FUNCTION,
    CONDITIONAL,
    DATA            // 监视点
};

enum class BreakpointState {
    ENABLED,
    DISABLED,
    PENDING         // 暂未解析到有效位置
};

class Breakpoint {
public:
    Breakpoint(size_t id, BreakpointType type)
        : m_id(id), m_type(type), m_state(BreakpointState::ENABLED),
          m_hitCount(0), m_ignoreCount(0), m_isTemporary(false) {}
    
    virtual ~Breakpoint() = default;

    size_t getId() const { return m_id; }
    BreakpointType getType() const { return m_type; }
    BreakpointState getState() const { return m_state; }
    void setState(BreakpointState state) { m_state = state; }

    size_t getHitCount() const { return m_hitCount; }
    void incrementHitCount() { ++m_hitCount; }
    void resetHitCount() { m_hitCount = 0; }

    // 跳过前 N 次命中
    size_t getIgnoreCount() const { return m_ignoreCount; }
    void setIgnoreCount(size_t count) { m_ignoreCount = count; }

    // 命中一次后自动删除
    bool isTemporary() const { return m_isTemporary; }
    void setTemporary(bool temp) { m_isTemporary = temp; }

    virtual bool shouldBreak(const BVMSimulator& vm) const = 0;
    virtual std::string getDescription() const = 0;

    bool isEnabled() const {
        return m_state == BreakpointState::ENABLED;
    }

protected:
    size_t m_id;
    BreakpointType m_type;
    BreakpointState m_state;
    size_t m_hitCount;
    size_t m_ignoreCount;
    bool m_isTemporary;
};

class LineBreakpoint : public Breakpoint {
public:
    LineBreakpoint(size_t id, const std::string& filename, size_t line)
        : Breakpoint(id, BreakpointType::LINE),
          m_filename(filename), m_line(line) {}
    
    bool shouldBreak(const BVMSimulator& vm) const override;
    
    std::string getFilename() const { return m_filename; }
    size_t getLine() const { return m_line; }

    const std::set<size_t>& getResolvedPCs() const { return m_resolvedPCs; }
    void addResolvedPC(size_t pc) { m_resolvedPCs.insert(pc); }
    void clearResolvedPCs() { m_resolvedPCs.clear(); }
    bool isResolved() const { return !m_resolvedPCs.empty(); }
    
    std::string getDescription() const override {
        return "行断点: " + m_filename + ":" + std::to_string(m_line);
    }
    
private:
    std::string m_filename;
    size_t m_line;
    std::set<size_t> m_resolvedPCs;
};

class FunctionBreakpoint : public Breakpoint {
public:
    FunctionBreakpoint(size_t id, const std::string& functionName)
        : Breakpoint(id, BreakpointType::FUNCTION),
          m_functionName(functionName) {}
    
    bool shouldBreak(const BVMSimulator& vm) const override;
    
    std::string getFunctionName() const { return m_functionName; }
    
    std::string getDescription() const override {
        return "函数断点: " + m_functionName;
    }
    
private:
    std::string m_functionName;
};

// 带条件的行断点
class ConditionalBreakpoint : public LineBreakpoint {
public:
    ConditionalBreakpoint(size_t id, const std::string& filename,
                         size_t line, const std::string& condition)
        : LineBreakpoint(id, filename, line), m_condition(condition) {
        m_type = BreakpointType::CONDITIONAL;
    }
    
    bool shouldBreak(const BVMSimulator& vm) const override;
    
    std::string getCondition() const { return m_condition; }
    void setCondition(const std::string& condition) { m_condition = condition; }
    
    std::string getDescription() const override {
        return "条件断点: " + getFilename() + ":" + std::to_string(getLine()) +
               " (条件: " + m_condition + ")";
    }
    
private:
    std::string m_condition;
    // TODO: 表达式求值器
};

// 监视点
class DataBreakpoint : public Breakpoint {
public:
    DataBreakpoint(size_t id, const std::string& variableName)
        : Breakpoint(id, BreakpointType::DATA),
          m_variableName(variableName), m_lastValue("") {}
    
    bool shouldBreak(const BVMSimulator& vm) const override;
    
    std::string getVariableName() const { return m_variableName; }
    
    std::string getDescription() const override {
        return "数据断点: " + m_variableName;
    }
    
private:
    std::string m_variableName;
    mutable std::string m_lastValue;  // 用于检测变化
};

std::string breakpointTypeToString(BreakpointType type);
BreakpointType stringToBreakpointType(const std::string& str);
std::string breakpointStateToString(BreakpointState state);

} // namespace apc_debug

#endif // BREAKPOINT_H


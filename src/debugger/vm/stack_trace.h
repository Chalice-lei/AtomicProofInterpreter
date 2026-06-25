#ifndef STACK_TRACE_H
#define STACK_TRACE_H

#include <memory>
#include <string>
#include <vector>

#include "../info/debug_info.h"
#include "stack_state.h"

namespace apc_debug
{

struct StackTraceStep
{
    size_t stepIndex = 0;
    size_t pc = 0;
    std::string instruction;
    std::string opcode;
    std::string operand;
    SourceLocation location;
    std::string functionName;
    std::vector<StackElement> mainStackBefore;
    std::vector<StackElement> mainStackAfter;
    std::vector<StackElement> altStackBefore;
    std::vector<StackElement> altStackAfter;
    std::string errorMessage;
};

class StackTraceRecorder
{
public:
    void clear();
    void record(StackTraceStep step);

    bool empty() const
    {
        return m_steps.empty();
    }

    size_t size() const
    {
        return m_steps.size();
    }

    const std::vector<StackTraceStep>& steps() const
    {
        return m_steps;
    }

    std::string toJson(
        const std::vector<std::string>& bytecode,
        const std::shared_ptr<DebugInfo>& debugInfo,
        const std::string& sourceCode = ""
    ) const;

    bool save(
        const std::string& filename,
        const std::vector<std::string>& bytecode,
        const std::shared_ptr<DebugInfo>& debugInfo,
        const std::string& sourceCode = ""
    ) const;

private:
    std::vector<StackTraceStep> m_steps;
};

} // namespace apc_debug

#endif // STACK_TRACE_H

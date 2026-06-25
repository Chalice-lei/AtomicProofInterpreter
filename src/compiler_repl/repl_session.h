#ifndef REPL_SESSION_H
#define REPL_SESSION_H

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "../compiler/compiler_driver.h"

#ifdef ENABLE_DEBUGGER
#include "../debugger/vm/stack_state.h"
#endif

namespace apc::repl
{

class ReplSession
{
public:
    int nextInputIndex() const
    {
        return m_nextInputIndex;
    }

    void advanceInputIndex()
    {
        ++m_nextInputIndex;
    }

    void addImport(const std::string& text);
    void addMember(const std::string& text);
    void addStatement(const std::string& text);
    void setOutput(int index, const std::string& value);

    void setLastCompile(
        const CompilerResult& result,
        std::string syntheticSource,
        std::string entryFunction,
        bool hasCellRange = false,
        size_t cellStartLine = 0,
        size_t cellEndLine = 0,
        bool hasPCRange = false,
        size_t pcStart = 0,
        size_t pcEnd = 0
    );

#ifdef ENABLE_DEBUGGER
    bool hasVmState() const
    {
        return m_hasVmState;
    }

    const apc_debug::StackState& mainStack() const
    {
        return m_mainStack;
    }

    const apc_debug::StackState& altStack() const
    {
        return m_altStack;
    }

    void setVmState(
        const apc_debug::StackState& main,
        const apc_debug::StackState& alt
    );
    void clearVmState();

    bool hasLastInitialVmState() const
    {
        return m_hasLastInitialVmState;
    }

    const apc_debug::StackState& lastInitialMainStack() const
    {
        return m_lastInitialMainStack;
    }

    const apc_debug::StackState& lastInitialAltStack() const
    {
        return m_lastInitialAltStack;
    }

    void setLastInitialVmState(
        const apc_debug::StackState& main,
        const apc_debug::StackState& alt
    );
    void clearLastInitialVmState();
#endif

    void reset();

    const std::vector<std::string>& imports() const
    {
        return m_imports;
    }

    const std::vector<std::string>& members() const
    {
        return m_members;
    }

    const std::vector<std::string>& statements() const
    {
        return m_statements;
    }

    const std::map<int, std::string>& outputs() const
    {
        return m_outputs;
    }

    bool hasLastCompile() const
    {
        return m_hasLastCompile;
    }

    const CompilerResult& lastCompile() const
    {
        return m_lastCompile;
    }

    const std::string& lastSyntheticSource() const
    {
        return m_lastSyntheticSource;
    }

    const std::string& lastEntryFunction() const
    {
        return m_lastEntryFunction;
    }

    bool hasLastCellRange() const
    {
        return m_hasLastCellRange;
    }

    size_t lastCellStartLine() const
    {
        return m_lastCellStartLine;
    }

    size_t lastCellEndLine() const
    {
        return m_lastCellEndLine;
    }

    bool hasLastPCRange() const
    {
        return m_hasLastPCRange;
    }

    size_t lastPCStart() const
    {
        return m_lastPCStart;
    }

    size_t lastPCEnd() const
    {
        return m_lastPCEnd;
    }

private:
    int m_nextInputIndex = 1;
    std::vector<std::string> m_imports;
    std::vector<std::string> m_members;
    std::vector<std::string> m_statements;
    std::map<int, std::string> m_outputs;

    bool m_hasLastCompile = false;
    CompilerResult m_lastCompile;
    std::string m_lastSyntheticSource;
    std::string m_lastEntryFunction;
    bool m_hasLastCellRange = false;
    size_t m_lastCellStartLine = 0;
    size_t m_lastCellEndLine = 0;
    bool m_hasLastPCRange = false;
    size_t m_lastPCStart = 0;
    size_t m_lastPCEnd = 0;

#ifdef ENABLE_DEBUGGER
    bool m_hasVmState = false;
    apc_debug::StackState m_mainStack;
    apc_debug::StackState m_altStack;

    bool m_hasLastInitialVmState = false;
    apc_debug::StackState m_lastInitialMainStack;
    apc_debug::StackState m_lastInitialAltStack;
#endif
};

} // namespace apc::repl

#endif // REPL_SESSION_H

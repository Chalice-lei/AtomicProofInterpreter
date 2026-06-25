#include "repl_session.h"

#include <utility>

namespace apc::repl
{

void ReplSession::addImport(const std::string& text)
{
    m_imports.push_back(text);
}

void ReplSession::addMember(const std::string& text)
{
    m_members.push_back(text);
}

void ReplSession::addStatement(const std::string& text)
{
    m_statements.push_back(text);
}

void ReplSession::setOutput(int index, const std::string& value)
{
    m_outputs[index] = value;
}

void ReplSession::setLastCompile(
    const CompilerResult& result,
    std::string syntheticSource,
    std::string entryFunction,
    bool hasCellRange,
    size_t cellStartLine,
    size_t cellEndLine,
    bool hasPCRange,
    size_t pcStart,
    size_t pcEnd
)
{
    m_lastCompile = result;
    m_lastSyntheticSource = std::move(syntheticSource);
    m_lastEntryFunction = std::move(entryFunction);
    m_hasLastCellRange = hasCellRange;
    m_lastCellStartLine = cellStartLine;
    m_lastCellEndLine = cellEndLine;
    m_hasLastPCRange = hasPCRange;
    m_lastPCStart = pcStart;
    m_lastPCEnd = pcEnd;
    m_hasLastCompile = true;
}

#ifdef ENABLE_DEBUGGER
void ReplSession::setVmState(
    const apc_debug::StackState& main,
    const apc_debug::StackState& alt
)
{
    m_mainStack = main;
    m_altStack = alt;
    m_hasVmState = true;
}

void ReplSession::clearVmState()
{
    m_mainStack.clear();
    m_altStack.clear();
    m_hasVmState = false;
}

void ReplSession::setLastInitialVmState(
    const apc_debug::StackState& main,
    const apc_debug::StackState& alt
)
{
    m_lastInitialMainStack = main;
    m_lastInitialAltStack = alt;
    m_hasLastInitialVmState = true;
}

void ReplSession::clearLastInitialVmState()
{
    m_lastInitialMainStack.clear();
    m_lastInitialAltStack.clear();
    m_hasLastInitialVmState = false;
}
#endif

void ReplSession::reset()
{
    m_imports.clear();
    m_members.clear();
    m_statements.clear();
    m_outputs.clear();
    m_lastCompile = CompilerResult();
    m_lastSyntheticSource.clear();
    m_lastEntryFunction.clear();
    m_hasLastCellRange = false;
    m_lastCellStartLine = 0;
    m_lastCellEndLine = 0;
    m_hasLastPCRange = false;
    m_lastPCStart = 0;
    m_lastPCEnd = 0;
    m_hasLastCompile = false;
#ifdef ENABLE_DEBUGGER
    clearVmState();
    clearLastInitialVmState();
#endif
    m_nextInputIndex = 1;
}

} // namespace apc::repl

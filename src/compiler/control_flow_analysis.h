#ifndef CONTROL_FLOW_ANALYSIS_H
#define CONTROL_FLOW_ANALYSIS_H

#include "../ast/ast.h"

namespace compiler_flow {

// A path may leave the current statement in three distinct ways. Lowercase
// return exits the current inline function, whereas uppercase Return terminates
// the script by emitting OP_RETURN.
struct ControlFlowOutcomes
{
    bool fallsThrough{false};
    bool inlineReturns{false};
    bool scriptTerminates{false};
};

inline ControlFlowOutcomes controlFlowOutcomes(const StmtNode* statement);

inline ControlFlowOutcomes sequenceControlFlowOutcomes(
    const std::vector<std::unique_ptr<StmtNode>>& statements,
    size_t startIndex = 0
)
{
    ControlFlowOutcomes result{.fallsThrough = true};

    for (size_t i = startIndex; i < statements.size(); ++i) {
        if (!result.fallsThrough) {
            break;
        }

        const ControlFlowOutcomes current =
            controlFlowOutcomes(statements[i].get());
        result.fallsThrough = current.fallsThrough;
        result.inlineReturns =
            result.inlineReturns || current.inlineReturns;
        result.scriptTerminates =
            result.scriptTerminates || current.scriptTerminates;
    }

    return result;
}

inline ControlFlowOutcomes controlFlowOutcomes(const StmtNode* statement)
{
    if (!statement) {
        return {.fallsThrough = true};
    }

    if (const auto* returnNode =
            dynamic_cast<const ReturnNode*>(statement)) {
        return returnNode->isValueReturn
                   ? ControlFlowOutcomes{.inlineReturns = true}
                   : ControlFlowOutcomes{.scriptTerminates = true};
    }

    if (const auto* blockNode =
            dynamic_cast<const BlockNode*>(statement)) {
        return sequenceControlFlowOutcomes(blockNode->statements);
    }

    if (const auto* ifNode = dynamic_cast<const IfNode*>(statement)) {
        const ControlFlowOutcomes thenOutcomes =
            controlFlowOutcomes(ifNode->thenBranch.get());
        const ControlFlowOutcomes elseOutcomes = ifNode->elseBranch
                                                     ? controlFlowOutcomes(
                                                           ifNode->elseBranch.get()
                                                       )
                                                     : ControlFlowOutcomes{
                                                           .fallsThrough = true
                                                       };
        return {
            .fallsThrough = thenOutcomes.fallsThrough ||
                            elseOutcomes.fallsThrough,
            .inlineReturns = thenOutcomes.inlineReturns ||
                             elseOutcomes.inlineReturns,
            .scriptTerminates = thenOutcomes.scriptTerminates ||
                                elseOutcomes.scriptTerminates,
        };
    }

    if (const auto* forNode = dynamic_cast<const ForNode*>(statement)) {
        // Range cardinality can depend on outer-loop fixed bindings and is no
        // longer cached on the shared AST. Context-free analysis must retain
        // the zero-iteration path while recording outcomes the body may
        // produce when the loop executes.
        const ControlFlowOutcomes body =
            controlFlowOutcomes(forNode->body.get());
        return {
            .fallsThrough = true,
            .inlineReturns = body.inlineReturns,
            .scriptTerminates = body.scriptTerminates,
        };
    }

    return {.fallsThrough = true};
}

inline bool reachesContinuation(const StmtNode* statement)
{
    return controlFlowOutcomes(statement).fallsThrough;
}

// Compatibility predicate used by post-statement cleanup planning. In the
// Interpreter lowering model both uppercase script termination and lowercase
// inline-function return prevent a following statement from executing.
inline bool statementAlwaysTerminates(const StmtNode* statement)
{
    return !controlFlowOutcomes(statement).fallsThrough;
}

} // namespace compiler_flow

#endif // CONTROL_FLOW_ANALYSIS_H

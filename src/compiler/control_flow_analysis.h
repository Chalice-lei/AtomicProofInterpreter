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
        if (forNode->getStaticIterations().empty()) {
            return {.fallsThrough = true};
        }

        ControlFlowOutcomes result{.fallsThrough = true};
        const ControlFlowOutcomes body =
            controlFlowOutcomes(forNode->body.get());
        for (size_t i = 0; i < forNode->getStaticIterations().size(); ++i) {
            if (!result.fallsThrough) {
                break;
            }
            result.fallsThrough = body.fallsThrough;
            result.inlineReturns = result.inlineReturns || body.inlineReturns;
            result.scriptTerminates =
                result.scriptTerminates || body.scriptTerminates;
        }
        return result;
    }

    return {.fallsThrough = true};
}

inline bool reachesContinuation(const StmtNode* statement)
{
    return controlFlowOutcomes(statement).fallsThrough;
}

} // namespace compiler_flow

#endif // CONTROL_FLOW_ANALYSIS_H

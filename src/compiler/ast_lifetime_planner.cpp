#include "ast_lifetime_planner.h"

#include <algorithm>
#include <optional>
#include <set>
#include <utility>

namespace apc::compiler
{
namespace
{

const std::vector<AstKillSuggestion> kNoAstKillSuggestions;

struct BindingState
{
    std::string name;
    BindingId binding;
    ValueId value;
    AtomId atom;
    ValueContext context;
};

using LexicalScope = std::map<std::string, BindingState>;
using ScopeStack = std::vector<LexicalScope>;

struct ReferenceDescription
{
    std::string rootName;
    std::vector<const ExprNode*> indices;
    bool dynamicIndex{false};
};

class PlannerBuilder final
{
public:
    PlannerBuilder(
        const FunctionNode& function,
        const ContractNode* contract
    )
        : m_function(function), m_contract(contract)
    {
        if (m_contract) {
            for (const auto& member : m_contract->members) {
                if (const auto* candidate =
                        dynamic_cast<const FunctionNode*>(member.get())) {
                    m_functions.emplace(candidate->name, candidate);
                }
            }
        }
    }

    AstLifetimePlan build()
    {
        const BasicBlockId entry = addBlock();
        m_plan.cfg.entry = entry;
        m_currentBlock = entry;
        m_context.inlineFrame = m_ids.nextInlineFrame();
        m_scopes.emplace_back();

        if (!m_function.block) {
            issue(
                AstLifetimePlannerIssueCode::MissingFunctionBody,
                &m_function,
                "cannot plan a function without a body"
            );
            finalize();
            return std::move(m_plan);
        }

        for (const auto& parameter : m_function.parameters) {
            LifetimeStatement ignoredDefinitions;
            declareFresh(
                parameter.name,
                nullptr,
                true,
                ignoredDefinitions,
                std::nullopt
            );
        }

        planBlock(*m_function.block, true);
        m_scopes.pop_back();
        finalize();
        return std::move(m_plan);
    }

private:
    BasicBlockId addBlock()
    {
        const BasicBlockId id = m_ids.nextBasicBlock();
        m_blockIndices.emplace(id, m_plan.cfg.blocks.size());
        m_plan.cfg.blocks.push_back(LifetimeBasicBlock{id, {}, {}});
        m_statementAnchors.emplace(id, std::vector<const StmtNode*>{});
        return id;
    }

    LifetimeBasicBlock* block(BasicBlockId id)
    {
        const auto found = m_blockIndices.find(id);
        if (found == m_blockIndices.end()) {
            return nullptr;
        }
        return &m_plan.cfg.blocks[found->second];
    }

    void appendStatement(
        const StmtNode* anchor,
        LifetimeStatement statement
    )
    {
        appendStatementTo(m_currentBlock, anchor, std::move(statement));
    }

    void appendStatementTo(
        BasicBlockId destination,
        const StmtNode* anchor,
        LifetimeStatement statement
    )
    {
        LifetimeBasicBlock* target = block(destination);
        auto anchorIt = m_statementAnchors.find(destination);
        if (!target || anchorIt == m_statementAnchors.end()) {
            issue(
                AstLifetimePlannerIssueCode::InternalControlFlowError,
                anchor,
                "attempted to append a statement to an unknown CFG block"
            );
            return;
        }
        target->statements.push_back(std::move(statement));
        anchorIt->second.push_back(anchor);
    }

    void addSuccessor(BasicBlockId from, BasicBlockId to)
    {
        LifetimeBasicBlock* source = block(from);
        if (!source || !block(to)) {
            issue(
                AstLifetimePlannerIssueCode::InternalControlFlowError,
                nullptr,
                "attempted to create an edge involving an unknown CFG block"
            );
            return;
        }
        if (std::find(source->successors.begin(), source->successors.end(), to)
            == source->successors.end()) {
            source->successors.push_back(to);
        }
    }

    void issue(
        AstLifetimePlannerIssueCode code,
        const ASTNode* node,
        std::string message
    )
    {
        m_plan.issues.push_back({code, node, std::move(message)});
    }

    BindingState* resolve(const std::string& name)
    {
        for (auto scope = m_scopes.rbegin(); scope != m_scopes.rend(); ++scope) {
            const auto found = scope->find(name);
            if (found != scope->end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    const BindingState* resolve(const std::string& name) const
    {
        for (auto scope = m_scopes.rbegin(); scope != m_scopes.rend(); ++scope) {
            const auto found = scope->find(name);
            if (found != scope->end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    LexicalScope* scopeContaining(const std::string& name)
    {
        for (auto scope = m_scopes.rbegin(); scope != m_scopes.rend(); ++scope) {
            if (scope->contains(name)) {
                return &*scope;
            }
        }
        return nullptr;
    }

    void recordValue(
        const BindingState& state,
        const ASTNode* definition,
        bool parameter,
        bool alias
    )
    {
        m_plan.valueRecords.push_back(
            {state.name,
             state.binding,
             state.value,
             state.atom,
             state.context,
             definition,
             parameter,
             alias}
        );
    }

    std::optional<BindingState> declareFresh(
        const std::string& name,
        const ASTNode* definition,
        bool parameter,
        LifetimeStatement& statement,
        std::optional<ValueId> aliasSource
    )
    {
        if (m_scopes.empty()) {
            issue(
                AstLifetimePlannerIssueCode::InternalControlFlowError,
                definition,
                "cannot declare a binding without a lexical scope"
            );
            return std::nullopt;
        }

        BindingState state{
            name,
            m_ids.nextBinding(),
            m_ids.nextValue(),
            {},
            m_context
        };

        std::optional<ValueRegistryError> error;
        bool alias = false;
        if (aliasSource.has_value()) {
            error = m_plan.values.registerAlias(
                state.binding,
                state.value,
                aliasSource.value(),
                state.context
            );
            if (!error.has_value()) {
                const auto atom = m_plan.values.atomOf(state.value);
                if (atom.has_value()) {
                    state.atom = atom.value();
                    alias = true;
                }
            }
        } else {
            state.atom = m_ids.nextAtom();
            error = m_plan.values.registerValue(
                ValueIdentity{
                    state.binding,
                    state.value,
                    state.atom,
                    state.context,
                    {}
                }
            );
        }

        if (error.has_value() || !state.atom.valid()) {
            issue(
                AstLifetimePlannerIssueCode::InvalidDefinition,
                definition,
                error.has_value() ? error->message
                                  : "definition has no storage atom"
            );
            return std::nullopt;
        }

        m_scopes.back()[name] = state;
        if (parameter) {
            m_plan.cfg.parameters.push_back(state.value);
        } else {
            statement.definitions.push_back({state.value});
        }
        recordValue(state, definition, parameter, alias);
        return state;
    }

    std::optional<BindingState> assignFresh(
        const std::string& name,
        const ASTNode& definition,
        LifetimeStatement& statement
    )
    {
        LexicalScope* owner = scopeContaining(name);
        if (!owner) {
            issue(
                AstLifetimePlannerIssueCode::UnknownAssignmentTarget,
                &definition,
                "assignment target '" + name + "' has no visible binding"
            );
            return std::nullopt;
        }

        BindingState& current = owner->at(name);
        statement.uses.emplace_back(
            current.value, AccessKind::LValue, OwnershipEffect::None
        );

        ValueContext definitionContext = current.context;
        if (m_context.loopIteration.valid()) {
            definitionContext.loopIteration = m_context.loopIteration;
        }

        BindingState replacement{
            current.name,
            current.binding,
            m_ids.nextValue(),
            m_ids.nextAtom(),
            definitionContext
        };
        const auto error = m_plan.values.registerValue(
            ValueIdentity{
                replacement.binding,
                replacement.value,
                replacement.atom,
                replacement.context,
                {}
            }
        );
        if (error.has_value()) {
            issue(
                AstLifetimePlannerIssueCode::InvalidDefinition,
                &definition,
                error->message
            );
            return std::nullopt;
        }

        current = replacement;
        statement.definitions.push_back({replacement.value});
        recordValue(replacement, &definition, false, false);
        return replacement;
    }

    void appendLoopDefinitionEscapes(
        const StmtNode* anchor,
        const std::vector<ValueId>& definitions
    )
    {
        if (m_loopDepth == 0 || definitions.empty()) {
            return;
        }
        LifetimeStatement escapes;
        for (ValueId value : definitions) {
            escapes.uses.emplace_back(
                value,
                AccessKind::Borrow,
                OwnershipEffect::Escape,
                EscapeKind::Explicit
            );
        }
        appendStatement(anchor, std::move(escapes));
    }

    static bool staticIndex(const ExprNode& expression)
    {
        const auto* literal = dynamic_cast<const LiteralNode*>(&expression);
        return literal && literal->type == LiteralNode::Type::Number;
    }

    static bool externalStorageRoot(const std::string& name)
    {
        return name == "self" || name == "BVM";
    }

    static bool describeReference(
        const ExprNode& expression,
        ReferenceDescription& description
    )
    {
        if (const auto* identifier =
                dynamic_cast<const IdentifierNode*>(&expression)) {
            description.rootName = identifier->name;
            return true;
        }
        if (const auto* field =
                dynamic_cast<const FieldAccessNode*>(&expression)) {
            return field->base && describeReference(*field->base, description);
        }
        if (const auto* index =
                dynamic_cast<const IndexAccessNode*>(&expression)) {
            if (!index->base || !index->index ||
                !describeReference(*index->base, description)) {
                return false;
            }
            description.indices.push_back(index->index.get());
            description.dynamicIndex =
                description.dynamicIndex || !staticIndex(*index->index);
            return true;
        }
        return false;
    }

    void addReferenceUse(
        const ExprNode& expression,
        LifetimeStatement& statement,
        AccessKind requestedAccess,
        OwnershipEffect requestedOwnership,
        EscapeKind requestedEscape
    )
    {
        ReferenceDescription reference;
        if (!describeReference(expression, reference)) {
            if (const auto* field =
                    dynamic_cast<const FieldAccessNode*>(&expression)) {
                if (field->base) {
                    analyzeExpression(
                        *field->base,
                        statement,
                        requestedAccess,
                        requestedOwnership,
                        requestedEscape
                    );
                }
            } else if (const auto* index =
                           dynamic_cast<const IndexAccessNode*>(&expression)) {
                if (index->base) {
                    analyzeExpression(
                        *index->base,
                        statement,
                        requestedAccess,
                        requestedOwnership,
                        requestedEscape
                    );
                }
                if (index->index) {
                    analyzeExpression(*index->index, statement);
                }
            }
            return;
        }

        for (const ExprNode* index : reference.indices) {
            analyzeExpression(
                *index,
                statement,
                AccessKind::Borrow,
                OwnershipEffect::None,
                EscapeKind::None
            );
        }

        const BindingState* state = resolve(reference.rootName);
        if (!state) {
            // `self`, `BVM` and already-resolved global constants are not local
            // stack bindings.  PreAnalysisVisitor has validated misspellings
            // before this planner runs, so leaving them untracked is safe.
            return;
        }

        if (reference.dynamicIndex) {
            statement.uses.emplace_back(
                state->value,
                requestedAccess,
                OwnershipEffect::Escape,
                EscapeKind::DynamicIndex
            );
            return;
        }
        statement.uses.emplace_back(
            state->value,
            requestedAccess,
            requestedOwnership,
            requestedEscape
        );
    }

    void analyzeCall(
        const CallNode& call,
        LifetimeStatement& statement
    )
    {
        auto analyzeArguments = [&](
                                    AccessKind access,
                                    OwnershipEffect ownership,
                                    EscapeKind escape) {
            for (const auto& argument : call.args) {
                if (argument) {
                    analyzeExpression(
                        *argument,
                        statement,
                        access,
                        ownership,
                        escape
                    );
                }
            }
        };

        if (call.isRangeCall || call.funcName == "Range" ||
            call.funcName == "Size") {
            analyzeArguments(
                AccessKind::Borrow,
                OwnershipEffect::None,
                EscapeKind::None
            );
            return;
        }
        if (call.funcName == "Delete") {
            analyzeArguments(
                AccessKind::LValue,
                OwnershipEffect::Consume,
                EscapeKind::None
            );
            return;
        }
        if (call.funcName == "Move") {
            analyzeArguments(
                AccessKind::LValue,
                OwnershipEffect::Relocate,
                EscapeKind::None
            );
            return;
        }
        if (call.funcName == "SetAlt" || call.funcName == "SetMain") {
            analyzeArguments(
                AccessKind::Borrow,
                OwnershipEffect::Escape,
                EscapeKind::AltStack
            );
            return;
        }
        if (call.funcName == "Keep") {
            analyzeArguments(
                AccessKind::Borrow,
                OwnershipEffect::Escape,
                EscapeKind::Explicit
            );
            return;
        }

        const auto function = m_functions.find(call.funcName);
        if (function != m_functions.end()) {
            InlineCallContext context;
            context.call = &call;
            context.callee = function->second;
            context.callSite = m_ids.nextCallSite();
            context.inlineFrame = m_ids.nextInlineFrame();
            m_plan.inlineCalls.emplace(&call, context);
        }

        // Unknown builtins and inline private/library functions can consume,
        // relocate, retain or export arguments.  Until physical lowering
        // reports a precise contract, all arguments escape.
        analyzeArguments(
            AccessKind::Borrow,
            OwnershipEffect::Escape,
            EscapeKind::Explicit
        );
    }

    void analyzeExpression(
        const ExprNode& expression,
        LifetimeStatement& statement,
        AccessKind requestedAccess = AccessKind::Borrow,
        OwnershipEffect requestedOwnership = OwnershipEffect::None,
        EscapeKind requestedEscape = EscapeKind::None
    )
    {
        if (dynamic_cast<const LiteralNode*>(&expression)) {
            return;
        }
        if (dynamic_cast<const IdentifierNode*>(&expression) ||
            dynamic_cast<const FieldAccessNode*>(&expression) ||
            dynamic_cast<const IndexAccessNode*>(&expression)) {
            addReferenceUse(
                expression,
                statement,
                requestedAccess,
                requestedOwnership,
                requestedEscape
            );
            return;
        }
        if (const auto* call = dynamic_cast<const CallNode*>(&expression)) {
            analyzeCall(*call, statement);
            return;
        }
        if (const auto* method =
                dynamic_cast<const MethodCallNode*>(&expression)) {
            if (method->object) {
                analyzeExpression(
                    *method->object,
                    statement,
                    AccessKind::Borrow,
                    OwnershipEffect::Escape,
                    EscapeKind::Explicit
                );
            }
            for (const auto& argument : method->args) {
                if (argument) {
                    analyzeExpression(
                        *argument,
                        statement,
                        AccessKind::Borrow,
                        OwnershipEffect::Escape,
                        EscapeKind::Explicit
                    );
                }
            }
            return;
        }
        if (const auto* operation = dynamic_cast<const OpNode*>(&expression)) {
            if (operation->lhs) {
                analyzeExpression(
                    *operation->lhs,
                    statement,
                    requestedAccess,
                    requestedOwnership,
                    requestedEscape
                );
            }
            if (operation->rhs) {
                analyzeExpression(
                    *operation->rhs,
                    statement,
                    requestedAccess,
                    requestedOwnership,
                    requestedEscape
                );
            }
            return;
        }
        if (const auto* array =
                dynamic_cast<const ArrayDefNode*>(&expression)) {
            for (const auto& element : array->elements) {
                if (element) {
                    analyzeExpression(
                        *element,
                        statement,
                        requestedAccess,
                        requestedOwnership,
                        requestedEscape
                    );
                }
            }
            return;
        }
        if (const auto* brace =
                dynamic_cast<const BraceExprNode*>(&expression)) {
            for (const auto& element : brace->elements) {
                if (element) {
                    analyzeExpression(
                        *element,
                        statement,
                        requestedAccess,
                        requestedOwnership,
                        requestedEscape
                    );
                }
            }
        }
    }

    std::optional<ValueId> directAliasSource(const ExprNode* expression) const
    {
        const auto* identifier =
            dynamic_cast<const IdentifierNode*>(expression);
        if (!identifier) {
            return std::nullopt;
        }
        const BindingState* source = resolve(identifier->name);
        return source ? std::optional<ValueId>(source->value) : std::nullopt;
    }

    void escapeVisibleBindings(LifetimeStatement& statement)
    {
        std::set<BindingId> seen;
        for (auto scope = m_scopes.rbegin(); scope != m_scopes.rend(); ++scope) {
            for (const auto& [name, state] : *scope) {
                (void)name;
                if (seen.insert(state.binding).second) {
                    statement.uses.emplace_back(
                        state.value,
                        AccessKind::Borrow,
                        OwnershipEffect::Escape,
                        EscapeKind::Explicit
                    );
                }
            }
        }
    }

    bool planBlock(const BlockNode& blockNode, bool createScope)
    {
        if (createScope) {
            m_scopes.emplace_back();
        }
        bool reachesEnd = true;
        for (const auto& statement : blockNode.statements) {
            if (!statement || !reachesEnd) {
                continue;
            }
            reachesEnd = planStatement(*statement);
        }
        if (createScope) {
            m_scopes.pop_back();
        }
        return reachesEnd;
    }

    bool planScopedStatement(const StmtNode& statement)
    {
        m_scopes.emplace_back();
        const bool reachesEnd = planStatement(statement);
        m_scopes.pop_back();
        return reachesEnd;
    }

    bool planIf(const IfNode& statement)
    {
        LifetimeStatement condition;
        if (statement.condition) {
            analyzeExpression(*statement.condition, condition);
        }
        appendStatement(&statement, std::move(condition));

        const BasicBlockId conditionBlock = m_currentBlock;
        const BasicBlockId thenBlock = addBlock();
        const BasicBlockId elseBlock = addBlock();
        const BasicBlockId joinBlock = addBlock();
        addSuccessor(conditionBlock, thenBlock);
        addSuccessor(conditionBlock, elseBlock);

        const ScopeStack before = m_scopes;

        m_currentBlock = thenBlock;
        m_scopes = before;
        bool thenReaches = true;
        if (statement.thenBranch) {
            thenReaches = planScopedStatement(*statement.thenBranch);
        }
        const ScopeStack afterThen = m_scopes;
        const BasicBlockId thenEnd = m_currentBlock;
        if (thenReaches) {
            addSuccessor(thenEnd, joinBlock);
        }

        m_currentBlock = elseBlock;
        m_scopes = before;
        bool elseReaches = true;
        if (statement.elseBranch) {
            elseReaches = planScopedStatement(*statement.elseBranch);
        }
        const ScopeStack afterElse = m_scopes;
        const BasicBlockId elseEnd = m_currentBlock;
        if (elseReaches) {
            addSuccessor(elseEnd, joinBlock);
        }

        m_currentBlock = joinBlock;
        if (!thenReaches && !elseReaches) {
            m_scopes = before;
            return false;
        }
        if (thenReaches && !elseReaches) {
            m_scopes = afterThen;
            return true;
        }
        if (!thenReaches && elseReaches) {
            m_scopes = afterElse;
            return true;
        }

        m_scopes = before;
        LifetimeStatement merges;
        LifetimeStatement mergeEscapes;
        LifetimeStatement thenEscapes;
        LifetimeStatement elseEscapes;
        for (size_t scopeIndex = 0; scopeIndex < before.size(); ++scopeIndex) {
            for (const auto& [name, incoming] : before[scopeIndex]) {
                const BindingState& thenState = afterThen[scopeIndex].at(name);
                const BindingState& elseState = afterElse[scopeIndex].at(name);
                if (thenState.value == elseState.value) {
                    m_scopes[scopeIndex][name] = thenState;
                    continue;
                }

                // The core IR currently has no executable phi instruction.
                // Preserve safety by escaping both incoming versions and a
                // fresh summary value.  Lowering may later replace this with
                // an AtomSet-backed merge without changing AST identities.
                thenEscapes.uses.emplace_back(
                    thenState.value,
                    AccessKind::Borrow,
                    OwnershipEffect::Escape,
                    EscapeKind::Explicit
                );
                elseEscapes.uses.emplace_back(
                    elseState.value,
                    AccessKind::Borrow,
                    OwnershipEffect::Escape,
                    EscapeKind::Explicit
                );

                BindingState merged{
                    incoming.name,
                    incoming.binding,
                    m_ids.nextValue(),
                    m_ids.nextAtom(),
                    incoming.context
                };
                const auto error = m_plan.values.registerValue(
                    ValueIdentity{
                        merged.binding,
                        merged.value,
                        merged.atom,
                        merged.context,
                        {}
                    }
                );
                if (error.has_value()) {
                    issue(
                        AstLifetimePlannerIssueCode::InvalidDefinition,
                        &statement,
                        error->message
                    );
                    continue;
                }
                merges.definitions.push_back({merged.value});
                mergeEscapes.uses.emplace_back(
                    merged.value,
                    AccessKind::Borrow,
                    OwnershipEffect::Escape,
                    EscapeKind::Explicit
                );
                m_scopes[scopeIndex][name] = merged;
                recordValue(merged, &statement, false, false);
            }
        }
        if (!thenEscapes.uses.empty()) {
            appendStatementTo(
                thenEnd, &statement, std::move(thenEscapes)
            );
        }
        if (!elseEscapes.uses.empty()) {
            appendStatementTo(
                elseEnd, &statement, std::move(elseEscapes)
            );
        }
        if (!merges.definitions.empty() || !merges.uses.empty()) {
            appendStatement(&statement, std::move(merges));
        }
        if (!mergeEscapes.uses.empty()) {
            appendStatement(&statement, std::move(mergeEscapes));
        }
        return true;
    }

    bool planFor(const ForNode& statement)
    {
        const LoopIterationId iteration = m_ids.nextLoopIteration();
        m_plan.loops.emplace(
            &statement,
            LoopPlanningContext{&statement, iteration, true}
        );

        LifetimeStatement setup;
        if (statement.iterable) {
            analyzeExpression(*statement.iterable, setup);
        }
        // The backend can statically expand some Range loops, but this AST
        // layer intentionally represents a single summary iteration.  Values
        // crossing that backedge cannot be reclaimed from a one-pass last-use
        // observation.
        escapeVisibleBindings(setup);
        appendStatement(&statement, std::move(setup));

        const BasicBlockId setupBlock = m_currentBlock;
        const BasicBlockId header = addBlock();
        const BasicBlockId body = addBlock();
        const BasicBlockId exit = addBlock();
        addSuccessor(setupBlock, header);
        addSuccessor(header, body);
        addSuccessor(header, exit);

        const ScopeStack before = m_scopes;
        m_currentBlock = body;
        m_scopes = before;
        m_scopes.emplace_back();
        const ValueContext previousContext = m_context;
        m_context.loopIteration = iteration;
        ++m_loopDepth;

        LifetimeStatement targetDefinition;
        const auto target = declareFresh(
            statement.target,
            &statement,
            false,
            targetDefinition,
            std::nullopt
        );
        appendStatement(&statement, std::move(targetDefinition));
        if (target.has_value()) {
            appendLoopDefinitionEscapes(
                &statement, {target->value}
            );
        }
        const bool bodyReaches =
            statement.body ? planBlock(*statement.body, true) : true;

        --m_loopDepth;
        m_context = previousContext;
        m_scopes.pop_back();
        const BasicBlockId bodyEnd = m_currentBlock;
        if (bodyReaches) {
            addSuccessor(bodyEnd, header);
        }

        m_scopes = before;
        m_currentBlock = exit;
        return true;
    }

    bool planStatement(const StmtNode& statement)
    {
        if (const auto* blockStatement =
                dynamic_cast<const BlockNode*>(&statement)) {
            return planBlock(*blockStatement, true);
        }
        if (const auto* ifStatement = dynamic_cast<const IfNode*>(&statement)) {
            return planIf(*ifStatement);
        }
        if (const auto* forStatement =
                dynamic_cast<const ForNode*>(&statement)) {
            return planFor(*forStatement);
        }
        if (const auto* declaration =
                dynamic_cast<const VarDeclNode*>(&statement)) {
            LifetimeStatement lifetime;
            const std::optional<ValueId> alias =
                directAliasSource(declaration->initValue.get());
            if (declaration->initValue) {
                analyzeExpression(*declaration->initValue, lifetime);
            }
            const auto definition = declareFresh(
                declaration->name,
                declaration,
                false,
                lifetime,
                alias
            );
            appendStatement(declaration, std::move(lifetime));
            if (definition.has_value()) {
                appendLoopDefinitionEscapes(
                    declaration, {definition->value}
                );
            }
            return true;
        }
        if (const auto* declaration =
                dynamic_cast<const ArrayDeclNode*>(&statement)) {
            LifetimeStatement lifetime;
            if (declaration->sizeExpr) {
                analyzeExpression(*declaration->sizeExpr, lifetime);
            }
            if (declaration->initArray) {
                analyzeExpression(*declaration->initArray, lifetime);
            }
            const auto definition = declareFresh(
                declaration->name,
                declaration,
                false,
                lifetime,
                std::nullopt
            );
            appendStatement(declaration, std::move(lifetime));
            if (definition.has_value()) {
                appendLoopDefinitionEscapes(
                    declaration, {definition->value}
                );
            }
            return true;
        }
        if (const auto* assignment =
                dynamic_cast<const AssignNode*>(&statement)) {
            LifetimeStatement lifetime;
            if (assignment->value) {
                analyzeExpression(*assignment->value, lifetime);
            }
            ReferenceDescription target;
            if (!assignment->name ||
                !describeReference(*assignment->name, target)) {
                issue(
                    AstLifetimePlannerIssueCode::UnknownAssignmentTarget,
                    assignment,
                    "assignment target is not a tracked storage reference"
                );
            } else {
                if (target.dynamicIndex) {
                    addReferenceUse(
                        *assignment->name,
                        lifetime,
                        AccessKind::LValue,
                        OwnershipEffect::Escape,
                        EscapeKind::DynamicIndex
                    );
                } else if (!resolve(target.rootName) &&
                           externalStorageRoot(target.rootName)) {
                    // Contract/BVM fields are external placeholders rather
                    // than local stack bindings. Their RHS and index
                    // expressions are still tracked, but there is no local
                    // value definition to register.
                } else {
                    for (const ExprNode* index : target.indices) {
                        analyzeExpression(*index, lifetime);
                    }
                    const auto definition = assignFresh(
                        target.rootName, *assignment, lifetime
                    );
                    appendStatement(assignment, std::move(lifetime));
                    if (definition.has_value()) {
                        appendLoopDefinitionEscapes(
                            assignment, {definition->value}
                        );
                    }
                    return true;
                }
            }
            appendStatement(assignment, std::move(lifetime));
            return true;
        }
        if (const auto* destructure =
                dynamic_cast<const DestructureAssignNode*>(&statement)) {
            LifetimeStatement lifetime;
            if (destructure->value) {
                analyzeExpression(*destructure->value, lifetime);
            }
            std::vector<ValueId> definitions;
            for (const std::string& target : destructure->targets) {
                if (resolve(target)) {
                    const auto definition =
                        assignFresh(target, *destructure, lifetime);
                    if (definition.has_value()) {
                        definitions.push_back(definition->value);
                    }
                } else {
                    const auto definition = declareFresh(
                        target,
                        destructure,
                        false,
                        lifetime,
                        std::nullopt
                    );
                    if (definition.has_value()) {
                        definitions.push_back(definition->value);
                    }
                }
            }
            appendStatement(destructure, std::move(lifetime));
            appendLoopDefinitionEscapes(destructure, definitions);
            return true;
        }
        if (const auto* expressionStatement =
                dynamic_cast<const ExprStmtNode*>(&statement)) {
            LifetimeStatement lifetime;
            if (expressionStatement->expr) {
                analyzeExpression(*expressionStatement->expr, lifetime);
            }
            appendStatement(expressionStatement, std::move(lifetime));
            return true;
        }
        if (const auto* returnStatement =
                dynamic_cast<const ReturnNode*>(&statement)) {
            LifetimeStatement lifetime;
            if (returnStatement->expr) {
                analyzeExpression(
                    *returnStatement->expr,
                    lifetime,
                    AccessKind::Borrow,
                    OwnershipEffect::Escape,
                    EscapeKind::Explicit
                );
            }
            appendStatement(returnStatement, std::move(lifetime));
            return returnStatement->isValueReturn;
        }

        issue(
            AstLifetimePlannerIssueCode::InternalControlFlowError,
            &statement,
            "unsupported statement kind in AST lifetime planner"
        );
        return true;
    }

    const AstValueRecord* valueRecord(ValueId value) const
    {
        const auto found = std::find_if(
            m_plan.valueRecords.begin(),
            m_plan.valueRecords.end(),
            [&](const AstValueRecord& record) {
                return record.value == value;
            }
        );
        return found == m_plan.valueRecords.end() ? nullptr : &*found;
    }

    void buildSchedule()
    {
        if (!m_plan.issues.empty() || !m_plan.analysis.valid()) {
            return;
        }
        for (const KillSuggestion& kill : m_plan.analysis.kills) {
            if (!kill.requiresCleanup ||
                m_plan.analysis.escapedAtoms.contains(kill.atom) ||
                kill.boundary.afterStatement == 0) {
                continue;
            }
            const auto anchors =
                m_statementAnchors.find(kill.boundary.block);
            if (anchors == m_statementAnchors.end() ||
                kill.boundary.afterStatement > anchors->second.size()) {
                continue;
            }
            const StmtNode* statement =
                anchors->second[kill.boundary.afterStatement - 1];
            if (!statement) {
                continue;
            }

            AstKillSuggestion suggestion;
            suggestion.lifetime = kill;
            std::set<BindingId> seenBindings;
            for (ValueId value : kill.retiredValues) {
                const AstValueRecord* record = valueRecord(value);
                if (record && seenBindings.insert(record->binding).second) {
                    suggestion.bindings.push_back(
                        {record->name, record->binding, record->value}
                    );
                }
            }
            if (!suggestion.bindings.empty()) {
                m_plan.cleanupSchedule[statement].push_back(
                    std::move(suggestion)
                );
            }
        }
    }

    void finalize()
    {
        m_plan.analysis =
            ValueLifetimeAnalyzer::analyze(m_plan.cfg, m_plan.values);
        buildSchedule();
    }

    const FunctionNode& m_function;
    const ContractNode* m_contract{nullptr};
    std::map<std::string, const FunctionNode*> m_functions;
    StableValueIdFactory m_ids;
    AstLifetimePlan m_plan;
    std::map<BasicBlockId, size_t> m_blockIndices;
    std::map<BasicBlockId, std::vector<const StmtNode*>> m_statementAnchors;
    ScopeStack m_scopes;
    BasicBlockId m_currentBlock;
    ValueContext m_context;
    size_t m_loopDepth{0};
};

} // namespace

const std::vector<AstKillSuggestion>&
AstLifetimePlan::suggestionsAfter(const StmtNode* statement) const
{
    const auto found = cleanupSchedule.find(statement);
    return found == cleanupSchedule.end() ? kNoAstKillSuggestions
                                          : found->second;
}

std::vector<const AstValueRecord*>
AstLifetimePlan::recordsForName(const std::string& name) const
{
    std::vector<const AstValueRecord*> records;
    for (const auto& record : valueRecords) {
        if (record.name == name) {
            records.push_back(&record);
        }
    }
    return records;
}

AstLifetimePlan AstLifetimePlanner::planAfterConstantFolding(
    const FunctionNode& function
) const
{
    return PlannerBuilder(function, m_contract).build();
}

ContractLifetimePlans
planContractLifetimesAfterConstantFolding(const ContractNode& contract)
{
    ContractLifetimePlans plans;
    AstLifetimePlanner planner(&contract);
    for (const auto& member : contract.members) {
        if (const auto* function =
                dynamic_cast<const FunctionNode*>(member.get())) {
            plans.emplace(
                function, planner.planAfterConstantFolding(*function)
            );
        }
    }
    return plans;
}

} // namespace apc::compiler

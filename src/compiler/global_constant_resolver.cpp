#include "global_constant_resolver.h"

#include <stdexcept>

#include "../bytecode/bytecode_helper_fun.h"
#include "../bytecode/type_validator.h"
#include "../error/error_manager.h"

namespace
{

const IdentifierNode* assignmentRootIdentifier(const ExprNode* expression)
{
    if (!expression) {
        return nullptr;
    }
    if (const auto* identifier =
            dynamic_cast<const IdentifierNode*>(expression)) {
        return identifier;
    }
    if (const auto* field = dynamic_cast<const FieldAccessNode*>(expression)) {
        return assignmentRootIdentifier(field->base.get());
    }
    if (const auto* index = dynamic_cast<const IndexAccessNode*>(expression)) {
        return assignmentRootIdentifier(index->base.get());
    }
    return nullptr;
}

} // namespace

GlobalConstantResolver::Result
GlobalConstantResolver::resolve(ContractNode& contract)
{
    m_result = Result{};
    m_globalNames.clear();
    m_constants.clear();

    collectConstants(contract);
    validateFunctions(contract);

    // 失败时保留原 AST，避免诊断恢复看到只改写了一部分的树。
    if (!m_result.hasError) {
        rewriteFunctions(contract);
    }

    return m_result;
}

void GlobalConstantResolver::collectConstants(ContractNode& contract)
{
    std::unordered_set<std::string> memberNames;
    for (const auto& member : contract.members) {
        if (const auto* function =
                dynamic_cast<const FunctionNode*>(member.get())) {
            memberNames.insert(function->name);
        } else if (const auto* structure =
                       dynamic_cast<const StructDefNode*>(member.get())) {
            memberNames.insert(structure->name);
        }
    }

    std::unordered_set<std::string> seenNames;
    for (const auto& constant : contract.globalConstants) {
        if (!constant) {
            reportError(
                "null global constant declaration",
                contract,
                "Remove the invalid declaration and declare a literal constant"
            );
            continue;
        }

        if (constant->name.empty()) {
            reportError(
                "global constant name must not be empty",
                *constant,
                "Give the global constant a valid identifier"
            );
            continue;
        }

        const bool firstDeclaration = seenNames.insert(constant->name).second;
        if (!firstDeclaration) {
            reportError(
                "duplicate global constant '" + constant->name + "'",
                *constant,
                "Remove the duplicate declaration or use a different name"
            );
            continue;
        }
        m_globalNames.insert(constant->name);

        if (memberNames.find(constant->name) != memberNames.end()) {
            reportError(
                "global constant '" + constant->name +
                    "' conflicts with a function or struct name",
                *constant,
                "Use a name that is unique among contract-level declarations"
            );
        }

        if (!constant->initializer) {
            reportError(
                "global constant '" + constant->name +
                    "' requires a literal initializer",
                *constant,
                "Initialize the constant with a string or number literal"
            );
            continue;
        }

        const auto* literal =
            dynamic_cast<const LiteralNode*>(constant->initializer.get());
        if (!literal) {
            reportError(
                "global constant '" + constant->name +
                    "' initializer must be a literal",
                *constant,
                "Replace the initializer with a direct literal value"
            );
            continue;
        }

        std::string validationError;
        if (!validateInitializer(*literal, validationError)) {
            reportError(
                "invalid initializer for global constant '" + constant->name +
                    "': " + validationError,
                *literal,
                "Use a valid scalar literal that can be emitted by the compiler"
            );
            continue;
        }

        m_constants.emplace(constant->name, literal);
    }
}

void GlobalConstantResolver::validateFunctions(const ContractNode& contract)
{
    // LibraryMerger 在 resolver 之前已将导入的函数合并到 members，
    // 因此合约和库函数共用同一套限制和替换规则。
    for (const auto& member : contract.members) {
        if (const auto* function =
                dynamic_cast<const FunctionNode*>(member.get())) {
            validateFunction(*function);
        }
    }
}

void GlobalConstantResolver::validateFunction(const FunctionNode& function)
{
    for (const auto& parameter : function.parameters) {
        checkForbiddenBinding(parameter.name, function, "function parameter");
    }

    validateStatement(function.block.get());
}

void GlobalConstantResolver::validateStatement(const StmtNode* statement)
{
    if (!statement) {
        return;
    }

    if (const auto* block = dynamic_cast<const BlockNode*>(statement)) {
        for (const auto& child : block->statements) {
            validateStatement(child.get());
        }
        return;
    }

    if (const auto* conditional = dynamic_cast<const IfNode*>(statement)) {
        validateStatement(conditional->thenBranch.get());
        validateStatement(conditional->elseBranch.get());
        return;
    }

    if (const auto* loop = dynamic_cast<const ForNode*>(statement)) {
        checkForbiddenBinding(loop->target, *loop, "for-loop target");
        validateStatement(loop->body.get());
        return;
    }

    if (const auto* declaration =
            dynamic_cast<const VarDeclNode*>(statement)) {
        checkForbiddenBinding(
            declaration->name, *declaration, "local variable"
        );
        return;
    }

    if (const auto* declaration =
            dynamic_cast<const ArrayDeclNode*>(statement)) {
        checkForbiddenBinding(declaration->name, *declaration, "local array");
        return;
    }

    if (const auto* assignment = dynamic_cast<const AssignNode*>(statement)) {
        if (const auto* identifier =
                assignmentRootIdentifier(assignment->name.get())) {
            checkForbiddenBinding(
                identifier->name, *identifier, "assignment target"
            );
        }
        return;
    }

    if (const auto* destructure =
            dynamic_cast<const DestructureAssignNode*>(statement)) {
        for (const auto& target : destructure->targets) {
            checkForbiddenBinding(target, *destructure, "destructure target");
        }
    }
}

void GlobalConstantResolver::rewriteFunctions(ContractNode& contract)
{
    for (auto& member : contract.members) {
        if (auto* function = dynamic_cast<FunctionNode*>(member.get())) {
            rewriteFunction(*function);
        }
    }
}

void GlobalConstantResolver::rewriteFunction(FunctionNode& function)
{
    rewriteStatement(function.block.get());
}

void GlobalConstantResolver::rewriteStatement(StmtNode* statement)
{
    if (!statement) {
        return;
    }

    if (auto* block = dynamic_cast<BlockNode*>(statement)) {
        for (auto& child : block->statements) {
            rewriteStatement(child.get());
        }
        return;
    }

    if (auto* conditional = dynamic_cast<IfNode*>(statement)) {
        rewriteExpression(conditional->condition);
        rewriteStatement(conditional->thenBranch.get());
        rewriteStatement(conditional->elseBranch.get());
        return;
    }

    if (auto* loop = dynamic_cast<ForNode*>(statement)) {
        rewriteExpression(loop->iterable);
        rewriteStatement(loop->body.get());
        return;
    }

    if (auto* assignment = dynamic_cast<AssignNode*>(statement)) {
        rewriteAssignmentTarget(assignment->name);
        rewriteExpression(assignment->value);
        return;
    }

    if (auto* expressionStatement = dynamic_cast<ExprStmtNode*>(statement)) {
        rewriteExpression(expressionStatement->expr);
        return;
    }

    if (auto* returnStatement = dynamic_cast<ReturnNode*>(statement)) {
        rewriteExpression(returnStatement->expr);
        return;
    }

    if (auto* declaration = dynamic_cast<VarDeclNode*>(statement)) {
        rewriteExpression(declaration->initValue);
        return;
    }

    if (auto* declaration = dynamic_cast<ArrayDeclNode*>(statement)) {
        rewriteExpression(declaration->sizeExpr);
        rewriteArrayDefinition(declaration->initArray.get());
        return;
    }

    if (auto* destructure =
            dynamic_cast<DestructureAssignNode*>(statement)) {
        rewriteExpression(destructure->value);
    }
}

void GlobalConstantResolver::rewriteExpression(
    std::unique_ptr<ExprNode>& expression
)
{
    if (!expression) {
        return;
    }

    if (const auto* identifier =
            dynamic_cast<const IdentifierNode*>(expression.get())) {
        const auto constant = m_constants.find(identifier->name);
        if (constant != m_constants.end()) {
            expression = cloneLiteralAtUse(*constant->second, *identifier);
            ++m_result.resolvedReferences;
        }
        return;
    }

    if (auto* operation = dynamic_cast<OpNode*>(expression.get())) {
        rewriteExpression(operation->lhs);
        rewriteExpression(operation->rhs);
        return;
    }

    if (auto* call = dynamic_cast<CallNode*>(expression.get())) {
        for (auto& argument : call->args) {
            rewriteExpression(argument);
        }
        return;
    }

    if (auto* method = dynamic_cast<MethodCallNode*>(expression.get())) {
        rewriteExpression(method->object);
        for (auto& argument : method->args) {
            rewriteExpression(argument);
        }
        return;
    }

    if (auto* field = dynamic_cast<FieldAccessNode*>(expression.get())) {
        rewriteExpression(field->base);
        return;
    }

    if (auto* index = dynamic_cast<IndexAccessNode*>(expression.get())) {
        rewriteExpression(index->base);
        rewriteExpression(index->index);
        return;
    }

    if (auto* brace = dynamic_cast<BraceExprNode*>(expression.get())) {
        for (auto& element : brace->elements) {
            rewriteExpression(element);
        }
        return;
    }

    if (auto* array = dynamic_cast<ArrayDefNode*>(expression.get())) {
        rewriteArrayDefinition(array);
    }
}

void GlobalConstantResolver::rewriteAssignmentTarget(
    std::unique_ptr<ExprNode>& expression
)
{
    if (!expression) {
        return;
    }

    // lvalue 根是存储位置而不是右值引用；仅计算索引仍是值上下文。
    if (auto* field = dynamic_cast<FieldAccessNode*>(expression.get())) {
        rewriteAssignmentTarget(field->base);
        return;
    }

    if (auto* index = dynamic_cast<IndexAccessNode*>(expression.get())) {
        rewriteAssignmentTarget(index->base);
        rewriteExpression(index->index);
    }
}

void GlobalConstantResolver::rewriteArrayDefinition(ArrayDefNode* array)
{
    if (!array) {
        return;
    }

    for (auto& element : array->elements) {
        rewriteExpression(element);
    }
}

bool GlobalConstantResolver::validateInitializer(
    const LiteralNode& literal,
    std::string& reason
)
{
    switch (literal.type) {
        case LiteralNode::Type::Number: {
            try {
                size_t parsedCharacters = 0;
                (void)std::stoll(literal.value, &parsedCharacters, 10);
                if (parsedCharacters != literal.value.size()) {
                    reason = "invalid integer literal '" + literal.value + "'";
                    return false;
                }
            } catch (const std::exception&) {
                reason = "integer literal out of range: '" + literal.value +
                         "'";
                return false;
            }
            return true;
        }
        case LiteralNode::Type::String: {
            const std::string scriptHex = tbc::stringToScriptHex(literal.value);
            if (!tbc::TypeValidator::validateSize(
                    tbc::BytecodeType::String, scriptHex.length() / 2
                )) {
                reason = "string literal exceeds the supported size";
                return false;
            }
            return true;
        }
        case LiteralNode::Type::Hex: {
            if (!tbc::TypeValidator::isValidHex(literal.value)) {
                reason = "invalid hexadecimal literal '" + literal.value + "'";
                return false;
            }
            const size_t dataSize =
                tbc::TypeValidator::getHexDataSize(literal.value);
            if (!tbc::TypeValidator::validateSize(
                    tbc::BytecodeType::Hex, dataSize
                )) {
                reason = "hexadecimal literal exceeds the supported size";
                return false;
            }
            return true;
        }
        case LiteralNode::Type::Addr: {
            const std::string pubkeyHash = tbc::decodeP2PKHAddress(literal.value);
            if (pubkeyHash.size() != 40) {
                reason = "invalid P2PKH address literal '" + literal.value +
                         "'";
                return false;
            }
            return true;
        }
        default:
            reason = "unsupported scalar literal type";
            return false;
    }
}

bool GlobalConstantResolver::isGlobal(const std::string& name) const
{
    return m_globalNames.find(name) != m_globalNames.end();
}

void GlobalConstantResolver::checkForbiddenBinding(
    const std::string& name,
    const ASTNode& node,
    const std::string& bindingKind
)
{
    if (name.empty() || !isGlobal(name)) {
        return;
    }

    reportError(
        bindingKind + " '" + name +
            "' cannot shadow or assign global constant '" + name + "'",
        node,
        "Rename the local binding or use a different global constant name"
    );
}

void GlobalConstantResolver::reportError(
    const std::string& message,
    const ASTNode& node,
    const std::string& suggestion
)
{
    m_result.hasError = true;
    m_result.errors.push_back(message);
    ErrorManager::getInstance().semanticError(
        message, getNodeLocation(node), suggestion
    );
}

SourceLocation GlobalConstantResolver::getNodeLocation(const ASTNode& node)
{
    if (node.hasSourceLocation()) {
        return node.sourceLocation;
    }
    return SourceLocation("", node.pos.first, node.pos.second);
}

std::unique_ptr<LiteralNode> GlobalConstantResolver::cloneLiteralAtUse(
    const LiteralNode& literal,
    const IdentifierNode& use
)
{
    auto clone = std::make_unique<LiteralNode>(
        literal.type, literal.value, use.pos.first, use.pos.second
    );
    if (use.hasSourceLocation()) {
        clone->setSourceLocation(use.sourceLocation);
    }
    return clone;
}

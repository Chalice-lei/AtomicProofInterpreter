#include "collect_symbols_visitor.h"

#include "../error/error_manager.h"

void CollectSymbolsVisitor::checkUniqueness(ContractNode& node)
{
    LOG_INFO(
        "Starting uniqueness check for contract '",
        node.name,
        "' - functions and structs"
    );

    m_definedFunctions.clear();
    m_definedStructs.clear();
    m_structDefinitions.clear();

    visit(node);

    LOG_INFO(
        "Uniqueness check completed for contract '",
        node.name,
        "' - functions: ",
        m_definedFunctions.size(),
        ", structs: ",
        m_definedStructs.size()
    );
}

void CollectSymbolsVisitor::visit(ContractNode& node)
{
    for (const auto& member : node.members) {
        member->accept(*this);
    }
}

void CollectSymbolsVisitor::visit(FunctionNode& node)
{
    LOG_DEBUG("Checking function: ", node.name);
    checkFunctionUniqueness(node.name, node.pos.first, node.pos.second);

    if (node.block) {
        LOG_DEBUG("Checking variables in function body: ", node.name);
        node.block->accept(*this);
    }
}

void CollectSymbolsVisitor::visit(ConstructorNode& node)
{
    LOG_DEBUG("Checking constructor: __init__");
    checkFunctionUniqueness("__init__", node.pos.first, node.pos.second);

    if (node.block) {
        LOG_DEBUG("Checking variables in constructor body");
        node.block->accept(*this);
    }
}

void CollectSymbolsVisitor::visit(StructDefNode& node)
{
    LOG_DEBUG("Checking struct: ", node.name);
    checkStructUniqueness(node.name, node.pos.first, node.pos.second);

    m_structDefinitions[node.name] = node.fields;
    LOG_DEBUG(
        "Struct '", node.name, "' saved with ", node.fields.size(), " fields"
    );
}

void CollectSymbolsVisitor::checkFunctionUniqueness(
    const std::string& functionName,
    int line,
    int column
)
{
    if (m_definedFunctions.find(functionName) != m_definedFunctions.end()) {
        SourceLocation loc("", line, column);
        std::ostringstream oss;
        oss << "redefinition of function '" << functionName << "'";
        LOG_ERROR(
            "Semantic error at line ",
            line,
            ", column ",
            column,
            " - ",
            oss.str()
        );
        SEMANTIC_ERROR(
            oss.str(),
            loc,
            "Function names must be unique within a contract. Use a "
            "different name or remove the duplicate definition"
        );
        return; // 不抛异常, 让编译继续.
    }

    m_definedFunctions.insert(functionName);
    LOG_DEBUG("Function '", functionName, "' uniqueness check passed");
}

void CollectSymbolsVisitor::checkStructUniqueness(
    const std::string& structName,
    int line,
    int column
)
{
    if (m_definedStructs.find(structName) != m_definedStructs.end()) {
        SourceLocation loc("", line, column);
        std::ostringstream oss;
        oss << "redefinition of struct '" << structName << "'";
        LOG_ERROR(
            "Semantic error at line ",
            line,
            ", column ",
            column,
            " - ",
            oss.str()
        );
        SEMANTIC_ERROR(
            oss.str(),
            loc,
            "Struct names must be unique within a contract. Use a "
            "different name or remove the duplicate definition"
        );
        return; // 不抛异常, 让编译继续.
    }

    m_definedStructs.insert(structName);
    LOG_DEBUG("Struct '", structName, "' uniqueness check passed");
}

void CollectSymbolsVisitor::visit(BlockNode& node)
{
    LOG_DEBUG("Checking variables in block");
    for (const auto& statement : node.statements) {
        statement->accept(*this);
    }
}

void CollectSymbolsVisitor::visit(VarDeclNode& node)
{
    LOG_DEBUG("Checking variable declaration: ", node.name);
    checkVariableStructConflict(node.name, node.pos.first, node.pos.second);

    if (node.initValue) {
        node.initValue->accept(*this);
    }
}

void CollectSymbolsVisitor::visit(AssignNode& node)
{
    LOG_DEBUG("Checking assignment statement");

    std::string variableName = extractVariableName(node.name.get());
    if (!variableName.empty()) {
        checkVariableStructConflict(
            variableName, node.pos.first, node.pos.second
        );
    }

    if (node.value) {
        node.value->accept(*this);
    }
}

void CollectSymbolsVisitor::visit(IfNode& node)
{
    LOG_DEBUG("Checking if statement");

    if (node.condition) {
        node.condition->accept(*this);
    }
    if (node.thenBranch) {
        node.thenBranch->accept(*this);
    }
    if (node.elseBranch) {
        node.elseBranch->accept(*this);
    }
}

void CollectSymbolsVisitor::visit(ForNode& node)
{
    LOG_DEBUG("Checking for loop with target: ", node.target);

    if (!node.target.empty()) {
        checkVariableStructConflict(
            node.target, node.targetPos.first, node.targetPos.second
        );
    }

    if (node.iterable) {
        node.iterable->accept(*this);
    }

    if (node.body) {
        node.body->accept(*this);
    }
}

void CollectSymbolsVisitor::visit(ExprStmtNode& node)
{
    LOG_DEBUG("Checking expression statement");
    if (node.expr) {
        node.expr->accept(*this);
    }
}

void CollectSymbolsVisitor::visit(ReturnNode& node)
{
    LOG_DEBUG("Checking return statement");
    if (node.expr) {
        node.expr->accept(*this);
    }
}

void CollectSymbolsVisitor::checkVariableStructConflict(
    const std::string& variableName,
    int line,
    int column
)
{
    // 大小写敏感: 与任何结构体名完全一致即视为冲突.
    if (m_definedStructs.find(variableName) != m_definedStructs.end()) {
        SourceLocation loc("", line, column);
        std::ostringstream oss;
        oss << "variable name '" << variableName
            << "' conflicts with struct name";
        LOG_ERROR(
            "Semantic error at line ",
            line,
            ", column ",
            column,
            " - ",
            oss.str()
        );
        SEMANTIC_ERROR(
            oss.str(),
            loc,
            "Variable names cannot be identical to struct names. "
            "Use a different variable name"
        );
        return; // 不抛异常, 让编译继续.
    }

    LOG_DEBUG(
        "Variable '", variableName, "' has no conflict with struct names"
    );
}

std::string CollectSymbolsVisitor::extractVariableName(ExprNode* expr)
{
    if (!expr) {
        return "";
    }

    if (auto identNode = dynamic_cast<IdentifierNode*>(expr)) {
        return identNode->name;
    }

    // self.field 这类字段访问: 追溯到最里层 base 标识符.
    if (auto fieldNode = dynamic_cast<FieldAccessNode*>(expr)) {
        auto baseNode = fieldNode->base.get();
        while (
            (fieldNode = dynamic_cast<FieldAccessNode*>(fieldNode->base.get()))
        ) {
            baseNode = fieldNode->base.get();
        }

        if (auto identNode = dynamic_cast<IdentifierNode*>(baseNode)) {
            return identNode->name;
        }
        return "";
    }

    return "";
}

std::map<std::string, std::vector<std::pair<std::string, StructFieldType>>>
CollectSymbolsVisitor::getStructs()
{
    return m_structDefinitions;
}

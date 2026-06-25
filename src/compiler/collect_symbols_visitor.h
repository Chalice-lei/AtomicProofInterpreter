#ifndef COLLECT_SYMBOLS_VISITOR_H
#define COLLECT_SYMBOLS_VISITOR_H

#include <map>
#include <stdexcept>
#include <unordered_set>

#include "../ast/ast_visitor.h"
#include "../log/logger.h"

// 检查函数名和结构体名的唯一性.
class CollectSymbolsVisitor : public ASTVisitor
{
public:
    CollectSymbolsVisitor() = default;

    void checkUniqueness(ContractNode& node);

    void visit(ContractNode& node) override;
    void visit(FunctionNode& node) override;
    void visit(ConstructorNode& node) override;
    void visit(StructDefNode& node) override;

    void visit(BlockNode& node) override;
    void visit(VarDeclNode& node) override;
    void visit(AssignNode& node) override;
    void visit(IfNode& node) override;
    void visit(ForNode& node) override;
    void visit(ExprStmtNode& node) override;
    void visit(ReturnNode& node) override;

    void visit(LiteralNode& /*node*/) override
    {}
    void visit(IdentifierNode& /*node*/) override
    {}
    void visit(CallNode& /*node*/) override
    {}
    void visit(MethodCallNode& /*node*/) override
    {}
    void visit(OpNode& /*node*/) override
    {}
    void visit(FieldAccessNode& /*node*/) override
    {}
    void visit(IndexAccessNode& /*node*/) override
    {}
    void visit(ArrayDeclNode& /*node*/) override
    {}
    void visit(ArrayDefNode& /*node*/) override
    {}
    void visit(BraceExprNode& /*node*/) override
    {}
    void visit(DestructureAssignNode& /*node*/) override
    {}

    std::map<std::string, std::vector<std::pair<std::string, StructFieldType>>>
    getStructs();

private:
    void checkFunctionUniqueness(
        const std::string& functionName,
        int line,
        int column
    );

    void
    checkStructUniqueness(const std::string& structName, int line, int column);

    // 大小写敏感: 变量名 vs 结构体名冲突.
    void checkVariableStructConflict(
        const std::string& variableName,
        int line,
        int column
    );

    // 从 AssignNode 左值表达式提取变量名.
    std::string extractVariableName(ExprNode* expr);

private:
    std::unordered_set<std::string> m_definedFunctions;
    std::unordered_set<std::string> m_definedStructs;

    // 结构体名 -> 字段列表 (字段名, StructFieldType)
    std::map<std::string, std::vector<std::pair<std::string, StructFieldType>>>
        m_structDefinitions;
};

#endif // COLLECT_SYMBOLS_VISITOR_H

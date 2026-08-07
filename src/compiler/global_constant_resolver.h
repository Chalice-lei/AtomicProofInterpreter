#ifndef GLOBAL_CONSTANT_RESOLVER_H
#define GLOBAL_CONSTANT_RESOLVER_H

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../ast/ast.h"

// 在所有权分析、AST 解释和字节码生成前解析 Contract 级常量。
// 全局常量是纯前端绑定：每个右值使用被替换为新的 LiteralNode，
// 不在 ABI、调试变量或运行时栈中分配符号。
class GlobalConstantResolver
{
public:
    struct Result
    {
        bool hasError{false};
        size_t resolvedReferences{0};
        std::vector<std::string> errors;

        bool success() const
        {
            return !hasError;
        }
    };

    Result resolve(ContractNode& contract);

private:
    void collectConstants(ContractNode& contract);
    void validateFunctions(const ContractNode& contract);
    void validateFunction(const FunctionNode& function);
    void validateStatement(const StmtNode* statement);

    void rewriteFunctions(ContractNode& contract);
    void rewriteFunction(FunctionNode& function);
    void rewriteStatement(StmtNode* statement);
    void rewriteExpression(std::unique_ptr<ExprNode>& expression);
    void rewriteAssignmentTarget(std::unique_ptr<ExprNode>& expression);
    void rewriteArrayDefinition(ArrayDefNode* array);

    static bool validateInitializer(
        const LiteralNode& literal,
        std::string& reason
    );
    bool isGlobal(const std::string& name) const;
    void checkForbiddenBinding(
        const std::string& name,
        const ASTNode& node,
        const std::string& bindingKind
    );
    void reportError(
        const std::string& message,
        const ASTNode& node,
        const std::string& suggestion
    );
    static SourceLocation getNodeLocation(const ASTNode& node);
    static std::unique_ptr<LiteralNode> cloneLiteralAtUse(
        const LiteralNode& literal,
        const IdentifierNode& use
    );

private:
    Result m_result;
    std::unordered_set<std::string> m_globalNames;
    std::unordered_map<std::string, const LiteralNode*> m_constants;
};

#endif // GLOBAL_CONSTANT_RESOLVER_H

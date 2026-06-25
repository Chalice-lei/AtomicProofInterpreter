#ifndef CONSTANT_FOLDER_H
#define CONSTANT_FOLDER_H

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../ast/ast.h"

// 常量折叠 + 常量传播 + 死变量消除.
//
// 不继承 ASTVisitor 是因为折叠要替换父节点里的 unique_ptr, 而 visit 返回 void 没法做.
// 运行点: PreAnalysisVisitor 之后、AstToBytecodeVisitor 之前.
//
// 折叠: 一元 -/!, 二元 + - * / % == != < > <= >= && || , 两侧均为整数字面量.
// 传播: 名字若在函数内仅赋值一次 (循环体内的赋值按多次计), 该名字可被其初始字面量替换.
// 错误: / 0 、% 0 编译期报错; 整数溢出按 int64_t 回绕, 超界交给下游 validateType.
class ConstantFolder
{
public:
    struct Result
    {
        bool hasError = false;
        std::vector<std::string> errors;
    };

    ConstantFolder() = default;

    Result fold(ContractNode& contract);

private:
    // 若子树可折叠, 把 expr 替换成新的 LiteralNode.
    void foldExpr(std::unique_ptr<ExprNode>& expr);

    void foldStmt(std::unique_ptr<StmtNode>& stmt);
    void foldBlock(BlockNode& block);
    void foldFunction(FunctionNode& fn);

    // 尝试把 OpNode 折叠为 LiteralNode; 失败返回 nullptr.
    std::unique_ptr<LiteralNode> tryFoldOp(OpNode& op);
    std::unique_ptr<LiteralNode>
    tryFoldUnary(OpNode& op, const LiteralNode& rhs);
    std::unique_ptr<LiteralNode> tryFoldBinary(
        OpNode& op, const LiteralNode& lhs, const LiteralNode& rhs
    );

    void reportError(const ASTNode& node, const std::string& msg);

    // 循环体内的赋值额外 +1, 使其计数 > 1, 不会被当成单次赋值.
    void countAssignmentsInStmt(StmtNode* stmt);

    void countReadsInExpr(ExprNode* expr);
    void countReadsInStmt(StmtNode* stmt);

    // LHS 标识符基名; IndexAccess/FieldAccess 追溯到最里层 Identifier.
    // 无法识别返回空串 (保守: 视为未知目标).
    std::string extractAssignTargetName(ExprNode* lhs) const;

    std::unique_ptr<LiteralNode>
    cloneLiteralAt(const LiteralNode& lit, int32_t x, int32_t y) const;

    static std::optional<int64_t> literalAsInt(const LiteralNode& lit);

    void invalidateName(const std::string& name);

    Result m_result;

    // 仅当 == 1 时启用传播; foldFunction 入口重建.
    std::map<std::string, int> m_assignCounts;

    // 死变量消除使用; foldFunction 入口重建.
    std::map<std::string, int> m_readCounts;

    // 仅保留赋值次数 == 1 且 RHS 为字面量的标量/数组元素.
    std::map<std::string, std::unique_ptr<LiteralNode>> m_scalarConsts;
    std::map<std::string, std::vector<std::unique_ptr<LiteralNode>>>
        m_arrayConsts;
};

#endif // CONSTANT_FOLDER_H

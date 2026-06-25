#ifndef AST_INTERPRETER_H
#define AST_INTERPRETER_H

#include <string>
#include <unordered_map>
#include <vector>

#include "../ast/ast.h"
#include "environment.h"
#include "runtime_argument.h"
#include "runtime_context.h"
#include "runtime_value.h"

namespace apc_interpreter
{

struct ASTInterpretOptions
{
    std::string functionName;
    std::vector<std::string> functionNames;
    std::vector<RuntimeValue> args;
    std::vector<std::vector<RuntimeValue>> callArgs;
    RuntimeValue::Struct selfFields;
    RuntimeValue::Struct bvmFields;
};

struct ASTInterpretResult
{
    bool success = false;
    std::string functionName;
    std::vector<std::string> functionNames;
    std::string errorMessage;
    std::vector<RuntimeValue> returnValues;
    std::vector<std::vector<RuntimeValue>> returnValuesByFunction;
};

struct ASTReplExecutionResult
{
    bool success = false;
    std::string errorMessage;
    bool hasOutput = false;
    std::vector<RuntimeValue> outputValues;
};

class ASTInterpreter
{
public:
    ASTInterpretResult run(
        ContractNode& contract,
        const ASTInterpretOptions& options
    );

    RuntimeValue evalExpr(ExprNode& expr);
    void beginSession(
        ContractNode& contract,
        const ASTInterpretOptions& options = {}
    );
    void refreshSessionProgram(ContractNode& contract);
    ASTReplExecutionResult executeReplBlock(BlockNode& block);
    std::vector<std::string> globalNames() const;

private:
    enum class FlowKind {
        Normal,
        Return
    };

    struct ControlFlow
    {
        FlowKind kind = FlowKind::Normal;
        std::vector<RuntimeValue> values;
    };

    RuntimeValue evalLiteral(LiteralNode& node);
    RuntimeValue evalIdentifier(IdentifierNode& node);
    RuntimeValue evalOp(OpNode& node);
    RuntimeValue evalArray(ArrayDefNode& node);
    RuntimeValue evalBrace(BraceExprNode& node);
    RuntimeValue evalCall(CallNode& node);
    RuntimeValue evalMethodCall(MethodCallNode& node);
    RuntimeValue evalFieldAccess(FieldAccessNode& node);
    RuntimeValue evalIndexAccess(IndexAccessNode& node);
    RuntimeValue evalStackTransferCall(CallNode& node);
    RuntimeValue& resolveAssignable(ExprNode& target, SourceLocation location);
    void assignIdentifier(
        IdentifierNode& target,
        RuntimeValue value,
        SourceLocation location
    );

    ControlFlow execStmt(StmtNode& stmt);
    ControlFlow execBlock(BlockNode& block, bool createScope);
    ControlFlow execIf(IfNode& node);
    ControlFlow execFor(ForNode& node);
    ControlFlow execAssign(AssignNode& node);
    ControlFlow execVarDecl(VarDeclNode& node);
    ControlFlow execArrayDecl(ArrayDeclNode& node);
    ControlFlow execExprStmt(ExprStmtNode& node);
    ControlFlow execReturn(ReturnNode& node);
    ControlFlow execDestructureAssign(DestructureAssignNode& node);

    std::vector<RuntimeValue> callFunction(
        FunctionNode& function,
        const std::vector<RuntimeValue>& args,
        SourceLocation callLocation
    );
    std::vector<int64_t> evalRange(CallNode& node);
    RuntimeValue firstOrAggregateReturn(
        std::vector<RuntimeValue> values,
        const std::string& functionName
    ) const;
    RuntimeValue coerceDeclaredValue(
        RuntimeValue value,
        const std::string& declaredType,
        const std::vector<CompoundFieldInfo>* compoundFields,
        SourceLocation location
    );
    RuntimeValue unpackCompoundValue(
        const RuntimeValue& value,
        const std::vector<CompoundFieldInfo>& fields,
        SourceLocation location
    ) const;
    RuntimeValue defaultCompoundValue(
        const std::vector<CompoundFieldInfo>& fields
    ) const;
    RuntimeValue unpackStructValue(
        const RuntimeValue& value,
        StructDefNode& structDef,
        SourceLocation location
    ) const;
    RuntimeValue defaultStructValue(StructDefNode& structDef) const;
    RuntimeValue defaultValueForStructField(
        const StructFieldType& fieldType
    ) const;
    std::string chooseFunctionName(
        ContractNode& contract,
        const std::string& requestedName
    ) const;
    RuntimeValue defaultValueForType(const std::string& typeName) const;
    void ensureCurrentEnvironment() const;
    SourceLocation locationOf(const ASTNode& node) const;

    RuntimeContext m_context;
    std::shared_ptr<Environment> m_currentEnv;
    std::unordered_map<std::string, std::vector<CompoundFieldInfo>>
        m_compoundDeclarations;
};

} // namespace apc_interpreter

#endif // AST_INTERPRETER_H

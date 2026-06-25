#ifndef AST_TO_BYTECODE_VISITOR_H
#define AST_TO_BYTECODE_VISITOR_H

#include <map>
#include <vector>

#include "../ast/ast_visitor.h"
#include "../bytecode/bytecode_generator.h"
#include "../bytecode/bytecode_opcodes.h"
#include "../bytecode/scope.h"
#include "../error/error_manager.h"
#include "../log/logger.h"

#ifdef ENABLE_DEBUGGER
#include "../debugger/info/debug_info_generator.h"
#endif

struct FunctionSignature
{
    std::vector<std::string> inputTypes;
    std::vector<std::string> outputTypes;
};

// AST -> 字节码直接转换器.
class ASTToBytecodeVisitor : public ASTVisitor
{
public:
    ASTToBytecodeVisitor(
        tbc::BytecodeGenerator& generator,
        const std::string& sourceFile = ""
    )
        : m_generator(generator)
    {
        m_scopePtr = std::make_shared<tbc::Scope>();
#ifdef ENABLE_DEBUGGER
        if (!sourceFile.empty()) {
            m_sourceFile = sourceFile;
            m_debugInfoGen = std::make_shared<apc_debug::DebugInfoGenerator>(
                sourceFile
            );
            m_generator.setDebugInfoCallback(
                [this](
                    size_t pc,
                    const std::string& opcode,
                    const std::string& operand,
                    const apc_debug::SourceLocation& loc
                ) {
                    if (m_debugInfoGen) {
                        m_debugInfoGen
                            ->onEmitInstruction(pc, opcode, operand, loc);
                    }
                }
            );
        }
#endif
    }
    ~ASTToBytecodeVisitor() = default;

#ifdef ENABLE_DEBUGGER
    std::shared_ptr<apc_debug::DebugInfoGenerator> getDebugInfoGenerator() const
    {
        return m_debugInfoGen;
    }

    bool hasDebugInfoGenerator() const
    {
        return m_debugInfoGen != nullptr;
    }
#endif

    void setStructDefinitions(
        const std::map<
            std::string,
            std::vector<std::pair<std::string, StructFieldType>>>&
            structDefinitions
    )
    {
        m_structDefinitions = structDefinitions;
    }

    void visit(ContractNode& node) override;
    void visit(FunctionNode& node) override;
    void visit(ConstructorNode& /*node*/) override {};
    void visit(StructDefNode& /*node*/) override {};
    void visit(BlockNode& node) override;
    void visit(IfNode&) override;
    void visit(ForNode&) override;
    void visit(AssignNode&) override;
    void visit(ExprStmtNode& node) override;
    void visit(ReturnNode& node) override;
    void visit(VarDeclNode& node) override;

    void visitExpr(ExprNode& node)
    {
        node.accept(*this);
    }
    // 下列若干 AST 节点走自定义 visitXxx 实现.
    void visit(LiteralNode& node) override
    {
        visitLiteral(node);
    }
    void visit(IdentifierNode& node) override
    {
        visitIdentifier(node);
    }
    void visit(CallNode& node) override
    {
        visitCall(node);
    }
    void visit(MethodCallNode& node) override
    {
        visitMethodCall(node);
    }
    void visit(OpNode& node) override
    {
        visitOperator(node);
    }
    void visit(FieldAccessNode& node) override
    {
        visitFieldAccess(node);
    }
    void visit(IndexAccessNode& node) override
    {
        visitIndexAccess(node);
    }

    void visit(ArrayDeclNode& node) override
    {
        visitArrayDecl(node);
    }

    void visit(ArrayDefNode& node) override
    {
        visitArrayDef(node);
    }

    void visit(BraceExprNode& node) override
    {
        visitBraceExpr(node);
    }

    void visit(DestructureAssignNode& node) override
    {
        visitDestructureAssign(node);
    }

    void visitLiteral(LiteralNode& node);
    void visitIdentifier(IdentifierNode& node);
    void visitOperator(OpNode& node);
    void visitCall(CallNode& node);
    void visitMethodCall(MethodCallNode& node);
    void visitFieldAccess(FieldAccessNode& node);
    void visitIndexAccess(IndexAccessNode& node);
    void visitArrayDecl(ArrayDeclNode& node);
    void visitArrayDef(ArrayDefNode& node);
    void visitBraceExpr(BraceExprNode& node);
    void visitDestructureAssign(DestructureAssignNode& node);

private:
    // 内联展开私有函数调用.
    void privateFunctionResolution(
        FunctionNode& node,
        const std::vector<tbc::StackElement>& existingArgs
    );

    void cleanupFunctionParameters(const FunctionNode& node);

    void cleanupStructParameter(
        const std::string& paramName,
        const std::string& paramType
    );

    void cleanupBasicParameter(const std::string& paramName);

    // pos==1 -> OP_SWAP, pos==2 -> OP_ROT, 其余 -> N OP_ROLL (省 1 字节).
    void emitRoll(int64_t pos);

    // pos==0 -> OP_DUP, pos==1 -> OP_OVER, 其余 -> N OP_PICK (非破坏性).
    void emitPick(int64_t pos);
    tbc::BytecodeType inferLiteralType(const LiteralNode& node);
    void reportTypeError(
        const std::string& context,
        tbc::BytecodeType expectedType,
        const std::string& actualValue
    );

    std::optional<tbc::StackElement> processMethodCallObject(
        const MethodCallNode& node
    );

    void processGenericFunctionCall(
        const std::string& functionName,
        const std::vector<std::unique_ptr<ExprNode>>& args,
        const ExprNode& node,
        std::optional<tbc::StackElement> objectElement = std::nullopt
    );

    // 访问每个参数表达式, 从栈顶 pop 收集; 数量不匹配发警告,
    // pop 失败抛 std::runtime_error.
    std::vector<tbc::StackElement> processArguments(
        const std::vector<std::unique_ptr<ExprNode>>& args,
        int expectedArgCount,
        const std::string& functionName
    );

    void adjustStackToMatch(const std::vector<tbc::StackElement>& elementsVec);

    // 把参数依次搬到栈顶; 结构体参数递归处理所有子字段.
    void processArgsToTop(
        const std::vector<tbc::StackElement>& elementsVec,
        const std::vector<ParameterInfo>& paramInfos
    );

    void expandStructParameter(
        const std::string& paramName,
        const std::string& paramType,
        const std::map<
            std::string,
            std::vector<std::pair<std::string, StructFieldType>>>&
            structDefinitions
    );

    void expandArrayParameter(
        const std::string& paramName,
        const std::string& paramType,
        const std::map<
            std::string,
            std::vector<std::pair<std::string, StructFieldType>>>&
            structDefinitions,
        const SourceLocation& loc = SourceLocation("", 0, 0)
    );

    void registerExpandedParameterField(
        const std::string& fieldPath,
        const std::string& fieldType,
        const std::string& sourceStructType,
        const std::string& sourcePrefix,
        const std::map<
            std::string,
            std::vector<std::pair<std::string, StructFieldType>>>&
            structDefinitions
    );

    std::vector<std::pair<std::string, std::string>> getStructFieldsExpanded(
        const std::string& structName,
        const std::string& prefix,
        const std::map<
            std::string,
            std::vector<std::pair<std::string, StructFieldType>>>&
            structDefinitions
    );

    SourceLocation getNodeLocation(const ASTNode& node) const;

#ifdef ENABLE_DEBUGGER
    apc_debug::SourceLocation extractDebugLocation(
        const ASTNode& node,
        const std::string& sourceFile = ""
    ) const;

    // 在生成指令前为生成器设置当前位置.
    void setCurrentLocationForGenerator(const ASTNode& node);
#endif

    void validateRebinding(const std::string& varName, const AssignNode& node);

    // 结构体整体赋值 ({...}): 成功 true, 非命名结构体 false.
    bool tryHandleStructBraceAssignment(
        const AssignNode& node,
        const std::string& leftVarName,
        const BraceExprNode& braceExpr
    );

    // 对结构体单个叶子字段做普通变量赋值语义.
    // VarDeclNode 大括号初始化和 tryHandleStructBraceAssignment 共用.
    // fieldPath    : 目标字段完整路径 (如 "x.a.b"、"arr[0x01]")
    // expectedType : 扁平化字段类型 (用于类型检查和 uint64[N] 注册)
    // rhsVal       : 从大括号对应元素 pop 出的栈元素
    // locNode      : 错误报告位置
    void applyLeafFieldAssignment(
        const std::string& fieldPath,
        const std::string& expectedType,
        const tbc::StackElement& rhsVal,
        const ASTNode& locNode
    );

    // 数组整体赋值 ([...]/array def): 成功处理返回 true.
    bool tryHandleArrayDefAssignment(
        const AssignNode& node,
        std::string leftVarName,
        ArrayDefNode& arrayDef
    );

    bool checkExpressionContainsVariable(
        const ExprNode& expr,
        const std::string& varName
    ) const;

    bool isTypeCompatible(
        const std::string& declaredType,
        const tbc::StackElement& valueElement,
        bool isArrayType = false
    ) const;

    void validateDeclaredType(
        const std::string& context,
        const std::string& declaredType,
        const tbc::StackElement& valueElement,
        const ASTNode& node,
        bool isArrayType = false
    ) const;

    void keep(std::vector<tbc::StackElement>& elementVec);

    // 不进入新作用域执行语句块.
    void executeStatements(
        const std::vector<std::unique_ptr<StmtNode>>& statements
    );

    void registerWholeArrayElement(
        const std::string& name,
        size_t arraySize,
        size_t elementByteSize
    );
    void unregisterWholeArrayElement(const std::string& name);
    bool isWholeArrayElement(const std::string& name) const;
    std::optional<std::pair<size_t, size_t>> getWholeArrayInfo(
        const std::string& name
    ) const;
    void splitWholeArrayElement(const std::string& name);

    // a = b 首次绑定时把 b 的元数据搬到 a.
    // 覆盖整体数组、普通数组、复合类型、结构体变量; 已转移返回 true.
    bool transferCompositeIdentity(
        const std::string& oldName,
        const std::string& newName
    );

    struct StructArrayInfo
    {
        int64_t stride = 0; // 单元素扁平字段数
        int64_t basePos = -1; // 数组起始栈位置 (第 0 元素第一个字段)
        std::unordered_map<std::string, int64_t> fieldOffsets;
        std::string elementType;
    };

    bool ensureStructArrayInfo(
        const std::string& arrayBase,
        StructArrayInfo& outInfo,
        const SourceLocation& loc
    );

    bool isStructArrayFieldSubfield(const std::string& fieldPath) const;

    bool blockContainsReturn(const StmtNode* stmt) const;

    void findAllReturnNodes(
        const StmtNode* stmt,
        std::vector<ReturnNode*>& returns
    ) const;

private:
    tbc::BytecodeGenerator& m_generator;
    std::shared_ptr<tbc::Scope> m_scopePtr;
    std::vector<int> m_codeBlockLevel;

    std::map<std::string, std::vector<std::pair<std::string, StructFieldType>>>
        m_structDefinitions;

    std::map<std::string, FunctionNode*> m_privateFunctions;

    // 整体数组元素: 变量名 -> (arraySize, elementByteSize).
    std::map<std::string, std::pair<size_t, size_t>> m_wholeArrayElements;

    ReturnNode* m_lastReturnNode = nullptr;
    ReturnNode* m_currentReturnNode = nullptr;
    std::string m_currentFunctionReturnType;


#ifdef ENABLE_DEBUGGER
    std::shared_ptr<apc_debug::DebugInfoGenerator> m_debugInfoGen;
    std::string m_sourceFile;
#endif
};

#endif // AST_TO_BYTECODE_VISITOR_H

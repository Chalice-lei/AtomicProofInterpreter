//
// Created by Wayne on 25-2-11.
//

#ifndef AST_H
#define AST_H

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../bytecode/byt_data_types.h"
#include "../error/error_types.h"

class ASTVisitor;

// AST 节点抽象基类
class ASTNode
{
public:
    ASTNode() : pos{0, 0}
    {}
    ASTNode(int32_t x, int32_t y) : pos{x, y}
    {}
    virtual ~ASTNode() = default;
    void setParent(ASTNode* astPtr)
    {
        m_parentPtr = astPtr;
    };
    ASTNode* getParent()
    {
        return m_parentPtr;
    };
    virtual void accept(ASTVisitor& visitor) = 0;
    void setSourceLocation(const SourceLocation& loc)
    {
        sourceLocation = loc;
        m_hasSourceLocation = loc.line > 0 && !loc.filename.empty();
    }
    bool hasSourceLocation() const
    {
        return m_hasSourceLocation;
    }

public:
    std::pair<int32_t, int32_t> pos;
    SourceLocation sourceLocation;

private:
    ASTNode* m_parentPtr = nullptr;
    bool m_hasSourceLocation = false;
};

class ExprNode : public ASTNode
{
public:
    ExprNode() = default;
    ExprNode(int32_t x, int32_t y) : ASTNode(x, y)
    {}
    virtual ~ExprNode() = default;
};

class StmtNode : public ASTNode
{
public:
    StmtNode() = default;
    StmtNode(int32_t x, int32_t y) : ASTNode(x, y)
    {}
    virtual ~StmtNode() = default;
};

class ContractNode final : public ASTNode
{
public:
    explicit ContractNode(std::string name) : name(std::move(name))
    {}
    explicit ContractNode(std::string name, int32_t x, int32_t y)
        : ASTNode(x, y), name(std::move(name))
    {}
    void accept(ASTVisitor& visitor) override;

    std::string name;
    std::vector<std::unique_ptr<ASTNode>> members; // 函数与结构体定义

    // 临时字段：解析阶段收到的 import 库块，由 MergeLibrariesPass 平铺到
    // members 后清空，visitor 流水线看不到 LibraryNode。
    std::vector<std::unique_ptr<class LibraryNode>> libraries;
};

// import 的 .ct 库解析结果，由 MergeLibrariesPass 合并入 ContractNode。
class LibraryNode final : public ASTNode
{
public:
    explicit LibraryNode(std::string name) : name(std::move(name))
    {}
    LibraryNode(std::string name, int32_t x, int32_t y)
        : ASTNode(x, y), name(std::move(name))
    {}
    void accept(ASTVisitor& visitor) override;

    std::string name;
    std::vector<std::unique_ptr<ASTNode>> members;
};

class BlockNode;

struct ParameterInfo
{
    std::string name;
    std::string type;

    ParameterInfo(const std::string& n, const std::string& t) : name(n), type(t)
    {}
};

class FunctionNode : public ASTNode
{
public:
    FunctionNode(
        const std::string& name,
        const std::vector<ParameterInfo>& params = {},
        std::unique_ptr<BlockNode> body = {},
        std::string returnType = ""
    )
        : name(name), parameters(params), block(std::move(body)),
          returnType(std::move(returnType))
    {}

    FunctionNode(
        const std::string& name,
        int32_t x,
        int32_t y,
        const std::vector<ParameterInfo>& params = {},
        std::unique_ptr<BlockNode> body = {},
        std::string returnType = ""
    )
        : ASTNode(x, y), name(name), parameters(params), block(std::move(body)),
          returnType(std::move(returnType))
    {}

    void accept(ASTVisitor& visitor) override;

    std::string name;
    std::vector<ParameterInfo> parameters;
    std::unique_ptr<BlockNode> block;
    std::string returnType;
    // 来自 import 的库函数：作为合约 main 可调用的私有函数，不发布到 ABI；
    // 由 LibraryMerger 的 library 合并步骤设置。
    bool fromLibrary = false;
};

class ConstructorNode : public FunctionNode
{
public:
    ConstructorNode(
        const std::vector<ParameterInfo>& params = {},
        std::unique_ptr<BlockNode> body = {}
    )
        : FunctionNode("__init__", params, std::move(body))
    {}

    ConstructorNode(
        int32_t x,
        int32_t y,
        const std::vector<ParameterInfo>& params = {},
        std::unique_ptr<BlockNode> body = {}
    )
        : FunctionNode("__init__", x, y, params, std::move(body))
    {}

    void accept(ASTVisitor& visitor) override;
};

using tbc::CompoundFieldInfo;

// 结构体字段类型
struct StructFieldType
{
    std::string baseType;
    bool isArray;
    size_t arraySize;        // 非数组时为 0
    size_t elementByteSize;  // 数组元素字节数（uint64 = 8）

    bool isCompoundType;
    std::vector<CompoundFieldInfo> compoundFields;

    StructFieldType(const std::string& type)
        : baseType(type), isArray(false), arraySize(0), elementByteSize(0),
          isCompoundType(false)
    {}

    StructFieldType(const std::string& type, size_t size, size_t byteSize)
        : baseType(type), isArray(true), arraySize(size),
          elementByteSize(byteSize), isCompoundType(false)
    {}

    StructFieldType(
        const std::string& type,
        const std::vector<CompoundFieldInfo>& fields
    )
        : baseType(type), isArray(false), arraySize(0), elementByteSize(0),
          isCompoundType(true), compoundFields(fields)
    {}

    std::string getTypeString() const
    {
        if (isCompoundType)
            return "__compound__";
        if (!isArray)
            return baseType;
        return baseType + "[" + std::to_string(arraySize) + "]";
    }

    bool isSimpleType() const
    {
        return !isArray && !isCompoundType;
    }

    size_t getTotalByteSize() const
    {
        if (isCompoundType) {
            size_t total = 0;
            for (const auto& field : compoundFields) {
                total += field.byteSize;
            }
            return total;
        }
        return isArray ? arraySize * elementByteSize : elementByteSize;
    }
};

class StructDefNode : public ASTNode
{
public:
    StructDefNode(
        std::string name,
        std::vector<std::pair<std::string, StructFieldType>> fields
    )
        : name(std::move(name)), fields(std::move(fields))
    {}

    StructDefNode(
        std::string name,
        std::vector<std::pair<std::string, StructFieldType>> fields,
        int32_t x,
        int32_t y
    )
        : ASTNode(x, y), name(std::move(name)), fields(std::move(fields))
    {}

    void accept(ASTVisitor& visitor) override;

    std::string name;
    std::vector<std::pair<std::string, StructFieldType>> fields; // 名称:类型
};

/* ============== 表达式类型 ============== */
class LiteralNode final : public ExprNode
{
public:
    using Type = tbc::BytecodeType;
    LiteralNode(Type type, std::string value)
        : type(type), value(std::move(value))
    {}

    LiteralNode(Type type, std::string value, int32_t x, int32_t y)
        : ExprNode(x, y), type(type), value(std::move(value))
    {}

    void accept(ASTVisitor& visitor) override;

    Type type;
    std::string value;
};

class IdentifierNode final : public ExprNode
{
public:
    explicit IdentifierNode(std::string name) : name(std::move(name))
    {}

    IdentifierNode(std::string name, int32_t x, int32_t y)
        : ExprNode(x, y), name(std::move(name))
    {}

    void accept(ASTVisitor& visitor) override;

    std::string name;
};

class CallNode final : public ExprNode
{
public:
    CallNode(std::string funcName, std::vector<std::unique_ptr<ExprNode>> args)
        : funcName(std::move(funcName)), args(std::move(args))
    {}

    CallNode(
        std::string funcName,
        std::vector<std::unique_ptr<ExprNode>> args,
        int32_t x,
        int32_t y
    )
        : ExprNode(x, y), funcName(std::move(funcName)), args(std::move(args))
    {}

    void accept(ASTVisitor& visitor) override;

    std::string funcName;
    std::vector<std::unique_ptr<ExprNode>> args;
    bool isRangeCall{false};
};

// 方法调用（支持链式表达式）
class MethodCallNode final : public ExprNode
{
public:
    MethodCallNode(
        std::unique_ptr<ExprNode> object,
        std::string methodName,
        std::vector<std::unique_ptr<ExprNode>> args
    )
        : object(std::move(object)), methodName(std::move(methodName)),
          args(std::move(args))
    {}

    MethodCallNode(
        std::unique_ptr<ExprNode> object,
        std::string methodName,
        std::vector<std::unique_ptr<ExprNode>> args,
        int32_t x,
        int32_t y
    )
        : ExprNode(x, y), object(std::move(object)),
          methodName(std::move(methodName)), args(std::move(args))
    {}

    void accept(ASTVisitor& visitor) override;

    std::unique_ptr<ExprNode> object;
    std::string methodName;
    std::vector<std::unique_ptr<ExprNode>> args;
};

class OpNode final : public ExprNode
{
public:
    OpNode(
        std::string op,
        std::unique_ptr<ExprNode> lhs,
        std::unique_ptr<ExprNode> rhs
    )
        : op(std::move(op)), lhs(std::move(lhs)), rhs(std::move(rhs))
    {}

    OpNode(
        std::string op,
        std::unique_ptr<ExprNode> lhs,
        std::unique_ptr<ExprNode> rhs,
        int32_t x,
        int32_t y
    )
        : ExprNode(x, y), op(std::move(op)), lhs(std::move(lhs)),
          rhs(std::move(rhs))
    {}

    void accept(ASTVisitor& visitor) override;

    std::string op; // "+", "-", "==", "&&" 等
    std::unique_ptr<ExprNode> lhs;
    std::unique_ptr<ExprNode> rhs;
};

class FieldAccessNode final : public ExprNode
{
public:
    FieldAccessNode(std::unique_ptr<ExprNode> base, const std::string& field)
        : base(std::move(base)), field(field)
    {}

    FieldAccessNode(
        std::unique_ptr<ExprNode> base,
        const std::string& field,
        int32_t x,
        int32_t y
    )
        : ExprNode(x, y), base(std::move(base)), field(field)
    {}

    void accept(ASTVisitor& visitor) override;

    std::unique_ptr<ExprNode> base;
    std::string field;
};

class IndexAccessNode final : public ExprNode
{
public:
    IndexAccessNode(
        std::unique_ptr<ExprNode> base,
        std::unique_ptr<ExprNode> index
    )
        : base(std::move(base)), index(std::move(index))
    {}

    IndexAccessNode(
        std::unique_ptr<ExprNode> base,
        std::unique_ptr<ExprNode> index,
        int32_t x,
        int32_t y
    )
        : ExprNode(x, y), base(std::move(base)), index(std::move(index))
    {}

    void accept(ASTVisitor& visitor) override;

    std::unique_ptr<ExprNode> base;
    std::unique_ptr<ExprNode> index;
};

/* ============== 语句类型 ============== */
class BlockNode final : public StmtNode
{
public:
    BlockNode() = default;
    BlockNode(int32_t x, int32_t y) : StmtNode(x, y)
    {}

    void accept(ASTVisitor& visitor) override;

    std::vector<std::unique_ptr<StmtNode>> statements;
};

class IfNode final : public StmtNode
{
public:
    IfNode() = default;
    IfNode(int32_t x, int32_t y) : StmtNode(x, y)
    {}

    void accept(ASTVisitor& visitor) override;

    std::unique_ptr<ExprNode> condition;
    std::unique_ptr<StmtNode> thenBranch;
    std::unique_ptr<StmtNode> elseBranch; // 可空
};

class ForNode final : public StmtNode
{
public:
    ForNode() = default;
    ForNode(int32_t x, int32_t y) : StmtNode(x, y)
    {}

    void accept(ASTVisitor& visitor) override;

    std::string target;
    std::pair<int32_t, int32_t> targetPos{0, 0};
    std::unique_ptr<ExprNode> iterable;
    std::unique_ptr<BlockNode> body;

    void setStaticIterations(std::vector<int64_t> values)
    {
        iterationValues = std::move(values);
        haveStaticIterations = !iterationValues.empty();
    }
    const std::vector<int64_t>& getStaticIterations() const
    {
        return iterationValues;
    }
    bool hasStaticIterations() const
    {
        return haveStaticIterations;
    }

    void setInferredType(std::string type)
    {
        inferredType = std::move(type);
    }
    const std::string& getInferredType() const
    {
        return inferredType;
    }

private:
    std::vector<int64_t> iterationValues;
    bool haveStaticIterations{false};
    std::string inferredType{"int"};
};

class AssignNode final : public StmtNode
{
public:
    AssignNode(std::unique_ptr<ExprNode> name, std::unique_ptr<ExprNode> value)
        : name(std::move(name)), value(std::move(value))
    {}

    AssignNode(
        std::unique_ptr<ExprNode> name,
        std::unique_ptr<ExprNode> value,
        int32_t x,
        int32_t y
    )
        : StmtNode(x, y), name(std::move(name)), value(std::move(value))
    {}

    void accept(ASTVisitor& visitor) override;

    std::unique_ptr<ExprNode> name;
    std::unique_ptr<ExprNode> value;
};

class ExprStmtNode final : public StmtNode
{
public:
    explicit ExprStmtNode(std::unique_ptr<ExprNode> expr)
        : expr(std::move(expr))
    {}

    ExprStmtNode(std::unique_ptr<ExprNode> expr, int32_t x, int32_t y)
        : StmtNode(x, y), expr(std::move(expr))
    {}

    void accept(ASTVisitor& visitor) override;

    std::unique_ptr<ExprNode> expr;
};

class ReturnNode final : public StmtNode
{
public:
    // isValueReturn=true: 小写 return，返回值已在栈顶
    explicit ReturnNode(
        std::unique_ptr<ExprNode> expr,
        bool isValueReturn = false
    )
        : expr(std::move(expr)), isValueReturn(isValueReturn)
    {}

    ReturnNode(
        std::unique_ptr<ExprNode> expr,
        int32_t x,
        int32_t y,
        bool isValueReturn = false
    )
        : StmtNode(x, y), expr(std::move(expr)), isValueReturn(isValueReturn)
    {}

    void accept(ASTVisitor& visitor) override;

    std::unique_ptr<ExprNode> expr; // 可空
    bool isValueReturn = false; // 小写 return：仅标记返回已有值
};

// 数组字面量
class ArrayDefNode final : public ExprNode
{
public:
    ArrayDefNode(
        std::string elementType,
        std::vector<std::unique_ptr<ExprNode>> elements
    )
        : elementType(std::move(elementType)), elements(std::move(elements))
    {}

    ArrayDefNode(
        std::string elementType,
        std::vector<std::unique_ptr<ExprNode>> elements,
        int32_t x,
        int32_t y
    )
        : ExprNode(x, y), elementType(std::move(elementType)),
          elements(std::move(elements))
    {}

    void accept(ASTVisitor& visitor) override;

    std::string elementType; // "int", "st", "st2" 等
    std::vector<std::unique_ptr<ExprNode>> elements;

    size_t getSize() const
    {
        return elements.size();
    }
    bool isFixedSize() const
    {
        return !elements.empty();
    }
};

// 数组变量声明
class ArrayDeclNode final : public StmtNode
{
public:
    ArrayDeclNode(
        std::string name,
        std::string elementType,
        std::unique_ptr<ExprNode> sizeExpr = nullptr,
        std::unique_ptr<ArrayDefNode> initArray = nullptr
    )
        : name(std::move(name)), elementType(std::move(elementType)),
          sizeExpr(std::move(sizeExpr)), initArray(std::move(initArray))
    {}

    ArrayDeclNode(
        std::string name,
        std::string elementType,
        int32_t x,
        int32_t y,
        std::unique_ptr<ExprNode> sizeExpr = nullptr,
        std::unique_ptr<ArrayDefNode> initArray = nullptr
    )
        : StmtNode(x, y), name(std::move(name)),
          elementType(std::move(elementType)), sizeExpr(std::move(sizeExpr)),
          initArray(std::move(initArray))
    {}

    void accept(ASTVisitor& visitor) override;

    std::string name;
    std::string elementType;
    std::unique_ptr<ExprNode> sizeExpr;       // 可选
    std::unique_ptr<ArrayDefNode> initArray;  // 可选

    bool hasInitializer() const
    {
        return initArray != nullptr;
    }
    bool hasFixedSize() const
    {
        return sizeExpr != nullptr || hasInitializer();
    }
};

// {} 语法糖
class BraceExprNode final : public ExprNode
{
public:
    explicit BraceExprNode(std::vector<std::unique_ptr<ExprNode>> elements)
        : elements(std::move(elements))
    {}

    BraceExprNode(
        std::vector<std::unique_ptr<ExprNode>> elements,
        int32_t x,
        int32_t y
    )
        : ExprNode(x, y), elements(std::move(elements))
    {}

    void accept(ASTVisitor& visitor) override;

    std::vector<std::unique_ptr<ExprNode>> elements;
};

// 解构赋值：{a, b} = expr
class DestructureAssignNode final : public StmtNode
{
public:
    DestructureAssignNode(
        std::vector<std::string> targets,
        std::unique_ptr<ExprNode> value
    )
        : targets(std::move(targets)), value(std::move(value))
    {}

    DestructureAssignNode(
        std::vector<std::string> targets,
        std::unique_ptr<ExprNode> value,
        int32_t x,
        int32_t y
    )
        : StmtNode(x, y), targets(std::move(targets)), value(std::move(value))
    {}

    void accept(ASTVisitor& visitor) override;

    std::vector<std::string> targets;
    std::unique_ptr<ExprNode> value;
};

class VarDeclNode final : public StmtNode
{
public:
    VarDeclNode(
        std::string name,
        std::string type,
        std::unique_ptr<ExprNode> initValue = nullptr
    )
        : name(std::move(name)), type(std::move(type)),
          initValue(std::move(initValue)), isCompoundType(false)
    {}

    VarDeclNode(
        std::string name,
        std::string type,
        int32_t x,
        int32_t y,
        std::unique_ptr<ExprNode> initValue = nullptr
    )
        : StmtNode(x, y), name(std::move(name)), type(std::move(type)),
          initValue(std::move(initValue)), isCompoundType(false)
    {}

    void accept(ASTVisitor& visitor) override;

    std::string name;
    std::string type;
    std::unique_ptr<ExprNode> initValue; // 可空

    bool isCompoundType;
    std::vector<CompoundFieldInfo> compoundFields;
};

#endif // AST_H

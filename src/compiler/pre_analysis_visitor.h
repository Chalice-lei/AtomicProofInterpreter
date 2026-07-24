#ifndef PRE_ANALYSIS_VISITOR_H
#define PRE_ANALYSIS_VISITOR_H

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../ast/ast_visitor.h"
#include "../error/error_manager.h"

// 变量数据来源.
enum class DataSource {
    CONTRACT_MEMBER, // 合约成员 (self.*, <self.*>)
    CONSTANT_VALUE,  // 常量赋值 (x = "st", x = 10, x = 0x11)
    STACK_DATA,      // 栈上数据 (函数参数、op_function 生成的数据)
    BUILTIN_OBJECT   // 内置对象 (BVM, alt 等)
};

enum class VariableState {
    DECLARED,
    USED,
    CONSUMED // 所有权转移.
};

// Tracks where a binding currently lives. Ownership decisions for assignment
// must follow the backend's real stack operation, not merely the declared type.
enum class StorageResidency {
    UNBOUND,
    FIXED_VALUE,
    MAIN_STACK,
    ALT_STACK
};

struct VariableInfo
{
    std::string name;
    std::string type;
    DataSource source;
    VariableState state;
    SourceLocation declLocation;
    SourceLocation lastUseLocation;
    StorageResidency storage;
    bool wasBorrowed = false;

    // 数组元素所有权: true=拥有, false=已移动.
    std::vector<bool> elementOwnership;
    std::vector<StorageResidency> elementStorage;
    size_t elementStackSize = 1;

    // 字段路径 -> 该字段的所有权状态.
    std::map<std::string, VariableState> fieldOwnership;
    std::map<std::string, StorageResidency> fieldStorage;

    VariableInfo(
        const std::string& n,
        const std::string& t,
        DataSource src,
        const SourceLocation& loc,
        StorageResidency residency = StorageResidency::UNBOUND
    )
        : name(n), type(t), source(src), state(VariableState::DECLARED),
          declLocation(loc), storage(residency)
    {}

    // 数组类型重载.
    VariableInfo(
        const std::string& n,
        const std::string& t,
        DataSource src,
        const SourceLocation& loc,
        size_t arraySize,
        size_t elemStackSize = 1,
        StorageResidency residency = StorageResidency::UNBOUND
    )
        : name(n), type(t), source(src), state(VariableState::DECLARED),
          declLocation(loc), storage(residency),
          elementStackSize(elemStackSize)
    {
        elementOwnership.resize(arraySize, true);
        elementStorage.resize(arraySize, residency);
    }

    // 仅栈上数据有所有权概念.
    bool hasOwnership() const
    {
        return source == DataSource::STACK_DATA;
    }

    bool isArrayType() const
    {
        return !elementOwnership.empty();
    }

    size_t getArraySize() const
    {
        return elementOwnership.size();
    }

    bool isFullyConsumed() const
    {
        if (!isArrayType()) {
            return state == VariableState::CONSUMED;
        }
        return state == VariableState::CONSUMED || std::all_of(
            elementOwnership.begin(),
            elementOwnership.end(),
            [](bool owned) { return !owned; }
        );
    }

    bool isElementAvailable(size_t index) const
    {
        if (!isArrayType()) {
            return state != VariableState::CONSUMED;
        }
        return state != VariableState::CONSUMED &&
               index < elementOwnership.size() && elementOwnership[index];
    }

    size_t getConsumedElementCount() const
    {
        if (!isArrayType()) {
            return (state == VariableState::CONSUMED) ? 1 : 0;
        }
        if (state == VariableState::CONSUMED) {
            return elementOwnership.size();
        }
        return std::count(
            elementOwnership.begin(), elementOwnership.end(), false
        );
    }

    size_t getAvailableElementCount() const
    {
        if (!isArrayType()) {
            return (state != VariableState::CONSUMED) ? 1 : 0;
        }
        if (state == VariableState::CONSUMED) {
            return 0;
        }
        return std::count(
            elementOwnership.begin(), elementOwnership.end(), true
        );
    }

    // 字段无单独记录则回退到整体变量状态.
    bool isFieldConsumed(const std::string& fieldPath) const
    {
        auto it = fieldOwnership.find(fieldPath);
        if (it != fieldOwnership.end()) {
            return it->second == VariableState::CONSUMED;
        }
        return state == VariableState::CONSUMED;
    }

    void markFieldUsed(const std::string& fieldPath)
    {
        if (fieldOwnership.find(fieldPath) == fieldOwnership.end()) {
            fieldOwnership[fieldPath] = VariableState::USED;
        } else if (fieldOwnership[fieldPath] == VariableState::DECLARED) {
            fieldOwnership[fieldPath] = VariableState::USED;
        }
    }

    void markFieldConsumed(const std::string& fieldPath)
    {
        fieldOwnership[fieldPath] = VariableState::CONSUMED;
    }

    VariableState getFieldState(const std::string& fieldPath) const
    {
        auto it = fieldOwnership.find(fieldPath);
        if (it != fieldOwnership.end()) {
            return it->second;
        }
        return state;
    }

    StorageResidency getFieldStorage(const std::string& fieldPath) const
    {
        auto it = fieldStorage.find(fieldPath);
        return it == fieldStorage.end() ? storage : it->second;
    }

    StorageResidency getElementStorage(size_t index) const
    {
        return index < elementStorage.size() ? elementStorage[index]
                                             : StorageResidency::UNBOUND;
    }

    bool isInAltStack() const
    {
        return storage == StorageResidency::ALT_STACK;
    }

    void markInAltStack()
    {
        storage = StorageResidency::ALT_STACK;
    }

    void markNotInAltStack()
    {
        storage = StorageResidency::MAIN_STACK;
    }
};

// 前置分析访问器.
class PreAnalysisVisitor : public ASTVisitor
{
public:
    PreAnalysisVisitor();
    ~PreAnalysisVisitor() = default;

    bool analyze(ASTNode& root);

    void setAllowSubscopeAltstack(bool allow)
    {
        m_allowSubscopeAltstack = allow;
    }

    const std::vector<std::string>& getErrors() const
    {
        return m_errors;
    }
    const std::vector<std::string>& getWarnings() const
    {
        return m_warnings;
    }

    void visit(ContractNode& node) override;
    void visit(FunctionNode& node) override;
    void visit(ConstructorNode& /*node*/) override {};
    void visit(StructDefNode& node) override;
    void visit(BlockNode& node) override;
    void visit(IfNode& node) override;
    void visit(AssignNode& node) override;
    void visit(ExprStmtNode& node) override;
    void visit(ReturnNode& node) override;
    void visit(VarDeclNode& node) override;
    void visit(ForNode& node) override;
    void visit(LiteralNode& node) override;
    void visit(IdentifierNode& node) override;
    void visit(CallNode& node) override;
    void visit(MethodCallNode& node) override;
    void visit(OpNode& node) override;
    void visit(FieldAccessNode& node) override;
    void visit(IndexAccessNode& node) override;
    void visit(ArrayDeclNode& node) override;
    void visit(ArrayDefNode& node) override;
    void visit(BraceExprNode& /*node*/) override {};
    void visit(DestructureAssignNode& node) override;

private:
    DataSource
    classifyVariable(const std::string& name, ExprNode* initValue = nullptr);
    void declareVariable(
        const std::string& name,
        const std::string& type,
        const SourceLocation& location,
        ExprNode* initValue = nullptr,
        StorageResidency storage = StorageResidency::UNBOUND
    );
    void useVariable(const std::string& name, const SourceLocation& location);
    void borrowVariable(const std::string& name, const SourceLocation& location);
    void
    consumeVariable(const std::string& name, const SourceLocation& location);

    void useField(
        const std::string& varName,
        const std::string& fieldPath,
        const SourceLocation& location
    );
    void borrowField(
        const std::string& varName,
        const std::string& fieldPath,
        const SourceLocation& location
    );
    void consumeField(
        const std::string& varName,
        const std::string& fieldPath,
        const SourceLocation& location
    );
    void checkFieldOwnershipViolation(
        const std::string& varName,
        const std::string& fieldPath,
        const SourceLocation& location
    );

    void declareArrayVariable(
        const std::string& name,
        const std::string& elementType,
        const SourceLocation& location,
        size_t arraySize,
        size_t elementStackSize = 1,
        StorageResidency storage = StorageResidency::UNBOUND
    );
    void useArrayElement(
        const std::string& arrayName,
        size_t index,
        const SourceLocation& location
    );
    void consumeArrayElement(
        const std::string& arrayName,
        size_t index,
        const SourceLocation& location
    );
    void borrowArrayElement(IndexAccessNode& node);
    void consumeWholeArray(
        const std::string& arrayName,
        const SourceLocation& location
    );
    bool isArrayElementAvailable(const std::string& arrayName, size_t index)
        const;

    size_t calculateElementStackSize(const std::string& elementType) const;

    std::optional<size_t> calculateIndexValue(ExprNode* indexExpr) const;
    std::optional<std::pair<std::string, size_t>>
    parseFixedArrayType(const std::string& type) const;

    VariableInfo* findVariable(const std::string& name);

    void
    reportError(const std::string& message, const SourceLocation& location);
    void
    reportWarning(const std::string& message, const SourceLocation& location);

    void analyzeExpression(ExprNode& expr);
    void analyzeBorrowedExpression(ExprNode& expr);
    void analyzeExpressionForValueReturn(ExprNode& expr); // 小写 return: 只使用不消耗.
    void analyzeConditionalExpression(ExprNode& expr);
    StorageResidency classifyStorage(ExprNode* value);
    bool expressionHasMainStackSlot(ExprNode& expr);
    bool assignmentTargetHasMainStackSlot(ExprNode& target);
    void bindAssignmentTarget(
        ExprNode& target,
        StorageResidency storage,
        ExprNode* value
    );
    std::string getVariableFromExpr(ExprNode& expr);
    // 例: ctx.FTbyChange.Tape.LockingScript -> {"ctx", "FTbyChange.Tape.LockingScript"}
    std::pair<std::string, std::string> getFieldPathFromExpr(ExprNode& expr);
    std::optional<int64_t> evaluateIntegerConstant(ExprNode& expr);

    bool isConsumingOperation(const std::string& operation);
    bool hasReturnValue(const std::string& functionName, size_t argCount);
    void validateRangeCall(CallNode& node);

    void checkOwnershipViolation(
        const std::string& varName,
        const SourceLocation& location
    );
    void checkUnusedVariables();
    void
    checkUnusedFunctionResult(ExprNode& expr, const SourceLocation& location);
    void
    reassignVariable(
        const std::string& name,
        const SourceLocation& location,
        StorageResidency storage = StorageResidency::MAIN_STACK
    );

    std::map<std::string, VariableInfo> saveVariableState();
    void restoreVariableState(
        const std::map<std::string, VariableInfo>& savedState
    );
    void mergeBranchStates(
        const std::map<std::string, VariableInfo>& thenState,
        const std::map<std::string, VariableInfo>& elseState,
        const std::map<std::string, VariableInfo>& entryState
    );

    SourceLocation getNodeLocation(ASTNode& node);
    bool isConstantValue(ExprNode& expr);
    bool isContractMember(const std::string& varName);
    bool isBuiltinObject(const std::string& varName);

    bool isInSubscope() const;
    bool isPrivateFunction(const std::string& functionName) const;
    bool isAltstackOperation(const std::string& functionName) const;
    void checkAltstackOperationAllowed(
        const std::string& functionName,
        const SourceLocation& location
    );

private:
    // 仅存当前函数作用域变量.
    std::map<std::string, VariableInfo> m_variables;

    std::map<std::string, std::vector<std::pair<std::string, std::string>>>
        m_structDefinitions;

    std::vector<std::string> m_errors;
    std::vector<std::string> m_warnings;

    std::set<std::string> m_consumingOperations;

    bool m_hasErrors;

    bool m_allowSubscopeAltstack{false};
    bool m_inIfElseScope{false};
    bool m_inPrivateFunction{false};
    bool m_inLibraryFunction{false};
    std::string m_currentFunctionName;
    std::map<std::string, int64_t> m_staticLoopValues;

    // 上下文敏感函数 (私有 / 库) 的形参名集合. 这些形参在调用点可能绑定到
    // 合约成员 <self.X> (允许多读) 或栈值 (受 move 约束). 由于 PreAnalysis
    // 无法静态判定绑定情况, 这里对集合内的变量跳过 move-once 检查, 留给
    // ast_to_bytecode 阶段的 visitOperator/visitCall 用 boundName 兜底.
    std::set<std::string> m_deferredOwnershipParams;

    // 小写 return 走只读: 访问表达式但不消耗所有权.
    bool m_readOnlyMode{false};
};

#endif // PRE_ANALYSIS_VISITOR_H

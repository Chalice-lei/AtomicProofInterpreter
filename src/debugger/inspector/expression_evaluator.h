#ifndef EXPRESSION_EVALUATOR_H
#define EXPRESSION_EVALUATOR_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "variable_inspector.h"
#include "scope_inspector.h"
#include "../vm/stack_state.h"

namespace apc_debug {

class BVMSimulator;

struct EvaluationResult {
    std::string value;
    std::string type;
    bool success;
    std::string errorMessage;

    EvaluationResult() : success(false) {}
    
    EvaluationResult(const std::string& v, const std::string& t)
        : value(v), type(t), success(true) {}
    
    static EvaluationResult error(const std::string& msg) {
        EvaluationResult result;
        result.success = false;
        result.errorMessage = msg;
        return result;
    }
};

/**
 * @brief 表达式求值器：变量、字面量、算术/比较运算、栈访问。
 */
class ExpressionEvaluator {
public:
    ExpressionEvaluator(
        std::shared_ptr<VariableInspector> varInspector,
        std::shared_ptr<ScopeInspector> scopeInspector
    );
    ~ExpressionEvaluator() = default;
    
    EvaluationResult evaluate(
        const std::string& expression,
        const StackState& stack,
        size_t currentPC
    );

    EvaluationResult evaluateVariable(
        const std::string& varName,
        const StackState& stack,
        size_t currentPC
    );

    // 形如 "stack[0]"、"stack.top"
    EvaluationResult evaluateStackAccess(
        const std::string& expression,
        const StackState& stack
    );

    EvaluationResult evaluateLiteral(const std::string& literal);

    EvaluationResult evaluateBinaryOp(
        const std::string& op,
        const EvaluationResult& left,
        const EvaluationResult& right
    );
    
private:
    std::shared_ptr<VariableInspector> m_varInspector;
    std::shared_ptr<ScopeInspector> m_scopeInspector;

    enum class ExprType {
        Variable,
        Literal,
        StackAccess,
        BinaryOp,
        Unknown
    };

    ExprType getExpressionType(const std::string& expr);

    struct BinaryOpParts {
        std::string left;
        std::string op;
        std::string right;
    };
    std::optional<BinaryOpParts> parseBinaryOp(const std::string& expr);

    std::string trim(const std::string& str);

    bool isLiteral(const std::string& expr);
    bool isNumberLiteral(const std::string& expr);
    bool isStringLiteral(const std::string& expr);
    bool isBooleanLiteral(const std::string& expr);
    bool isStackAccess(const std::string& expr);

    EvaluationResult performArithmetic(
        const std::string& op,
        int64_t left,
        int64_t right
    );

    EvaluationResult performComparison(
        const std::string& op,
        int64_t left,
        int64_t right
    );

    std::optional<int64_t> toInt(const std::string& str);
};

} // namespace apc_debug

#endif // EXPRESSION_EVALUATOR_H




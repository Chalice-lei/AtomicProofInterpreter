#include "constant_folder.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>

#include "../error/error_manager.h"
#include "../log/logger.h"
#include "range_plan.h"
#include "static_integer_evaluator.h"

ConstantFolder::Result ConstantFolder::fold(ContractNode& contract)
{
    m_result = Result{};

    for (auto& member : contract.members) {
        if (auto* fn = dynamic_cast<FunctionNode*>(member.get())) {
            foldFunction(*fn);
        }
        // StructDefNode 等无可折叠表达式, 跳过.
    }
    return m_result;
}

void ConstantFolder::foldFunction(FunctionNode& fn)
{
    // 每个函数独立维护传播环境, 避免同名局部变量跨函数误传播.
    m_assignCounts.clear();
    m_readCounts.clear();
    m_scalarConsts.clear();
    m_arrayConsts.clear();
    m_declaredNamesStack.clear();
    m_parameterNames.clear();
    for (const auto& parameter : fn.parameters) {
        m_parameterNames.insert(parameter.name);
    }

    if (fn.block) {
        // 赋值=1 启用传播; (assign==1 && read==0) 触发死变量消除.
        for (auto& stmt : fn.block->statements) {
            countAssignmentsInStmt(stmt.get());
            countReadsInStmt(stmt.get());
        }
        foldBlock(*fn.block);
    }
}

void ConstantFolder::foldBlock(BlockNode& block)
{
    auto entryEnvironment = captureEnvironment();
    m_declaredNamesStack.emplace_back();

    for (auto& stmt : block.statements) {
        foldStmt(stmt);
    }
    // 死分支消除可能把 stmt 置空, 过滤掉.
    block.statements.erase(
        std::remove_if(
            block.statements.begin(),
            block.statements.end(),
            [](const std::unique_ptr<StmtNode>& s) { return !s; }
        ),
        block.statements.end()
    );

    auto declaredNames = std::move(m_declaredNamesStack.back());
    m_declaredNamesStack.pop_back();
    restoreDeclaredNames(entryEnvironment, declaredNames);
}

void ConstantFolder::foldStmt(std::unique_ptr<StmtNode>& stmt)
{
    if (!stmt) {
        return;
    }

    if (auto* b = dynamic_cast<BlockNode*>(stmt.get())) {
        foldBlock(*b);
    } else if (auto* i = dynamic_cast<IfNode*>(stmt.get())) {
        foldExpr(i->condition);
        // 死分支剪除: condition 折成数值字面量后, 直接换成选中的分支.
        if (auto* lit = dynamic_cast<LiteralNode*>(i->condition.get());
            lit && lit->type == LiteralNode::Type::Number) {
            auto numOpt = literalAsInt(*lit);
            if (numOpt.has_value()) {
                std::unique_ptr<StmtNode> kept = (*numOpt != 0)
                                                     ? std::move(i->thenBranch)
                                                     : std::move(i->elseBranch);
                LOG_DEBUG(
                    "Dead branch elimination: if(" + lit->value + ") -> " +
                    std::string(*numOpt != 0 ? "thenBranch" : "elseBranch")
                );
                if (kept) {
                    foldStmt(kept);
                }
                stmt = std::move(kept);
                return;
            }
        }

        // then/else 必须从同一入口常量环境开始. 两条可达路径结束后，
        // 只有在两边仍存在且值相同的绑定才能继续传播.
        auto entryEnvironment = captureEnvironment();

        foldStmt(i->thenBranch);
        auto thenEnvironment = captureEnvironment();

        restoreEnvironment(std::move(entryEnvironment));
        foldStmt(i->elseBranch);
        auto elseEnvironment = captureEnvironment();

        restoreEnvironment(
            mergeEnvironments(thenEnvironment, elseEnvironment)
        );
    } else if (auto* f = dynamic_cast<ForNode*>(stmt.get())) {
        foldExpr(f->iterable);
        // 死循环消除复用 lowering 的有界整数求值和 Range 算法，避免
        // 在这里维护另一套可能溢出的 int64 步进逻辑。
        if (auto* call = dynamic_cast<CallNode*>(f->iterable.get());
            call && call->funcName == "Range" && !call->args.empty() &&
            call->args.size() <= 3) {
            std::vector<int64_t> bounds;
            bounds.reserve(call->args.size());
            bool allKnown = true;
            for (auto& arg : call->args) {
                const auto value =
                    compiler::StaticIntegerEvaluator::evaluate(*arg);
                if (value.isKnown()) {
                    bounds.push_back(value.value);
                    continue;
                }
                allKnown = false;
                break;
            }
            if (allKnown) {
                int64_t start = 0, stop = 0, step = 1;
                if (bounds.size() == 1) {
                    stop = bounds[0];
                } else if (bounds.size() == 2) {
                    start = bounds[0];
                    stop = bounds[1];
                } else {
                    start = bounds[0];
                    stop = bounds[1];
                    step = bounds[2];
                }
                auto plan = apc::compiler::RangePlan::build(
                    start, stop, step
                );
                if (plan && plan.value().empty()) {
                    LOG_DEBUG(
                        "Dead loop elimination: for " + f->target +
                        " in Range(empty) -> ∅"
                    );
                    stmt.reset();
                    return;
                }
            }
        }

        // for target 与 AST/bytecode 分析使用相同的绑定规则:
        // - 入口已有同名绑定: 每轮对外层变量重新赋值，旧常量必须
        //   在折叠 body 前失效，退出循环后也不能恢复;
        // - 入口无同名绑定: target 属于 for 作用域，退出时移除.
        const auto loopEntryEnvironment = captureEnvironment();
        const bool targetExistedBeforeLoop = isNameVisible(f->target);
        m_declaredNamesStack.emplace_back();
        if (!targetExistedBeforeLoop) {
            recordDeclaration(f->target);
        }
        invalidateName(f->target);

        if (f->body) {
            foldBlock(*f->body);
        }

        auto loopDeclaredNames = std::move(m_declaredNamesStack.back());
        m_declaredNamesStack.pop_back();
        restoreDeclaredNames(loopEntryEnvironment, loopDeclaredNames);
    } else if (auto* a = dynamic_cast<AssignNode*>(stmt.get())) {
        // LHS 可能含 a[1+2] 这类索引: 保留 base 的变量身份，但允许
        // 折叠索引表达式.
        foldExpr(a->name, FoldContext::LValue);
        foldExpr(a->value);
        // 赋值目标从传播环境失效; 传播仅作用于全函数唯一赋值的名字.
        invalidateName(extractAssignTargetName(a->name.get()));
    } else if (auto* es = dynamic_cast<ExprStmtNode*>(stmt.get())) {
        foldExpr(es->expr);
    } else if (auto* r = dynamic_cast<ReturnNode*>(stmt.get())) {
        foldExpr(
            r->expr,
            r->isValueReturn ? FoldContext::Identity : FoldContext::RValue
        );
    } else if (auto* vd = dynamic_cast<VarDeclNode*>(stmt.get())) {
        foldExpr(vd->initValue);
        // 死变量消除: 唯一声明 + 从未被读 + RHS 是字面量 (或 null) -> 删整条.
        // 必须要求字面量: `x + y` 这类纯表达式仍会消耗栈参数, 删掉会让
        // 参数滞留栈顶, 破坏 scope cleanup 的栈模型.
        auto cntIt = m_assignCounts.find(vd->name);
        auto readIt = m_readCounts.find(vd->name);
        int reads = (readIt == m_readCounts.end()) ? 0 : readIt->second;
        const bool rhsIsLiteralOrNone =
            !vd->initValue ||
            dynamic_cast<LiteralNode*>(vd->initValue.get()) != nullptr;
        if (cntIt != m_assignCounts.end() && cntIt->second == 1 &&
            reads == 0 && rhsIsLiteralOrNone) {
            LOG_DEBUG("Dead variable elimination: " + vd->name);
            stmt.reset();
            return;
        }
        recordDeclaration(vd->name);
        invalidateName(vd->name);
        // 唯一赋值 + 字面量初始化: 记录供后续标识符替换.
        if (cntIt != m_assignCounts.end() && cntIt->second == 1 &&
            vd->initValue) {
            if (auto* lit = dynamic_cast<LiteralNode*>(vd->initValue.get())) {
                m_scalarConsts[vd->name] = cloneLiteralAt(
                    *lit, vd->pos.first, vd->pos.second
                );
            }
        }
    } else if (auto* ad = dynamic_cast<ArrayDeclNode*>(stmt.get())) {
        foldExpr(ad->sizeExpr);
        if (ad->initArray) {
            for (auto& e : ad->initArray->elements) {
                foldExpr(e);
            }
        }
        recordDeclaration(ad->name);
        invalidateName(ad->name);
        // 唯一赋值 + 全字面量元素: 记录元素列表供 arr[常量] 替换.
        auto cntIt = m_assignCounts.find(ad->name);
        if (cntIt != m_assignCounts.end() && cntIt->second == 1 &&
            ad->initArray) {
            std::vector<std::unique_ptr<LiteralNode>> elems;
            elems.reserve(ad->initArray->elements.size());
            bool allLiterals = true;
            for (auto& e : ad->initArray->elements) {
                auto* lit = dynamic_cast<LiteralNode*>(e.get());
                if (!lit) {
                    allLiterals = false;
                    break;
                }
                elems.push_back(
                    cloneLiteralAt(*lit, ad->pos.first, ad->pos.second)
                );
            }
            if (allLiterals) {
                m_arrayConsts[ad->name] = std::move(elems);
            }
        }
    } else if (auto* da = dynamic_cast<DestructureAssignNode*>(stmt.get())) {
        foldExpr(da->value);
        for (auto& t : da->targets) {
            invalidateName(t);
        }
    }
    // 其他语句类型无可折叠表达式.
}

void ConstantFolder::foldExpr(
    std::unique_ptr<ExprNode>& expr, FoldContext context
)
{
    if (!expr) {
        return;
    }

    if (auto* op = dynamic_cast<OpNode*>(expr.get())) {
        // 先递归折子树, 使 (1+2)+3 外层能看到内层已折为 Literal.
        if (op->lhs) {
            foldExpr(op->lhs, context);
        }
        if (op->rhs) {
            foldExpr(op->rhs, context);
        }
        // 短路: 0 && x -> 0, 1 || x -> 1. 仅按左侧字面量短路, 右侧字面量
        // 不短路 (左侧可能有副作用必须求值).
        if (context == FoldContext::RValue &&
            (op->op == "&&" || op->op == "||") && op->lhs && op->rhs) {
            if (auto* lhsLit = dynamic_cast<LiteralNode*>(op->lhs.get());
                lhsLit && lhsLit->type == LiteralNode::Type::Number) {
                auto lv = literalAsInt(*lhsLit);
                if (lv.has_value()) {
                    if (op->op == "&&" && *lv == 0) {
                        LOG_DEBUG("Short-circuit: 0 && x -> 0");
                        expr = std::make_unique<LiteralNode>(
                            LiteralNode::Type::Number,
                            "0",
                            op->pos.first,
                            op->pos.second
                        );
                        return;
                    }
                    if (op->op == "||" && *lv != 0) {
                        LOG_DEBUG("Short-circuit: 1 || x -> 1");
                        expr = std::make_unique<LiteralNode>(
                            LiteralNode::Type::Number,
                            "1",
                            op->pos.first,
                            op->pos.second
                        );
                        return;
                    }
                }
            }
        }
        if (context == FoldContext::RValue) {
            if (auto folded = tryFoldOp(*op)) {
                expr = std::move(folded);
            }
        }
    } else if (auto* call = dynamic_cast<CallNode*>(expr.get())) {
        const bool identitySensitive = isIdentitySensitiveCall(call->funcName);
        const auto argumentContext = identitySensitive ? FoldContext::Identity
                                                       : context;
        for (auto& arg : call->args) {
            foldExpr(arg, argumentContext);
        }
        // 一旦栈操作观察或改变了变量的运行时栈槽，后续访问就不能
        // 再用字面量替换，否则会绕过 Delete/Move 的消费检查，或忽略
        // SetAlt/SetMain/Keep 建立的真实栈布局.
        if (identitySensitive) {
            for (const auto& arg : call->args) {
                invalidateName(extractAssignTargetName(arg.get()));
            }
        }
    } else if (auto* mc = dynamic_cast<MethodCallNode*>(expr.get())) {
        // 方法调用的 receiver 参与所有权/栈槽查找，必须保留身份.
        foldExpr(mc->object, FoldContext::Identity);
        for (auto& arg : mc->args) {
            foldExpr(arg);
        }
        invalidateName(extractAssignTargetName(mc->object.get()));
    } else if (auto* fa = dynamic_cast<FieldAccessNode*>(expr.get())) {
        foldExpr(
            fa->base,
            context == FoldContext::RValue ? FoldContext::RValue
                                           : FoldContext::Identity
        );
    } else if (auto* ia = dynamic_cast<IndexAccessNode*>(expr.get())) {
        foldExpr(
            ia->base,
            context == FoldContext::RValue ? FoldContext::RValue
                                           : FoldContext::Identity
        );
        foldExpr(ia->index);
        // arr[字面常量] + arr 唯一字面量数组初始化 -> 替换为对应元素.
        if (context == FoldContext::RValue) {
            if (auto* baseId = dynamic_cast<IdentifierNode*>(ia->base.get())) {
                auto arrIt = m_arrayConsts.find(baseId->name);
                if (arrIt != m_arrayConsts.end()) {
                    if (auto* idxLit =
                            dynamic_cast<LiteralNode*>(ia->index.get())) {
                        auto idxVal = literalAsInt(*idxLit);
                        if (idxVal.has_value() && *idxVal >= 0 &&
                            static_cast<size_t>(*idxVal) <
                                arrIt->second.size()) {
                            const LiteralNode& lit =
                                *arrIt->second[static_cast<size_t>(*idxVal)];
                            LOG_DEBUG(
                                "Constant propagation: " + baseId->name +
                                "[" + idxLit->value + "] = " + lit.value
                            );
                            expr = cloneLiteralAt(
                                lit, ia->pos.first, ia->pos.second
                            );
                        }
                    }
                }
            }
        }
    } else if (auto* be = dynamic_cast<BraceExprNode*>(expr.get())) {
        for (auto& e : be->elements) {
            foldExpr(e);
        }
    } else if (auto* ad = dynamic_cast<ArrayDefNode*>(expr.get())) {
        for (auto& e : ad->elements) {
            foldExpr(e);
        }
    } else if (auto* id = dynamic_cast<IdentifierNode*>(expr.get())) {
        // 唯一字面量初始化的标量, 替换为初始值.
        auto it = m_scalarConsts.find(id->name);
        if (context == FoldContext::RValue && it != m_scalarConsts.end()) {
            LOG_DEBUG(
                "Constant propagation: " + id->name + " = " + it->second->value
            );
            expr =
                cloneLiteralAt(*it->second, id->pos.first, id->pos.second);
        }
    }
    // LiteralNode: 叶子, 无子树.
}

ConstantFolder::ConstEnvironment ConstantFolder::captureEnvironment() const
{
    ConstEnvironment result;
    for (const auto& [name, literal] : m_scalarConsts) {
        result.scalars[name] = cloneLiteralAt(
            *literal, literal->pos.first, literal->pos.second
        );
    }
    for (const auto& [name, literals] : m_arrayConsts) {
        auto& destination = result.arrays[name];
        destination.reserve(literals.size());
        for (const auto& literal : literals) {
            destination.push_back(cloneLiteralAt(
                *literal, literal->pos.first, literal->pos.second
            ));
        }
    }
    return result;
}

void ConstantFolder::restoreEnvironment(ConstEnvironment environment)
{
    m_scalarConsts = std::move(environment.scalars);
    m_arrayConsts = std::move(environment.arrays);
}

ConstantFolder::ConstEnvironment ConstantFolder::mergeEnvironments(
    const ConstEnvironment& thenEnvironment,
    const ConstEnvironment& elseEnvironment
) const
{
    ConstEnvironment result;
    for (const auto& [name, thenLiteral] : thenEnvironment.scalars) {
        auto elseIt = elseEnvironment.scalars.find(name);
        if (elseIt != elseEnvironment.scalars.end() &&
            sameLiteral(*thenLiteral, *elseIt->second)) {
            result.scalars[name] = cloneLiteralAt(
                *thenLiteral,
                thenLiteral->pos.first,
                thenLiteral->pos.second
            );
        }
    }
    for (const auto& [name, thenLiterals] : thenEnvironment.arrays) {
        auto elseIt = elseEnvironment.arrays.find(name);
        if (elseIt != elseEnvironment.arrays.end() &&
            sameLiteralArray(thenLiterals, elseIt->second)) {
            auto& destination = result.arrays[name];
            destination.reserve(thenLiterals.size());
            for (const auto& literal : thenLiterals) {
                destination.push_back(cloneLiteralAt(
                    *literal, literal->pos.first, literal->pos.second
                ));
            }
        }
    }
    return result;
}

void ConstantFolder::restoreDeclaredNames(
    const ConstEnvironment& entryEnvironment,
    const std::set<std::string>& declaredNames
)
{
    for (const auto& name : declaredNames) {
        invalidateName(name);

        auto scalarIt = entryEnvironment.scalars.find(name);
        if (scalarIt != entryEnvironment.scalars.end()) {
            m_scalarConsts[name] = cloneLiteralAt(
                *scalarIt->second,
                scalarIt->second->pos.first,
                scalarIt->second->pos.second
            );
        }

        auto arrayIt = entryEnvironment.arrays.find(name);
        if (arrayIt != entryEnvironment.arrays.end()) {
            auto& destination = m_arrayConsts[name];
            destination.reserve(arrayIt->second.size());
            for (const auto& literal : arrayIt->second) {
                destination.push_back(cloneLiteralAt(
                    *literal, literal->pos.first, literal->pos.second
                ));
            }
        }
    }
}

void ConstantFolder::recordDeclaration(const std::string& name)
{
    if (!m_declaredNamesStack.empty()) {
        m_declaredNamesStack.back().insert(name);
    }
}

bool ConstantFolder::isNameVisible(const std::string& name) const
{
    if (m_parameterNames.count(name) != 0) {
        return true;
    }
    return std::any_of(
        m_declaredNamesStack.rbegin(),
        m_declaredNamesStack.rend(),
        [&](const auto& declaredNames) {
            return declaredNames.count(name) != 0;
        }
    );
}

bool ConstantFolder::sameLiteral(
    const LiteralNode& lhs, const LiteralNode& rhs
)
{
    return lhs.type == rhs.type && lhs.value == rhs.value;
}

bool ConstantFolder::sameLiteralArray(
    const std::vector<std::unique_ptr<LiteralNode>>& lhs,
    const std::vector<std::unique_ptr<LiteralNode>>& rhs
)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!sameLiteral(*lhs[i], *rhs[i])) {
            return false;
        }
    }
    return true;
}

bool ConstantFolder::isIdentitySensitiveCall(const std::string& functionName)
{
    return functionName == "Delete" || functionName == "Keep" ||
           functionName == "Move" || functionName == "SetAlt" ||
           functionName == "SetMain";
}

std::unique_ptr<LiteralNode> ConstantFolder::tryFoldOp(OpNode& op)
{
    if (!op.rhs) {
        return nullptr;
    }
    auto* rhsLit = dynamic_cast<LiteralNode*>(op.rhs.get());
    if (!rhsLit) {
        return nullptr;
    }

    if (!op.lhs) {
        return tryFoldUnary(op, *rhsLit);
    }

    auto* lhsLit = dynamic_cast<LiteralNode*>(op.lhs.get());
    if (!lhsLit) {
        return nullptr;
    }
    return tryFoldBinary(op, *lhsLit, *rhsLit);
}

std::unique_ptr<LiteralNode>
ConstantFolder::tryFoldUnary(OpNode& op, const LiteralNode& rhs)
{
    const auto result = compiler::StaticIntegerEvaluator::evaluate(op);
    if (result.isError()) {
        reportError(op, result.diagnostic);
        return nullptr;
    }
    if (!result.isKnown()) {
        return nullptr;
    }

    LOG_DEBUG(
        "Constant folding unary: " + op.op + rhs.value + " = " +
        std::to_string(result.value)
    );
    return std::make_unique<LiteralNode>(
        LiteralNode::Type::Number,
        std::to_string(result.value),
        op.pos.first,
        op.pos.second
    );
}

std::unique_ptr<LiteralNode> ConstantFolder::tryFoldBinary(
    OpNode& op, const LiteralNode& lhs, const LiteralNode& rhs
)
{
    const auto result = compiler::StaticIntegerEvaluator::evaluate(op);
    if (result.isError()) {
        reportError(op, result.diagnostic);
        return nullptr;
    }
    if (!result.isKnown()) {
        return nullptr;
    }

    LOG_DEBUG(
        "Constant folding binary: " + lhs.value + " " + op.op + " " + rhs.value +
        " = " + std::to_string(result.value)
    );
    // 比较 / 逻辑结果统一按 Number(0|1) 输出, 避开 Boolean 字面量在
    // visitLiteral 里的 true/false -> 0x00/0x51 特殊映射.
    return std::make_unique<LiteralNode>(
        LiteralNode::Type::Number,
        std::to_string(result.value),
        op.pos.first,
        op.pos.second
    );
}

void ConstantFolder::reportError(const ASTNode& node, const std::string& msg)
{
    m_result.hasError = true;
    m_result.errors.push_back(msg);
    SourceLocation loc = node.hasSourceLocation()
                             ? node.sourceLocation
                             : SourceLocation(
                                   "", node.pos.first, node.pos.second
                               );
    ErrorManager::getInstance().semanticError(msg, loc);
    LOG_ERROR("Constant folding error: " + msg);
}

void ConstantFolder::countAssignmentsInStmt(StmtNode* stmt)
{
    if (!stmt) {
        return;
    }
    if (auto* b = dynamic_cast<BlockNode*>(stmt)) {
        for (auto& s : b->statements) {
            countAssignmentsInStmt(s.get());
        }
    } else if (auto* i = dynamic_cast<IfNode*>(stmt)) {
        countAssignmentsInStmt(i->thenBranch.get());
        countAssignmentsInStmt(i->elseBranch.get());
    } else if (auto* f = dynamic_cast<ForNode*>(stmt)) {
        // 循环目标在每轮入口都会被重新赋值，即使源码没有显式 AssignNode。
        // 把它视为多写，防止循环前常量被传播进循环体。
        if (!f->target.empty()) {
            m_assignCounts[f->target] += 2;
        }
        // 循环体内的赋值会执行多次: body 内出现的名字额外 +1, 使
        // 计数 > 1 以禁用常量传播.
        if (!f->body) {
            return;
        }
        auto before = m_assignCounts;
        countAssignmentsInStmt(f->body.get());
        for (auto& kv : m_assignCounts) {
            auto it = before.find(kv.first);
            int delta = (it == before.end()) ? kv.second : kv.second - it->second;
            if (delta > 0) {
                kv.second += 1;
            }
        }
    } else if (auto* a = dynamic_cast<AssignNode*>(stmt)) {
        std::string tgt = extractAssignTargetName(a->name.get());
        if (!tgt.empty()) {
            m_assignCounts[tgt]++;
        }
    } else if (auto* da = dynamic_cast<DestructureAssignNode*>(stmt)) {
        for (auto& t : da->targets) {
            m_assignCounts[t]++;
        }
    } else if (auto* vd = dynamic_cast<VarDeclNode*>(stmt)) {
        m_assignCounts[vd->name]++;
    } else if (auto* ad = dynamic_cast<ArrayDeclNode*>(stmt)) {
        m_assignCounts[ad->name]++;
    }
    // ExprStmtNode / ReturnNode: 无赋值.
}

std::string ConstantFolder::extractAssignTargetName(ExprNode* lhs) const
{
    if (!lhs) {
        return {};
    }
    if (auto* id = dynamic_cast<IdentifierNode*>(lhs)) {
        return id->name;
    }
    if (auto* ia = dynamic_cast<IndexAccessNode*>(lhs)) {
        return extractAssignTargetName(ia->base.get());
    }
    if (auto* fa = dynamic_cast<FieldAccessNode*>(lhs)) {
        return extractAssignTargetName(fa->base.get());
    }
    return {};
}

std::unique_ptr<LiteralNode> ConstantFolder::cloneLiteralAt(
    const LiteralNode& lit, int32_t x, int32_t y
) const
{
    return std::make_unique<LiteralNode>(lit.type, lit.value, x, y);
}

std::optional<int64_t> ConstantFolder::literalAsInt(const LiteralNode& lit)
{
    if (lit.type != LiteralNode::Type::Number) {
        return std::nullopt;
    }
    try {
        return std::stoll(lit.value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void ConstantFolder::invalidateName(const std::string& name)
{
    if (name.empty()) {
        return;
    }
    m_scalarConsts.erase(name);
    m_arrayConsts.erase(name);
}

void ConstantFolder::countReadsInExpr(ExprNode* expr)
{
    if (!expr) {
        return;
    }
    if (auto* id = dynamic_cast<IdentifierNode*>(expr)) {
        m_readCounts[id->name]++;
    } else if (auto* op = dynamic_cast<OpNode*>(expr)) {
        countReadsInExpr(op->lhs.get());
        countReadsInExpr(op->rhs.get());
    } else if (auto* call = dynamic_cast<CallNode*>(expr)) {
        for (auto& arg : call->args) {
            countReadsInExpr(arg.get());
        }
    } else if (auto* mc = dynamic_cast<MethodCallNode*>(expr)) {
        countReadsInExpr(mc->object.get());
        for (auto& arg : mc->args) {
            countReadsInExpr(arg.get());
        }
    } else if (auto* fa = dynamic_cast<FieldAccessNode*>(expr)) {
        countReadsInExpr(fa->base.get());
    } else if (auto* ia = dynamic_cast<IndexAccessNode*>(expr)) {
        countReadsInExpr(ia->base.get());
        countReadsInExpr(ia->index.get());
    } else if (auto* be = dynamic_cast<BraceExprNode*>(expr)) {
        for (auto& e : be->elements) {
            countReadsInExpr(e.get());
        }
    } else if (auto* ad = dynamic_cast<ArrayDefNode*>(expr)) {
        for (auto& e : ad->elements) {
            countReadsInExpr(e.get());
        }
    }
}

void ConstantFolder::countReadsInStmt(StmtNode* stmt)
{
    if (!stmt) {
        return;
    }
    if (auto* b = dynamic_cast<BlockNode*>(stmt)) {
        for (auto& s : b->statements) {
            countReadsInStmt(s.get());
        }
    } else if (auto* i = dynamic_cast<IfNode*>(stmt)) {
        countReadsInExpr(i->condition.get());
        countReadsInStmt(i->thenBranch.get());
        countReadsInStmt(i->elseBranch.get());
    } else if (auto* f = dynamic_cast<ForNode*>(stmt)) {
        countReadsInExpr(f->iterable.get());
        if (f->body) {
            for (auto& s : f->body->statements) {
                countReadsInStmt(s.get());
            }
        }
    } else if (auto* a = dynamic_cast<AssignNode*>(stmt)) {
        // 纯标识符 LHS 不算读; arr[i]=v 中 i 算读, base 仅被写入.
        if (dynamic_cast<IdentifierNode*>(a->name.get())) {
        } else if (auto* ia = dynamic_cast<IndexAccessNode*>(a->name.get())) {
            countReadsInExpr(ia->index.get());
        }
        countReadsInExpr(a->value.get());
    } else if (auto* es = dynamic_cast<ExprStmtNode*>(stmt)) {
        countReadsInExpr(es->expr.get());
    } else if (auto* r = dynamic_cast<ReturnNode*>(stmt)) {
        countReadsInExpr(r->expr.get());
    } else if (auto* vd = dynamic_cast<VarDeclNode*>(stmt)) {
        countReadsInExpr(vd->initValue.get());
    } else if (auto* ad = dynamic_cast<ArrayDeclNode*>(stmt)) {
        countReadsInExpr(ad->sizeExpr.get());
        if (ad->initArray) {
            for (auto& e : ad->initArray->elements) {
                countReadsInExpr(e.get());
            }
        }
    } else if (auto* da = dynamic_cast<DestructureAssignNode*>(stmt)) {
        countReadsInExpr(da->value.get());
    }
}

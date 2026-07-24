#include "debug_info.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_set>

using json = nlohmann::json;

namespace apc_debug
{
namespace
{
std::string normalizeSourceFilename(
    const std::string& filename,
    const std::string& primarySource
)
{
    if (filename.empty()) {
        return "";
    }

    namespace fs = std::filesystem;
    fs::path sourcePath(filename);
    if (sourcePath.is_relative() && !primarySource.empty()) {
        const fs::path primaryPath(primarySource);
        if (sourcePath.lexically_normal() != primaryPath.lexically_normal()) {
            std::error_code primaryError;
            fs::path primaryAbsolute = primaryPath.is_absolute()
                                           ? primaryPath
                                           : fs::absolute(
                                                 primaryPath,
                                                 primaryError
                                             );
            if (primaryError) {
                primaryAbsolute = primaryPath;
            }
            sourcePath = primaryAbsolute.parent_path() / sourcePath;
        }
    }

    std::error_code error;
    fs::path normalized = fs::weakly_canonical(sourcePath, error);
    if (error) {
        normalized = fs::absolute(sourcePath, error);
        if (error) {
            normalized = sourcePath;
        }
        normalized = normalized.lexically_normal();
    }

    std::string value = normalized.generic_string();
#ifdef _WIN32
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); }
    );
#endif
    return value;
}

json sourceLocationToJson(const SourceLocation& loc)
{
    json locJson;
    locJson["file"] = loc.filename;
    locJson["line"] = loc.line;
    locJson["column"] = loc.column;
    locJson["endLine"] = loc.endLine;
    locJson["endColumn"] = loc.endColumn;
    return locJson;
}

SourceLocation sourceLocationFromJson(const json& locJson)
{
    SourceLocation loc;
    loc.filename = locJson.value("file", "");
    loc.line = locJson.value("line", 0);
    loc.column = locJson.value("column", 0);
    loc.endLine = locJson.value("endLine", loc.line);
    loc.endColumn = locJson.value("endColumn", loc.column);
    return loc;
}

json variableToJson(const VariableDebugInfo& var)
{
    json varJson;
    varJson["name"] = var.name;
    varJson["type"] = var.type;
    varJson["scopeName"] = var.scopeName;
    varJson["declLine"] = var.declLine;
    varJson["declColumn"] = var.declColumn;
    varJson["isStackVar"] = var.isStackVar;
    varJson["stackOffset"] = var.stackOffset;
    varJson["isParameter"] = var.isParameter;
    return varJson;
}

VariableDebugInfo variableFromJson(const json& varJson)
{
    VariableDebugInfo var;
    var.name = varJson.value("name", "");
    var.type = varJson.value("type", "");
    var.scopeName = varJson.value("scopeName", "");
    var.declLine = varJson.value("declLine", 0);
    var.declColumn = varJson.value("declColumn", 0);
    var.isStackVar = varJson.value("isStackVar", true);
    var.stackOffset = varJson.value("stackOffset", -1);
    var.isParameter = varJson.value("isParameter", false);
    return var;
}

bool sameVariable(
    const VariableDebugInfo& left,
    const VariableDebugInfo& right
)
{
    return left.name == right.name && left.type == right.type &&
           left.scopeName == right.scopeName &&
           left.declLine == right.declLine &&
           left.declColumn == right.declColumn &&
           left.isStackVar == right.isStackVar &&
           left.stackOffset == right.stackOffset &&
           left.isParameter == right.isParameter;
}

bool sameSourceLocation(const SourceLocation& left, const SourceLocation& right)
{
    return left.filename == right.filename && left.line == right.line &&
           left.column == right.column && left.endLine == right.endLine &&
           left.endColumn == right.endColumn;
}

bool rangesOverlap(
    const ScopeDebugInfo& left,
    const ScopeDebugInfo& right
)
{
    // 空作用域只保留源码结构锚点，不覆盖任何运行时 PC。
    if (left.startPC == left.endPC || right.startPC == right.endPC) {
        return false;
    }
    return std::max(left.startPC, right.startPC) <
           std::min(left.endPC, right.endPC);
}

bool rangeContains(
    const ScopeDebugInfo& outer,
    const ScopeDebugInfo& inner
)
{
    return outer.startPC <= inner.startPC && outer.endPC >= inner.endPC;
}
} // namespace

std::string SourceLocation::toString() const
{
    std::ostringstream oss;
    oss << filename << ":" << line << ":" << column;
    if (endLine != line || endColumn != column) {
        oss << "-" << endLine << ":" << endColumn;
    }
    return oss.str();
}

void ScopeDebugInfo::addVariable(const VariableDebugInfo& var)
{
    variables.push_back(var);
}

const VariableDebugInfo* ScopeDebugInfo::findVariable(const std::string& name
) const
{
    for (const auto& var : variables) {
        if (var.name == name) {
            return &var;
        }
    }

    // 父作用域递归查找
    if (parent) {
        return parent->findVariable(name);
    }

    return nullptr;
}

void DebugInfo::addSourceMapping(size_t pc, const SourceLocation& loc)
{
    if (!loc.isValid()) {
        return;
    }

    auto existing = pcToSource.find(pc);
    if (existing != pcToSource.end() && existing->second.line != loc.line) {
        auto lineIt = lineToPCs.find(existing->second.line);
        if (lineIt != lineToPCs.end()) {
            auto& oldPcs = lineIt->second;
            oldPcs.erase(std::remove(oldPcs.begin(), oldPcs.end(), pc), oldPcs.end());
            if (oldPcs.empty()) {
                lineToPCs.erase(lineIt);
            }
        }
    }

    pcToSource[pc] = loc;
    auto& pcs = lineToPCs[loc.line];
    if (std::find(pcs.begin(), pcs.end(), pc) == pcs.end()) {
        pcs.push_back(pc);
        std::sort(pcs.begin(), pcs.end());
    }
}

void DebugInfo::addInstruction(const InstructionDebugInfo& info)
{
    instructions[info.pc] = info;

    if (info.location.isValid()) {
        addSourceMapping(info.pc, info.location);
    }
}

void DebugInfo::addInstruction(
    size_t pc,
    const std::string& opcode,
    const std::string& operand,
    const SourceLocation& loc
)
{
    InstructionDebugInfo info;
    info.pc = pc;
    info.opcode = opcode;
    info.operand = operand;
    info.location = loc;

    addInstruction(info);
}

void DebugInfo::addFunction(const FunctionDebugInfo& info)
{
    functions[info.name] = info;
}

void DebugInfo::addVariable(const VariableDebugInfo& info)
{
    variables[info.name] = info;
}

void DebugInfo::addScope(std::shared_ptr<ScopeDebugInfo> scope)
{
    scopes.push_back(scope);

    if (scope->type == ScopeType::GLOBAL) {
        globalScope = scope;
    }
}

SourceLocation DebugInfo::getSourceLocation(size_t pc) const
{
    auto it = pcToSource.find(pc);
    if (it != pcToSource.end()) {
        return it->second;
    }

    return SourceLocation();
}

std::vector<size_t> DebugInfo::getPCsForLine(size_t line) const
{
    auto it = lineToPCs.find(line);
    if (it != lineToPCs.end()) {
        return it->second;
    }
    return std::vector<size_t>();
}

std::vector<size_t> DebugInfo::getPCsForSourceLine(
    const std::string& filename,
    size_t line
) const
{
    const std::string requested = normalizeSourceFilename(
        filename,
        sourceFilename
    );
    if (requested.empty()) {
        return {};
    }

    std::vector<size_t> result;
    for (size_t pc : getPCsForLine(line)) {
        auto location = pcToSource.find(pc);
        if (location == pcToSource.end()) {
            continue;
        }
        const std::string& mappedFilename =
            location->second.filename.empty() ? sourceFilename
                                              : location->second.filename;
        if (normalizeSourceFilename(mappedFilename, sourceFilename) ==
            requested) {
            result.push_back(pc);
        }
    }
    return result;
}

std::vector<size_t>
DebugInfo::findNearestValidLine(size_t line, size_t maxDistance) const
{
    auto pcs = getPCsForLine(line);
    if (!pcs.empty()) {
        return pcs;
    }

    // 在附近查找；上限 10000 行用作粗略保护
    for (size_t dist = 1; dist <= maxDistance; ++dist) {
        if (line <= 10000 && dist <= 10000 - line) {
            pcs = getPCsForLine(line + dist);
            if (!pcs.empty()) {
                return pcs;
            }
        }

        if (line > dist) {
            pcs = getPCsForLine(line - dist);
            if (!pcs.empty()) {
                return pcs;
            }
        }
    }

    return std::vector<size_t>();
}

std::vector<size_t> DebugInfo::findNearestValidSourceLine(
    const std::string& filename,
    size_t line,
    size_t maxDistance
) const
{
    auto pcs = getPCsForSourceLine(filename, line);
    if (!pcs.empty()) {
        return pcs;
    }

    // 在同一源文件的附近查找；上限 10000 行用作粗略保护。
    for (size_t dist = 1; dist <= maxDistance; ++dist) {
        if (line <= 10000 && dist <= 10000 - line) {
            pcs = getPCsForSourceLine(filename, line + dist);
            if (!pcs.empty()) {
                return pcs;
            }
        }

        if (line > dist) {
            pcs = getPCsForSourceLine(filename, line - dist);
            if (!pcs.empty()) {
                return pcs;
            }
        }
    }

    return std::vector<size_t>();
}

const FunctionDebugInfo* DebugInfo::getFunctionAtPC(size_t pc) const
{
    for (const auto& [name, func] : functions) {
        if (pc >= func.startPC && pc < func.endPC) {
            return &func;
        }
    }
    return nullptr;
}

std::vector<VariableDebugInfo> DebugInfo::getVariablesInScope(size_t pc) const
{
    auto scope = getScopeAtPC(pc);
    if (!scope) {
        return std::vector<VariableDebugInfo>();
    }

    std::vector<VariableDebugInfo> result;

    auto currentScope = scope;
    while (currentScope) {
        result.insert(
            result.end(),
            currentScope->variables.begin(),
            currentScope->variables.end()
        );
        currentScope = currentScope->parent;
    }

    return result;
}

std::shared_ptr<ScopeDebugInfo> DebugInfo::getScopeAtPC(size_t pc) const
{
    // 取覆盖该 PC 的最小作用域
    std::shared_ptr<ScopeDebugInfo> result = nullptr;
    size_t smallestRange = SIZE_MAX;

    for (const auto& scope : scopes) {
        if (pc >= scope->startPC && pc < scope->endPC) {
            size_t range = scope->endPC - scope->startPC;
            if (range < smallestRange) {
                smallestRange = range;
                result = scope;
            }
        }
    }

    return result;
}

void DebugInfo::buildLineToPC()
{
    lineToPCs.clear();

    for (const auto& [pc, loc] : pcToSource) {
        if (loc.isValid()) {
            lineToPCs[loc.line].push_back(pc);
        }
    }

    for (auto& [line, pcs] : lineToPCs) {
        std::sort(pcs.begin(), pcs.end());
        pcs.erase(std::unique(pcs.begin(), pcs.end()), pcs.end());
    }
}

void DebugInfo::remapPCs(
    const std::vector<size_t>& oldToNew,
    size_t newInstructionCount
)
{
    const size_t invalidPC = std::numeric_limits<size_t>::max();

    auto mapPC = [&](size_t oldPC, size_t& newPC) {
        if (oldPC >= oldToNew.size()) {
            return false;
        }

        const size_t mappedPC = oldToNew[oldPC];
        if (mappedPC == invalidPC || mappedPC >= newInstructionCount) {
            return false;
        }

        newPC = mappedPC;
        return true;
    };

    std::map<size_t, SourceLocation> remappedSources;
    for (const auto& [oldPC, loc] : pcToSource) {
        size_t newPC = 0;
        if (mapPC(oldPC, newPC)) {
            remappedSources.emplace(newPC, loc);
        }
    }
    pcToSource = std::move(remappedSources);

    std::map<size_t, InstructionDebugInfo> remappedInstructions;
    for (const auto& [oldPC, inst] : instructions) {
        size_t newPC = 0;
        if (mapPC(oldPC, newPC)) {
            InstructionDebugInfo copy = inst;
            copy.pc = newPC;
            remappedInstructions.emplace(newPC, std::move(copy));
        }
    }
    instructions = std::move(remappedInstructions);

    auto remapRange = [&](size_t startPC,
                          size_t endPC,
                          size_t& newStartPC,
                          size_t& newEndPC) {
        if (startPC >= endPC) {
            return false;
        }

        bool found = false;
        size_t minPC = std::numeric_limits<size_t>::max();
        size_t maxPC = 0;
        const size_t cappedEndPC = std::min(endPC, oldToNew.size());
        for (size_t oldPC = startPC; oldPC < cappedEndPC; ++oldPC) {
            size_t mappedPC = 0;
            if (!mapPC(oldPC, mappedPC)) {
                continue;
            }

            found = true;
            minPC = std::min(minPC, mappedPC);
            maxPC = std::max(maxPC, mappedPC);
        }

        if (!found) {
            return false;
        }

        newStartPC = minPC;
        newEndPC = std::min(maxPC + 1, newInstructionCount);
        return newStartPC < newEndPC;
    };

    for (auto& [name, func] : functions) {
        size_t newStartPC = func.startPC;
        size_t newEndPC = func.endPC;
        if (remapRange(func.startPC, func.endPC, newStartPC, newEndPC)) {
            func.startPC = newStartPC;
            func.endPC = newEndPC;
        } else {
            func.endPC = func.startPC;
        }
    }

    for (auto& scope : scopes) {
        if (!scope) {
            continue;
        }

        size_t newStartPC = scope->startPC;
        size_t newEndPC = scope->endPC;
        if (remapRange(scope->startPC, scope->endPC, newStartPC, newEndPC)) {
            scope->startPC = newStartPC;
            scope->endPC = newEndPC;
        } else {
            scope->endPC = scope->startPC;
        }
    }

    buildLineToPC();
}

void DebugInfo::syncInstructionOpcodes(const std::vector<std::string>& bytecode)
{
    for (auto it = instructions.begin(); it != instructions.end();) {
        if (it->first >= bytecode.size()) {
            it = instructions.erase(it);
            continue;
        }

        it->second.pc = it->first;
        it->second.opcode = bytecode[it->first];
        it->second.operand.clear();
        ++it;
    }
}

bool DebugInfo::validate(std::string* errorMessage) const
{
    return validate({}, errorMessage);
}

bool DebugInfo::validate(
    const std::vector<std::string>& bytecode,
    std::string* errorMessage
) const
{
    auto fail = [&](const std::string& message) {
        if (errorMessage) {
            *errorMessage = message;
        }
        return false;
    };

    if (errorMessage) {
        errorMessage->clear();
    }

    if (sourceFilename.empty()) {
        return fail("缺少源码文件名");
    }
    if (!scopeNestingValid) {
        return fail("作用域 enter/exit 不配对");
    }

    const bool validateBytecode = !bytecode.empty();
    const size_t instructionCount = bytecode.size();
    size_t executableInstructionCount = instructionCount;
    if (validateBytecode) {
        const auto padding = std::find(bytecode.begin(), bytecode.end(), "ff");
        if (padding != bytecode.end()) {
            executableInstructionCount = static_cast<size_t>(
                std::distance(bytecode.begin(), padding)
            );
        }
    }

    for (const auto& [pc, loc] : pcToSource) {
        if (!loc.isValid()) {
            return fail("PC " + std::to_string(pc) + " 的源码位置无效");
        }
        if (validateBytecode && pc >= executableInstructionCount) {
            return fail("源码映射 PC 超出真实可执行字节码范围: " +
                        std::to_string(pc));
        }
        auto instIt = instructions.find(pc);
        if (validateBytecode && instIt == instructions.end()) {
            return fail("源码映射 PC 不对应真实调试指令: " +
                        std::to_string(pc));
        }
    }

    for (const auto& [line, pcs] : lineToPCs) {
        if (line == 0 || !std::is_sorted(pcs.begin(), pcs.end()) ||
            std::adjacent_find(pcs.begin(), pcs.end()) != pcs.end()) {
            return fail("行号反向索引无效: " + std::to_string(line));
        }
        for (size_t pc : pcs) {
            auto sourceIt = pcToSource.find(pc);
            if (sourceIt == pcToSource.end() || sourceIt->second.line != line) {
                return fail("行号反向索引与 PC 映射不一致: " +
                            std::to_string(line));
            }
        }
    }
    for (const auto& [pc, loc] : pcToSource) {
        auto lineIt = lineToPCs.find(loc.line);
        if (lineIt == lineToPCs.end() ||
            !std::binary_search(
                lineIt->second.begin(), lineIt->second.end(), pc
            )) {
            return fail("PC 映射缺少反向行号索引: " + std::to_string(pc));
        }
    }

    for (const auto& [pc, inst] : instructions) {
        if (inst.pc != pc) {
            return fail("指令记录的 PC 键值不一致: " + std::to_string(pc));
        }
        if (!inst.location.isValid()) {
            return fail("指令缺少有效源码位置: " + std::to_string(pc));
        }
        auto sourceIt = pcToSource.find(pc);
        if (sourceIt == pcToSource.end() ||
            !sameSourceLocation(sourceIt->second, inst.location)) {
            return fail("指令源码位置与 PC 映射不一致: " +
                        std::to_string(pc));
        }
        if (validateBytecode) {
            if (pc >= executableInstructionCount) {
                return fail("调试指令 PC 超出真实可执行字节码范围: " +
                            std::to_string(pc));
            }
            std::string expected = bytecode[pc];
            std::string actual = inst.opcode;
            std::transform(expected.begin(), expected.end(), expected.begin(),
                           [](unsigned char ch) {
                               return static_cast<char>(std::tolower(ch));
                           });
            std::transform(actual.begin(), actual.end(), actual.begin(),
                           [](unsigned char ch) {
                               return static_cast<char>(std::tolower(ch));
                           });
            if (actual != expected) {
                return fail("调试指令与真实字节码不一致: PC " +
                            std::to_string(pc));
            }
        }
    }

    std::unordered_set<const ScopeDebugInfo*> scopeSet;
    for (const auto& scope : scopes) {
        if (!scope || !scopeSet.insert(scope.get()).second) {
            return fail("作用域列表包含空项或重复项");
        }
    }

    for (const auto& [name, func] : functions) {
        if (name.empty() || func.name != name) {
            return fail("函数索引名称不一致");
        }
        if (func.startPC > func.endPC) {
            return fail("函数范围反向: " + name);
        }
        if (validateBytecode && func.endPC > executableInstructionCount) {
            return fail("函数范围超出真实可执行字节码: " + name);
        }
        if (!func.scope || func.scope->type != ScopeType::FUNCTION ||
            scopeSet.find(func.scope.get()) == scopeSet.end() ||
            func.scope->name != name ||
            func.scope->startPC != func.startPC ||
            func.scope->endPC != func.endPC) {
            return fail("函数与函数作用域不一致: " + name);
        }
    }

    if (!globalScope ||
        std::find(scopes.begin(), scopes.end(), globalScope) == scopes.end()) {
        return fail("缺少全局作用域");
    }

    size_t globalScopeCount = 0;
    auto containsScope = [&](const std::shared_ptr<ScopeDebugInfo>& scope) {
        return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
    };

    for (const auto& scope : scopes) {
        if (!scope || scope->startPC > scope->endPC) {
            return fail("作用域范围反向");
        }
        if (validateBytecode && scope->type != ScopeType::GLOBAL &&
            scope->endPC > executableInstructionCount) {
            return fail("作用域范围超出真实可执行字节码: " + scope->name);
        }

        for (const auto& var : scope->variables) {
            if (var.name.empty() || var.type.empty() ||
                var.scopeName != scope->name || var.declLine == 0) {
                return fail("变量声明与所属作用域不一致: " + var.name);
            }
        }

        if (scope->type == ScopeType::GLOBAL) {
            ++globalScopeCount;
            if (scope != globalScope || scope->parent) {
                return fail("全局作用域不唯一或包含父作用域");
            }
            continue;
        }

        if (!scope->parent || !containsScope(scope->parent)) {
            return fail("作用域缺少有效父作用域: " + scope->name);
        }
        if (!scope->location.isValid()) {
            return fail("非全局作用域缺少有效源码位置: " + scope->name);
        }

        const ScopeType parentType = scope->parent->type;
        bool validParent = false;
        switch (scope->type) {
            case ScopeType::CONTRACT:
                validParent = parentType == ScopeType::GLOBAL;
                break;
            case ScopeType::FUNCTION:
                validParent = parentType == ScopeType::GLOBAL;
                break;
            case ScopeType::BLOCK:
            case ScopeType::LOOP:
            case ScopeType::CONDITIONAL:
                validParent = parentType == ScopeType::FUNCTION ||
                              parentType == ScopeType::BLOCK ||
                              parentType == ScopeType::LOOP ||
                              parentType == ScopeType::CONDITIONAL;
                break;
            case ScopeType::GLOBAL:
                break;
        }
        if (!validParent) {
            return fail("非法的作用域父子类型: " + scope->name);
        }
        if (scope->type == ScopeType::FUNCTION &&
            functions.find(scope->name) == functions.end()) {
            return fail("函数作用域没有对应函数记录: " + scope->name);
        }

        if (scope->parent->type != ScopeType::GLOBAL &&
            (scope->startPC < scope->parent->startPC ||
             scope->endPC > scope->parent->endPC)) {
            return fail("子作用域范围超出父作用域: " + scope->name);
        }

        // parent 链必须最终到达 global，且不能形成环。
        std::vector<const ScopeDebugInfo*> ancestry;
        auto current = scope;
        while (current) {
            if (std::find(ancestry.begin(), ancestry.end(), current.get()) !=
                ancestry.end()) {
                return fail("作用域父链形成环: " + scope->name);
            }
            ancestry.push_back(current.get());
            current = current->parent;
        }
        if (ancestry.empty() || ancestry.back() != globalScope.get()) {
            return fail("作用域父链未到达全局作用域: " + scope->name);
        }
    }

    if (globalScopeCount != 1) {
        return fail("全局作用域数量不是 1");
    }

    // children 必须与 parent 双向一致，兄弟运行时范围不能部分交叉。
    for (const auto& parent : scopes) {
        std::unordered_set<const ScopeDebugInfo*> childSet;
        for (const auto& child : parent->children) {
            if (!child || !containsScope(child) || child->parent != parent ||
                !childSet.insert(child.get()).second) {
                return fail("作用域 children 与 parent 不一致: " +
                            parent->name);
            }
        }
        for (const auto& candidate : scopes) {
            if (candidate->parent == parent &&
                childSet.find(candidate.get()) == childSet.end()) {
                return fail("父作用域缺少 child 反向引用: " + parent->name);
            }
        }

        for (size_t i = 0; i < parent->children.size(); ++i) {
            for (size_t j = i + 1; j < parent->children.size(); ++j) {
                const auto& left = parent->children[i];
                const auto& right = parent->children[j];
                if (!rangesOverlap(*left, *right)) {
                    continue;
                }

                // 私有函数按运行时内联生成，因此全局函数实例可能完全
                // 包含于 public 函数范围；部分交叉仍然是损坏的数据。
                const bool nestedFunctionRanges =
                    left->type == ScopeType::FUNCTION &&
                    right->type == ScopeType::FUNCTION &&
                    (rangeContains(*left, *right) ||
                     rangeContains(*right, *left));
                if (!nestedFunctionRanges) {
                    return fail("兄弟作用域范围交叉: " + left->name +
                                " / " + right->name);
                }
            }
        }
    }

    auto scopeContainsVariable = [&](const VariableDebugInfo& variable) {
        for (const auto& scope : scopes) {
            if (scope->name != variable.scopeName) {
                continue;
            }
            if (std::any_of(
                    scope->variables.begin(),
                    scope->variables.end(),
                    [&](const VariableDebugInfo& candidate) {
                        return sameVariable(variable, candidate);
                    }
                )) {
                return true;
            }
        }
        return false;
    };

    for (const auto& [name, variable] : variables) {
        if (name != variable.name || !scopeContainsVariable(variable)) {
            return fail("全局变量索引包含幽灵变量: " + name);
        }
    }

    for (const auto& [name, func] : functions) {
        for (const auto& parameter : func.parameters) {
            if (!parameter.isParameter ||
                parameter.scopeName != func.scope->name ||
                !std::any_of(
                    func.scope->variables.begin(),
                    func.scope->variables.end(),
                    [&](const VariableDebugInfo& candidate) {
                        return sameVariable(parameter, candidate);
                    }
                )) {
                return fail("函数参数不属于函数作用域: " + name);
            }
        }
        for (const auto& local : func.localVars) {
            bool foundInFunction = false;
            for (const auto& scope : scopes) {
                bool belongsToFunction = false;
                auto ancestor = scope;
                while (ancestor) {
                    if (ancestor == func.scope) {
                        belongsToFunction = true;
                        break;
                    }
                    ancestor = ancestor->parent;
                }
                if (!belongsToFunction || scope->name != local.scopeName) {
                    continue;
                }
                foundInFunction = std::any_of(
                    scope->variables.begin(),
                    scope->variables.end(),
                    [&](const VariableDebugInfo& candidate) {
                        return sameVariable(local, candidate);
                    }
                );
                if (foundInFunction) {
                    break;
                }
            }
            if (!foundInFunction) {
                return fail("函数局部变量缺少所属作用域: " + name +
                            "/" + local.name);
            }
        }
    }

    return true;
}

// ===== JSON 序列化 =====

bool DebugInfo::save(const std::string& filename) const
{
    try {
        std::ofstream file(filename);
        if (!file.is_open()) {
            return false;
        }

        file << toJson();
        file.close();
        return true;
    } catch (...) {
        return false;
    }
}

std::shared_ptr<DebugInfo> DebugInfo::load(const std::string& filename)
{
    try {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return nullptr;
        }

        std::string content(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>()
        );
        file.close();

        return fromJson(content);
    } catch (...) {
        return nullptr;
    }
}

std::string DebugInfo::toJson() const
{
    json j;

    j["version"] = "1.0";
    j["format"] = "apc-debug";
    j["sourceFile"] = sourceFilename;
    j["contractName"] = contractName;
    j["scopeNestingValid"] = scopeNestingValid;

    json pcToSourceJson = json::object();
    for (const auto& [pc, loc] : pcToSource) {
        pcToSourceJson[std::to_string(pc)] = sourceLocationToJson(loc);
    }
    j["pcToSource"] = pcToSourceJson;

    json lineToPCJson = json::object();
    for (const auto& [line, pcs] : lineToPCs) {
        lineToPCJson[std::to_string(line)] = pcs;
    }
    j["lineToPC"] = lineToPCJson;

    json instructionsJson = json::array();
    for (const auto& [pc, inst] : instructions) {
        json instJson;
        instJson["pc"] = inst.pc;
        instJson["opcode"] = inst.opcode;
        instJson["operand"] = inst.operand;
        instJson["affectedVars"] = inst.affectedVars;
        instJson["location"] = sourceLocationToJson(inst.location);
        instructionsJson.push_back(instJson);
    }
    j["instructions"] = instructionsJson;

    json functionsJson = json::array();
    for (const auto& [name, func] : functions) {
        json funcJson;
        funcJson["name"] = func.name;
        funcJson["startPC"] = func.startPC;
        funcJson["endPC"] = func.endPC;
        funcJson["isPublic"] = func.isPublic;
        funcJson["location"] = sourceLocationToJson(func.location);

        json paramsJson = json::array();
        for (const auto& param : func.parameters) {
            paramsJson.push_back(variableToJson(param));
        }
        funcJson["parameters"] = paramsJson;

        json localsJson = json::array();
        for (const auto& local : func.localVars) {
            localsJson.push_back(variableToJson(local));
        }
        funcJson["localVars"] = localsJson;

        functionsJson.push_back(funcJson);
    }
    j["functions"] = functionsJson;

    json localVarsJson = json::array();
    for (const auto& [name, func] : functions) {
        for (const auto& local : func.localVars) {
            localVarsJson.push_back(variableToJson(local));
        }
    }
    j["localVars"] = localVarsJson;

    json variablesJson = json::array();
    for (const auto& [name, var] : variables) {
        variablesJson.push_back(variableToJson(var));
    }
    j["variables"] = variablesJson;

    std::map<const ScopeDebugInfo*, size_t> scopeIndices;
    for (size_t i = 0; i < scopes.size(); ++i) {
        scopeIndices[scopes[i].get()] = i;
    }

    json scopesJson = json::array();
    for (size_t i = 0; i < scopes.size(); ++i) {
        const auto& scope = scopes[i];
        json scopeJson;
        scopeJson["index"] = i;
        scopeJson["name"] = scope->name;
        scopeJson["type"] = scopeTypeToString(scope->type);
        scopeJson["location"] = sourceLocationToJson(scope->location);
        scopeJson["startPC"] = scope->startPC;
        scopeJson["endPC"] = scope->endPC;
        scopeJson["parentIndex"] =
            scope->parent ? static_cast<int>(scopeIndices[scope->parent.get()])
                          : -1;

        json scopeVars = json::array();
        for (const auto& var : scope->variables) {
            scopeVars.push_back(variableToJson(var));
        }
        scopeJson["variables"] = scopeVars;
        scopesJson.push_back(scopeJson);
    }
    j["scopes"] = scopesJson;

    return j.dump(2);
}

std::shared_ptr<DebugInfo> DebugInfo::fromJson(const std::string& jsonStr)
{
    try {
        auto info = std::make_shared<DebugInfo>();
        json j = json::parse(jsonStr);

        info->version = j.value("version", "1.0");
        info->sourceFilename = j.value("sourceFile", "");
        info->contractName = j.value("contractName", "");
        // 旧文件若没有显式平衡标记，无法证明 enter/exit 配对，默认拒绝。
        info->scopeNestingValid = j.value("scopeNestingValid", false);

        if (j.contains("pcToSource")) {
            for (auto& [pcStr, locJson] : j["pcToSource"].items()) {
                size_t pc = std::stoull(pcStr);
                info->pcToSource[pc] = sourceLocationFromJson(locJson);
            }
        }

        if (j.contains("lineToPC")) {
            for (auto& [lineStr, pcsJson] : j["lineToPC"].items()) {
                size_t line = std::stoull(lineStr);
                std::vector<size_t> pcs = pcsJson.get<std::vector<size_t>>();
                info->lineToPCs[line] = pcs;
            }
        }

        if (j.contains("instructions")) {
            for (const auto& instJson : j["instructions"]) {
                InstructionDebugInfo inst;
                inst.pc = instJson.value("pc", 0);
                inst.opcode = instJson.value("opcode", "");
                inst.operand = instJson.value("operand", "");
                if (instJson.contains("location")) {
                    inst.location = sourceLocationFromJson(instJson["location"]);
                } else {
                    auto locIt = info->pcToSource.find(inst.pc);
                    if (locIt != info->pcToSource.end()) {
                        inst.location = locIt->second;
                    }
                }
                if (instJson.contains("affectedVars")) {
                    inst.affectedVars = instJson["affectedVars"]
                                            .get<std::vector<std::string>>();
                }
                info->instructions[inst.pc] = inst;
            }
        }

        if (j.contains("functions")) {
            for (const auto& funcJson : j["functions"]) {
                FunctionDebugInfo func;
                func.name = funcJson.value("name", "");
                func.startPC = funcJson.value("startPC", 0);
                func.endPC = funcJson.value("endPC", 0);
                func.isPublic = funcJson.value("isPublic", false);
                if (funcJson.contains("location")) {
                    func.location = sourceLocationFromJson(funcJson["location"]);
                }

                if (funcJson.contains("parameters")) {
                    for (const auto& paramJson : funcJson["parameters"]) {
                        VariableDebugInfo param = variableFromJson(paramJson);
                        param.isParameter = true;
                        func.parameters.push_back(param);
                    }
                }
                if (funcJson.contains("localVars")) {
                    for (const auto& localJson : funcJson["localVars"]) {
                        func.localVars.push_back(variableFromJson(localJson));
                    }
                }

                info->functions[func.name] = func;
            }
        }

        if (j.contains("variables")) {
            for (const auto& varJson : j["variables"]) {
                VariableDebugInfo var = variableFromJson(varJson);
                info->variables[var.name] = var;
            }
        }

        std::vector<int> parentIndices;
        if (j.contains("scopes")) {
            for (const auto& scopeJson : j["scopes"]) {
                auto scope = std::make_shared<ScopeDebugInfo>();
                scope->name = scopeJson.value("name", "");
                scope->type =
                    stringToScopeType(scopeJson.value("type", "block"));
                if (scopeJson.contains("location")) {
                    scope->location =
                        sourceLocationFromJson(scopeJson["location"]);
                }
                scope->startPC = scopeJson.value("startPC", 0);
                scope->endPC = scopeJson.value("endPC", 0);
                if (scopeJson.contains("variables")) {
                    for (const auto& varJson : scopeJson["variables"]) {
                        scope->variables.push_back(variableFromJson(varJson));
                    }
                }
                parentIndices.push_back(scopeJson.value("parentIndex", -1));
                info->scopes.push_back(scope);
                if (scope->type == ScopeType::GLOBAL) {
                    info->globalScope = scope;
                }
            }

            for (size_t i = 0; i < info->scopes.size(); ++i) {
                int parentIndex = parentIndices[i];
                if (parentIndex >= 0 &&
                    static_cast<size_t>(parentIndex) < info->scopes.size()) {
                    info->scopes[i]->parent = info->scopes[parentIndex];
                    info->scopes[parentIndex]->children.push_back(info->scopes[i]);
                }
            }

            for (auto& [name, func] : info->functions) {
                for (const auto& scope : info->scopes) {
                    if (scope->type == ScopeType::FUNCTION &&
                        scope->name == name &&
                        scope->startPC == func.startPC) {
                        func.scope = scope;
                        break;
                    }
                }
            }
        }

        if (info->lineToPCs.empty()) {
            info->buildLineToPC();
        } else {
            for (auto& [line, pcs] : info->lineToPCs) {
                std::sort(pcs.begin(), pcs.end());
                pcs.erase(std::unique(pcs.begin(), pcs.end()), pcs.end());
            }
        }

        return info;
    } catch (...) {
        return nullptr;
    }
}

std::string scopeTypeToString(ScopeType type)
{
    switch (type) {
        case ScopeType::GLOBAL:
            return "global";
        case ScopeType::CONTRACT:
            return "contract";
        case ScopeType::FUNCTION:
            return "function";
        case ScopeType::BLOCK:
            return "block";
        case ScopeType::LOOP:
            return "loop";
        case ScopeType::CONDITIONAL:
            return "conditional";
        default:
            return "unknown";
    }
}

ScopeType stringToScopeType(const std::string& str)
{
    if (str == "global")
        return ScopeType::GLOBAL;
    if (str == "contract")
        return ScopeType::CONTRACT;
    if (str == "function")
        return ScopeType::FUNCTION;
    if (str == "block")
        return ScopeType::BLOCK;
    if (str == "loop")
        return ScopeType::LOOP;
    if (str == "conditional")
        return ScopeType::CONDITIONAL;
    return ScopeType::BLOCK;
}

const VariableDebugInfo* DebugInfo::getVariableInfo(const std::string& varName
) const
{
    auto it = variables.find(varName);
    if (it != variables.end()) {
        return &(it->second);
    }
    return nullptr;
}

} // namespace apc_debug

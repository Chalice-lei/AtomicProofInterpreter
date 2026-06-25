#include "debug_info.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace apc_debug
{
namespace
{
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

std::vector<size_t>
DebugInfo::findNearestValidLine(size_t line, size_t maxDistance) const
{
    auto pcs = getPCsForLine(line);
    if (!pcs.empty()) {
        return pcs;
    }

    // 在附近查找；上限 10000 行用作粗略保护
    for (size_t dist = 1; dist <= maxDistance; ++dist) {
        if (line + dist <= 10000) {
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

bool DebugInfo::validate() const
{
    if (sourceFilename.empty()) {
        return false;
    }

    for (const auto& [pc, loc] : pcToSource) {
        if (!loc.isValid()) {
            return false;
        }
    }

    for (const auto& [name, func] : functions) {
        if (func.startPC >= func.endPC) {
            return false;
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

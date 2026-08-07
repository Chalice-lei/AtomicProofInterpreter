#include "debug_info.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
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

bool sameLocation(const SourceLocation& lhs, const SourceLocation& rhs)
{
    return lhs.filename == rhs.filename && lhs.line == rhs.line &&
           lhs.column == rhs.column && lhs.endLine == rhs.endLine &&
           lhs.endColumn == rhs.endColumn;
}

bool sameSourceLocation(
    const SourceLocation& lhs,
    const SourceLocation& rhs
)
{
    return sameLocation(lhs, rhs);
}

bool sameOrigin(const SourceOrigin& lhs, const SourceOrigin& rhs)
{
    return sameLocation(lhs.location, rhs.location) &&
           lhs.scopeId == rhs.scopeId &&
           lhs.functionName == rhs.functionName &&
           lhs.originalPC == rhs.originalPC && lhs.path == rhs.path &&
           lhs.affectedVars == rhs.affectedVars;
}

void appendUniqueOrigin(
    std::vector<SourceOrigin>& origins,
    const SourceOrigin& origin
)
{
    if (std::none_of(origins.begin(), origins.end(), [&](const auto& item) {
            return sameOrigin(item, origin);
        })) {
        origins.push_back(origin);
    }
}

std::vector<PCRange> normalizeRanges(std::vector<PCRange> ranges)
{
    ranges.erase(
        std::remove_if(ranges.begin(), ranges.end(), [](const PCRange& range) {
            return !range.isValid();
        }),
        ranges.end()
    );
    std::sort(ranges.begin(), ranges.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.beginPC != rhs.beginPC) {
            return lhs.beginPC < rhs.beginPC;
        }
        return lhs.endPC < rhs.endPC;
    });

    std::vector<PCRange> result;
    for (const auto& range : ranges) {
        if (result.empty() || range.beginPC > result.back().endPC) {
            result.push_back(range);
            continue;
        }
        result.back().endPC = std::max(result.back().endPC, range.endPC);
    }
    return result;
}

std::vector<PCRange> effectiveRanges(const ScopeDebugInfo& scope)
{
    if (!scope.ranges.empty()) {
        return normalizeRanges(scope.ranges);
    }
    if (scope.startPC < scope.endPC) {
        return {PCRange(scope.startPC, scope.endPC)};
    }
    return {};
}

size_t scopeDepth(const std::shared_ptr<ScopeDebugInfo>& scope)
{
    size_t depth = 0;
    auto current = scope;
    std::unordered_set<const ScopeDebugInfo*> visited;
    while (current && visited.insert(current.get()).second) {
        ++depth;
        current = current->parent.lock();
    }
    return depth;
}

std::string branchArmToString(BranchArm arm)
{
    return arm == BranchArm::Then ? "then" : "else";
}

BranchArm branchArmFromString(const std::string& value)
{
    if (value == "then") {
        return BranchArm::Then;
    }
    if (value == "else") {
        return BranchArm::Else;
    }
    throw std::invalid_argument("unknown debug branch arm: " + value);
}

json sourceLocationToJson(const SourceLocation& loc)
{
    return json{{"file", loc.filename},
                {"line", loc.line},
                {"column", loc.column},
                {"endLine", loc.endLine},
                {"endColumn", loc.endColumn}};
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

json branchPathToJson(const std::vector<BranchPredicate>& path)
{
    json result = json::array();
    for (const auto& predicate : path) {
        result.push_back(
            {{"region", predicate.region},
             {"arm", branchArmToString(predicate.arm)}}
        );
    }
    return result;
}

std::vector<BranchPredicate> branchPathFromJson(const json& pathJson)
{
    std::vector<BranchPredicate> result;
    if (!pathJson.is_array()) {
        throw std::invalid_argument("debug branch path must be an array");
    }
    for (const auto& item : pathJson) {
        if (!item.is_object() || !item.contains("region") ||
            !item.contains("arm") || !item["arm"].is_string()) {
            throw std::invalid_argument("invalid debug branch predicate");
        }
        BranchPredicate predicate;
        predicate.region = item.at("region").get<ControlRegionId>();
        predicate.arm =
            branchArmFromString(item.at("arm").get<std::string>());
        result.push_back(predicate);
    }
    return result;
}

json sourceOriginToJson(const SourceOrigin& origin)
{
    json result{{"location", sourceLocationToJson(origin.location)},
                {"scopeId", origin.scopeId},
                {"functionName", origin.functionName},
                {"path", branchPathToJson(origin.path)},
                {"affectedVars", origin.affectedVars}};
    if (origin.originalPC != UNKNOWN_ORIGINAL_PC) {
        result["originalPC"] = origin.originalPC;
    }
    return result;
}

SourceOrigin sourceOriginFromJson(const json& originJson)
{
    SourceOrigin result;
    if (originJson.contains("location")) {
        result.location = sourceLocationFromJson(originJson["location"]);
    }
    result.scopeId = originJson.value("scopeId", INVALID_SCOPE_ID);
    result.functionName = originJson.value("functionName", "");
    result.originalPC =
        originJson.value("originalPC", UNKNOWN_ORIGINAL_PC);
    if (originJson.contains("path")) {
        result.path = branchPathFromJson(originJson["path"]);
    }
    if (originJson.contains("affectedVars")) {
        result.affectedVars =
            originJson["affectedVars"].get<std::vector<std::string>>();
    }
    return result;
}

json variableToJson(const VariableDebugInfo& var)
{
    json result{{"name", var.name},
                {"type", var.type},
                {"scopeName", var.scopeName},
                {"scopeId", var.scopeId},
                {"declLine", var.declLine},
                {"declColumn", var.declColumn},
                {"isStackVar", var.isStackVar},
                {"stackOffset", var.stackOffset},
                {"isParameter", var.isParameter}};
    if (var.hasExplicitAvailability) {
        result["availabilityRanges"] = json::array();
        for (const auto& range : var.availabilityRanges) {
            result["availabilityRanges"].push_back(
                {{"beginPC", range.beginPC}, {"endPC", range.endPC}}
            );
        }
    }
    return result;
}

VariableDebugInfo variableFromJson(const json& varJson)
{
    VariableDebugInfo var;
    var.name = varJson.value("name", "");
    var.type = varJson.value("type", "");
    var.scopeName = varJson.value("scopeName", "");
    var.scopeId = varJson.value("scopeId", INVALID_SCOPE_ID);
    var.declLine = varJson.value("declLine", 0);
    var.declColumn = varJson.value("declColumn", 0);
    var.isStackVar = varJson.value("isStackVar", true);
    var.stackOffset = varJson.value("stackOffset", -1);
    var.isParameter = varJson.value("isParameter", false);
    const char* rangeKey = nullptr;
    if (varJson.contains("availabilityRanges")) {
        rangeKey = "availabilityRanges";
    } else if (varJson.contains("liveRanges")) {
        // Accept the early V2 spelling while emitting one canonical key.
        rangeKey = "liveRanges";
    }
    if (rangeKey != nullptr) {
        const auto& rangesJson = varJson.at(rangeKey);
        if (!rangesJson.is_array()) {
            throw std::invalid_argument(
                "variable availability ranges must be an array"
            );
        }
        var.hasExplicitAvailability = true;
        for (const auto& rangeJson : rangesJson) {
            if (!rangeJson.is_object() ||
                !rangeJson.contains("beginPC") ||
                !rangeJson.contains("endPC")) {
                throw std::invalid_argument(
                    "invalid variable availability range"
                );
            }
            var.availabilityRanges.emplace_back(
                rangeJson.at("beginPC").get<size_t>(),
                rangeJson.at("endPC").get<size_t>()
            );
        }
    }
    return var;
}

void mergePath(
    std::vector<BranchPredicate>& destination,
    const std::vector<BranchPredicate>& additional
)
{
    for (const auto& predicate : additional) {
        auto existing = std::find_if(
            destination.begin(),
            destination.end(),
            [&](const BranchPredicate& item) {
                return item.region == predicate.region;
            }
        );
        if (existing == destination.end()) {
            destination.push_back(predicate);
        } else {
            existing->arm = predicate.arm;
        }
    }
}

bool hasDuplicatePathRegion(const std::vector<BranchPredicate>& path)
{
    std::set<ControlRegionId> regions;
    for (const auto& predicate : path) {
        if (!regions.insert(predicate.region).second) {
            return true;
        }
    }
    return false;
}

bool isStructuredIfOpcode(const std::string& opcode)
{
    std::string normalized;
    normalized.reserve(opcode.size());
    for (unsigned char ch : opcode) {
        if (!std::isspace(ch)) {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return normalized == "63" || normalized == "64" ||
           normalized == "op_if" || normalized == "op_notif";
}

bool isPathCompatible(
    const SourceOrigin& origin,
    const BranchTrace& branchTrace
)
{
    return std::all_of(
        origin.path.begin(),
        origin.path.end(),
        [&](const BranchPredicate& predicate) {
            const auto selected = branchTrace.find(predicate.region);
            return selected != branchTrace.end() &&
                   selected->second == predicate.arm;
        }
    );
}

bool validVariableRanges(const VariableDebugInfo& variable)
{
    if (!variable.hasExplicitAvailability) {
        return variable.availabilityRanges.empty();
    }
    size_t previousEnd = 0;
    bool first = true;
    for (const auto& range : variable.availabilityRanges) {
        if (!range.isValid() || (!first && range.beginPC < previousEnd)) {
            return false;
        }
        first = false;
        previousEnd = range.endPC;
    }
    return true;
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

bool rangesOverlap(
    const ScopeDebugInfo& left,
    const ScopeDebugInfo& right
)
{
    // Empty scopes are source anchors and cover no runtime PC.
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

bool VariableDebugInfo::isAvailableAtPC(size_t pc) const
{
    if (!hasExplicitAvailability) {
        return true;
    }
    return std::any_of(
        availabilityRanges.begin(),
        availabilityRanges.end(),
        [&](const PCRange& range) { return range.contains(pc); }
    );
}

void VariableDebugInfo::setAvailabilityRange(size_t begin, size_t end)
{
    hasExplicitAvailability = true;
    availabilityRanges.clear();
    if (begin < end) {
        availabilityRanges.emplace_back(begin, end);
    }
}

void ScopeDebugInfo::addVariable(const VariableDebugInfo& var)
{
    VariableDebugInfo copy = var;
    if (copy.scopeId == INVALID_SCOPE_ID) {
        copy.scopeId = scopeId;
    }
    if (copy.scopeName.empty()) {
        copy.scopeName = name;
    }
    variables.push_back(std::move(copy));
}

void ScopeDebugInfo::setRange(size_t begin, size_t end)
{
    ranges.clear();
    startPC = begin;
    endPC = end;
    if (begin < end) {
        ranges.emplace_back(begin, end);
    }
}

void ScopeDebugInfo::addRange(size_t begin, size_t end)
{
    if (begin >= end) {
        return;
    }
    ranges.emplace_back(begin, end);
    ranges = normalizeRanges(std::move(ranges));
    startPC = ranges.front().beginPC;
    endPC = ranges.back().endPC;
}

bool ScopeDebugInfo::containsPC(size_t pc) const
{
    const auto intervals = effectiveRanges(*this);
    return std::any_of(intervals.begin(), intervals.end(), [&](const auto& range) {
        return range.contains(pc);
    });
}

size_t ScopeDebugInfo::coveredInstructionCount() const
{
    size_t total = 0;
    for (const auto& range : effectiveRanges(*this)) {
        total += range.endPC - range.beginPC;
    }
    return total;
}

const VariableDebugInfo* ScopeDebugInfo::findVariable(const std::string& value
) const
{
    const ScopeDebugInfo* current = this;
    std::shared_ptr<ScopeDebugInfo> parentOwner;
    std::unordered_set<const ScopeDebugInfo*> visited;
    while (current && visited.insert(current).second) {
        for (const auto& variable : current->variables) {
            if (variable.name == value) {
                return &variable;
            }
        }
        parentOwner = current->parent.lock();
        current = parentOwner.get();
    }
    return nullptr;
}

const VariableDebugInfo* ScopeDebugInfo::findVariable(
    const std::string& value,
    size_t pc
) const
{
    const ScopeDebugInfo* current = this;
    std::shared_ptr<ScopeDebugInfo> parentOwner;
    std::unordered_set<const ScopeDebugInfo*> visited;
    while (current && visited.insert(current).second) {
        for (const auto& variable : current->variables) {
            if (variable.name == value) {
                // An unavailable inner binding still shadows a parent
                // binding for the remainder of its lexical scope.
                return variable.isAvailableAtPC(pc) ? &variable : nullptr;
            }
        }
        parentOwner = current->parent.lock();
        current = parentOwner.get();
    }
    return nullptr;
}

void DebugInfo::addSourceMapping(size_t pc, const SourceLocation& loc)
{
    if (!loc.isValid()) {
        return;
    }
    pcToSource[pc] = loc;
    auto instruction = instructions.find(pc);
    if (instruction != instructions.end()) {
        instruction->second.location = loc;
        if (instruction->second.origins.empty()) {
            SourceOrigin origin(loc);
            origin.originalPC = pc;
            origin.affectedVars = instruction->second.affectedVars;
            instruction->second.origins.push_back(std::move(origin));
        } else {
            instruction->second.origins.front().location = loc;
        }
    }
    buildLineToPC();
}

void DebugInfo::addInstruction(const InstructionDebugInfo& value)
{
    InstructionDebugInfo info = value;
    if (info.origins.empty() && info.location.isValid()) {
        SourceOrigin origin(info.location);
        origin.originalPC = info.pc;
        origin.affectedVars = info.affectedVars;
        info.origins.push_back(std::move(origin));
    }
    if (!info.origins.empty()) {
        info.location = info.origins.front().location;
        if (info.affectedVars.empty()) {
            info.affectedVars = info.origins.front().affectedVars;
        }
    }

    instructions[info.pc] = info;
    if (info.location.isValid()) {
        pcToSource[info.pc] = info.location;
    } else {
        pcToSource.erase(info.pc);
    }
    buildLineToPC();
}

void DebugInfo::addInstruction(
    size_t pc,
    const std::string& opcode,
    const std::string& operand,
    const SourceLocation& loc
)
{
    InstructionDebugInfo info(pc, loc);
    info.opcode = opcode;
    info.operand = operand;
    addInstruction(info);
}

void DebugInfo::addInstructionOrigin(size_t pc, const SourceOrigin& value)
{
    auto [it, inserted] = instructions.emplace(pc, InstructionDebugInfo());
    auto& instruction = it->second;
    if (inserted) {
        instruction.pc = pc;
    }
    appendUniqueOrigin(instruction.origins, value);
    if (!instruction.origins.empty()) {
        instruction.location = instruction.origins.front().location;
        instruction.affectedVars = instruction.origins.front().affectedVars;
        if (instruction.location.isValid()) {
            pcToSource[pc] = instruction.location;
        }
    }
    buildLineToPC();
}

void DebugInfo::setInstructionOrigins(
    size_t pc,
    const std::vector<SourceOrigin>& values
)
{
    auto [it, inserted] = instructions.emplace(pc, InstructionDebugInfo());
    auto& instruction = it->second;
    if (inserted) {
        instruction.pc = pc;
    }
    instruction.origins.clear();
    for (const auto& origin : values) {
        appendUniqueOrigin(instruction.origins, origin);
    }
    if (instruction.origins.empty()) {
        instruction.location = SourceLocation();
        instruction.affectedVars.clear();
        pcToSource.erase(pc);
    } else {
        instruction.location = instruction.origins.front().location;
        instruction.affectedVars = instruction.origins.front().affectedVars;
        if (instruction.location.isValid()) {
            pcToSource[pc] = instruction.location;
        } else {
            pcToSource.erase(pc);
        }
    }
    buildLineToPC();
}

void DebugInfo::addFunction(const FunctionDebugInfo& info)
{
    functions[info.name] = info;
}

void DebugInfo::addVariable(const VariableDebugInfo& info)
{
    // Kept for V1 callers. Scope-aware queries use ScopeDebugInfo::variables,
    // while this map remains the legacy name-only fallback.
    variables[info.name] = info;
}

void DebugInfo::addScope(std::shared_ptr<ScopeDebugInfo> scope)
{
    if (!scope) {
        return;
    }
    if (scope->scopeId == INVALID_SCOPE_ID) {
        ScopeId nextId = 1;
        for (const auto& existing : scopes) {
            if (existing) {
                nextId = std::max(nextId, existing->scopeId + 1);
            }
        }
        scope->scopeId = nextId;
    }
    for (auto& var : scope->variables) {
        if (var.scopeId == INVALID_SCOPE_ID) {
            var.scopeId = scope->scopeId;
        }
        if (var.scopeName.empty()) {
            var.scopeName = scope->name;
        }
    }
    scopes.push_back(scope);
    if (scope->type == ScopeType::GLOBAL) {
        globalScope = scope;
    }
}

std::vector<SourceOrigin> DebugInfo::getOriginsForPC(size_t pc) const
{
    auto instruction = instructions.find(pc);
    if (instruction != instructions.end()) {
        if (!instruction->second.origins.empty()) {
            return instruction->second.origins;
        }
        if (instruction->second.location.isValid()) {
            SourceOrigin origin(instruction->second.location);
            origin.originalPC = pc;
            origin.affectedVars = instruction->second.affectedVars;
            return {origin};
        }
    }

    auto source = pcToSource.find(pc);
    if (source != pcToSource.end()) {
        SourceOrigin origin(source->second);
        origin.originalPC = pc;
        return {origin};
    }
    return {};
}

const SourceOrigin* DebugInfo::resolveOrigin(
    size_t pc,
    const BranchTrace& branchTrace
) const
{
    auto instruction = instructions.find(pc);
    if (instruction == instructions.end() ||
        instruction->second.origins.empty()) {
        return nullptr;
    }
    const auto& origins = instruction->second.origins;
    if (branchTrace.empty()) {
        return &origins.front();
    }

    const SourceOrigin* best = nullptr;
    size_t bestMatches = 0;
    size_t bestSpecificity = 0;
    for (const auto& origin : origins) {
        bool conflicts = false;
        size_t matches = 0;
        for (const auto& predicate : origin.path) {
            auto actual = branchTrace.find(predicate.region);
            if (actual == branchTrace.end()) {
                conflicts = true;
                break;
            }
            if (actual->second != predicate.arm) {
                conflicts = true;
                break;
            }
            ++matches;
        }
        if (conflicts) {
            continue;
        }
        if (!best || matches > bestMatches ||
            (matches == bestMatches && origin.path.size() > bestSpecificity)) {
            best = &origin;
            bestMatches = matches;
            bestSpecificity = origin.path.size();
        }
    }
    return best;
}

SourceLocation DebugInfo::getSourceLocation(size_t pc) const
{
    return getSourceLocation(pc, BranchTrace());
}

SourceLocation DebugInfo::getSourceLocation(
    size_t pc,
    const BranchTrace& branchTrace
) const
{
    if (const auto* origin = resolveOrigin(pc, branchTrace)) {
        return origin->location;
    }
    // A non-empty trace with path-qualified origins must not silently select
    // an origin from a branch that did not execute.
    auto instruction = instructions.find(pc);
    if (!branchTrace.empty() && instruction != instructions.end() &&
        !instruction->second.origins.empty()) {
        return SourceLocation();
    }
    auto it = pcToSource.find(pc);
    return it != pcToSource.end() ? it->second : SourceLocation();
}

bool DebugInfo::hasActiveSourceOrigin(
    size_t pc,
    const std::string& filename,
    size_t line,
    const BranchTrace& branchTrace
) const
{
    const std::string requested =
        normalizeSourceFilename(filename, sourceFilename);
    if (requested.empty()) {
        return false;
    }
    const auto origins = getOriginsForPC(pc);
    return std::any_of(
        origins.begin(),
        origins.end(),
        [&](const SourceOrigin& origin) {
            if (!origin.location.isValid() ||
                origin.location.line != line ||
                !isPathCompatible(origin, branchTrace)) {
                return false;
            }
            const std::string& mapped = origin.location.filename.empty()
                                            ? sourceFilename
                                            : origin.location.filename;
            return normalizeSourceFilename(mapped, sourceFilename) ==
                   requested;
        }
    );
}

std::vector<size_t> DebugInfo::getPCsForLine(size_t line) const
{
    auto it = lineToPCs.find(line);
    return it != lineToPCs.end() ? it->second : std::vector<size_t>();
}

std::vector<size_t> DebugInfo::getPCsForSourceLine(
    const std::string& filename,
    size_t line
) const
{
    const std::string requested =
        normalizeSourceFilename(filename, sourceFilename);
    if (requested.empty()) {
        return {};
    }

    std::vector<size_t> result;
    for (size_t pc : getPCsForLine(line)) {
        const auto origins = getOriginsForPC(pc);
        const bool matches = std::any_of(
            origins.begin(),
            origins.end(),
            [&](const SourceOrigin& origin) {
                if (origin.location.line != line) {
                    return false;
                }
                const std::string& mapped = origin.location.filename.empty()
                                                ? sourceFilename
                                                : origin.location.filename;
                return normalizeSourceFilename(mapped, sourceFilename) ==
                       requested;
            }
        );
        if (matches) {
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
    for (size_t distance = 1; distance <= maxDistance; ++distance) {
        if (line <= 10000 && distance <= 10000 - line) {
            pcs = getPCsForLine(line + distance);
            if (!pcs.empty()) {
                return pcs;
            }
        }
        if (line > distance) {
            pcs = getPCsForLine(line - distance);
            if (!pcs.empty()) {
                return pcs;
            }
        }
    }
    return {};
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
    for (size_t distance = 1; distance <= maxDistance; ++distance) {
        if (line <= 10000 && distance <= 10000 - line) {
            pcs = getPCsForSourceLine(filename, line + distance);
            if (!pcs.empty()) {
                return pcs;
            }
        }
        if (line > distance) {
            pcs = getPCsForSourceLine(filename, line - distance);
            if (!pcs.empty()) {
                return pcs;
            }
        }
    }
    return {};
}

const FunctionDebugInfo* DebugInfo::getFunctionAtPC(size_t pc) const
{
    for (const auto& [name, function] : functions) {
        (void)name;
        if (pc >= function.startPC && pc < function.endPC) {
            return &function;
        }
    }
    return nullptr;
}

std::shared_ptr<ScopeDebugInfo> DebugInfo::getScopeById(ScopeId id) const
{
    if (id == INVALID_SCOPE_ID) {
        return nullptr;
    }
    auto it = std::find_if(scopes.begin(), scopes.end(), [&](const auto& scope) {
        return scope && scope->scopeId == id;
    });
    return it != scopes.end() ? *it : nullptr;
}

std::shared_ptr<ScopeDebugInfo> DebugInfo::getScopeAtPC(size_t pc) const
{
    return getScopeAtPC(pc, BranchTrace());
}

std::shared_ptr<ScopeDebugInfo> DebugInfo::getScopeAtPC(
    size_t pc,
    const BranchTrace& branchTrace
) const
{
    if (branchTrace.empty()) {
        const auto origins = getOriginsForPC(pc);
        if (origins.size() > 1) {
            std::vector<std::shared_ptr<ScopeDebugInfo>> originScopes;
            originScopes.reserve(origins.size());
            for (const auto& origin : origins) {
                auto scope = getScopeById(origin.scopeId);
                if (!scope) {
                    // With incomplete V1 metadata, the global scope is the
                    // only conservative common visibility boundary.
                    return globalScope;
                }
                originScopes.push_back(std::move(scope));
            }

            std::unordered_set<const ScopeDebugInfo*> visited;
            for (auto candidate = originScopes.front(); candidate;
                 candidate = candidate->parent.lock()) {
                if (!visited.insert(candidate.get()).second) {
                    break;
                }
                const bool commonToAll = std::all_of(
                    originScopes.begin() + 1,
                    originScopes.end(),
                    [&](const std::shared_ptr<ScopeDebugInfo>& scope) {
                        std::unordered_set<const ScopeDebugInfo*> chainVisited;
                        for (auto ancestor = scope; ancestor;
                             ancestor = ancestor->parent.lock()) {
                            if (!chainVisited.insert(ancestor.get()).second) {
                                break;
                            }
                            if (ancestor->scopeId == candidate->scopeId) {
                                return true;
                            }
                        }
                        return false;
                    }
                );
                if (commonToAll) {
                    return candidate;
                }
            }
            return globalScope;
        }
    }

    if (const auto* origin = resolveOrigin(pc, branchTrace)) {
        if (auto scope = getScopeById(origin->scopeId)) {
            return scope;
        }
    } else if (!branchTrace.empty()) {
        auto instruction = instructions.find(pc);
        if (instruction != instructions.end() &&
            std::any_of(
                instruction->second.origins.begin(),
                instruction->second.origins.end(),
                [](const SourceOrigin& origin) { return !origin.path.empty(); }
            )) {
            return nullptr;
        }
    }

    std::shared_ptr<ScopeDebugInfo> result;
    size_t smallestCoverage = std::numeric_limits<size_t>::max();
    size_t deepest = 0;
    for (const auto& scope : scopes) {
        if (!scope || !scope->containsPC(pc)) {
            continue;
        }
        const size_t coverage = scope->coveredInstructionCount();
        const size_t depth = scopeDepth(scope);
        if (!result || coverage < smallestCoverage ||
            (coverage == smallestCoverage && depth > deepest)) {
            result = scope;
            smallestCoverage = coverage;
            deepest = depth;
        }
    }
    return result;
}

std::vector<VariableDebugInfo> DebugInfo::getVariablesInScope(size_t pc) const
{
    return getVariablesInScope(pc, BranchTrace());
}

std::vector<VariableDebugInfo> DebugInfo::getVariablesInScope(
    size_t pc,
    const BranchTrace& branchTrace
) const
{
    std::vector<VariableDebugInfo> result;
    std::set<std::string> visibleNames;
    auto current = getScopeAtPC(pc, branchTrace);
    std::unordered_set<const ScopeDebugInfo*> visited;
    while (current && visited.insert(current.get()).second) {
        for (const auto& variable : current->variables) {
            if (visibleNames.insert(variable.name).second &&
                variable.isAvailableAtPC(pc)) {
                result.push_back(variable);
            }
        }
        current = current->parent.lock();
    }
    return result;
}

const VariableDebugInfo* DebugInfo::getVariableInfo(const std::string& name
) const
{
    auto it = variables.find(name);
    return it != variables.end() ? &it->second : nullptr;
}

const VariableDebugInfo* DebugInfo::getVariableInfo(
    const std::string& name,
    size_t pc,
    const BranchTrace& branchTrace
) const
{
    auto scope = getScopeAtPC(pc, branchTrace);
    if (scope) {
        // A scope-aware lookup is authoritative. Falling back to the legacy
        // name-only map here would expose inactive branch locals or values
        // whose availability range has ended.
        return scope->findVariable(name, pc);
    }
    if (!branchTrace.empty()) {
        auto instruction = instructions.find(pc);
        if (instruction != instructions.end() &&
            std::any_of(
                instruction->second.origins.begin(),
                instruction->second.origins.end(),
                [](const SourceOrigin& origin) { return !origin.path.empty(); }
            )) {
            return nullptr;
        }
    }
    // V1 files without any scope data retain the legacy map fallback. V2
    // availability still applies if such a producer supplied it.
    const auto* legacy = getVariableInfo(name);
    return legacy && legacy->isAvailableAtPC(pc) ? legacy : nullptr;
}

void DebugInfo::buildLineToPC()
{
    lineToPCs.clear();
    std::set<size_t> instructionsWithOrigins;
    for (const auto& [pc, instruction] : instructions) {
        if (!instruction.origins.empty()) {
            instructionsWithOrigins.insert(pc);
            for (const auto& origin : instruction.origins) {
                if (origin.location.isValid()) {
                    lineToPCs[origin.location.line].push_back(pc);
                }
            }
        } else if (instruction.location.isValid()) {
            lineToPCs[instruction.location.line].push_back(pc);
        }
    }
    for (const auto& [pc, location] : pcToSource) {
        if (!instructionsWithOrigins.contains(pc) && location.isValid()) {
            lineToPCs[location.line].push_back(pc);
        }
    }
    for (auto& [line, pcs] : lineToPCs) {
        (void)line;
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
        const size_t mapped = oldToNew[oldPC];
        if (mapped == invalidPC || mapped >= newInstructionCount) {
            return false;
        }
        newPC = mapped;
        return true;
    };

    std::map<size_t, SourceLocation> remappedSources;
    for (const auto& [oldPC, location] : pcToSource) {
        size_t newPC = 0;
        if (mapPC(oldPC, newPC)) {
            remappedSources.emplace(newPC, location);
        }
    }

    std::map<size_t, InstructionDebugInfo> remappedInstructions;
    for (const auto& [oldPC, oldInstruction] : instructions) {
        size_t newPC = 0;
        if (!mapPC(oldPC, newPC)) {
            continue;
        }

        InstructionDebugInfo copy = oldInstruction;
        copy.pc = newPC;
        if (copy.origins.empty() && copy.location.isValid()) {
            SourceOrigin origin(copy.location);
            origin.originalPC = oldPC;
            origin.affectedVars = copy.affectedVars;
            copy.origins.push_back(std::move(origin));
        }
        for (auto& origin : copy.origins) {
            if (origin.originalPC == UNKNOWN_ORIGINAL_PC) {
                origin.originalPC = oldPC;
            }
            for (auto& predicate : origin.path) {
                size_t remappedRegion = 0;
                if (predicate.region <=
                        static_cast<ControlRegionId>(
                            std::numeric_limits<size_t>::max()
                        ) &&
                    mapPC(
                        static_cast<size_t>(predicate.region),
                        remappedRegion
                    )) {
                    predicate.region = remappedRegion;
                }
            }
        }

        auto [destination, inserted] =
            remappedInstructions.emplace(newPC, copy);
        if (!inserted) {
            for (const auto& origin : copy.origins) {
                appendUniqueOrigin(destination->second.origins, origin);
            }
        }
    }
    for (auto& [pc, instruction] : remappedInstructions) {
        if (!instruction.origins.empty()) {
            instruction.location = instruction.origins.front().location;
            instruction.affectedVars =
                instruction.origins.front().affectedVars;
            if (instruction.location.isValid()) {
                remappedSources[pc] = instruction.location;
            }
        }
    }
    pcToSource = std::move(remappedSources);
    instructions = std::move(remappedInstructions);

    auto remapIntervals = [&](const std::vector<PCRange>& oldRanges) {
        std::vector<size_t> mappedPCs;
        for (const auto& range : oldRanges) {
            const size_t cappedEnd = std::min(range.endPC, oldToNew.size());
            for (size_t oldPC = range.beginPC;
                 oldPC < cappedEnd;
                 ++oldPC) {
                size_t newPC = 0;
                if (mapPC(oldPC, newPC)) {
                    mappedPCs.push_back(newPC);
                }
            }
        }
        std::sort(mappedPCs.begin(), mappedPCs.end());
        mappedPCs.erase(
            std::unique(mappedPCs.begin(), mappedPCs.end()),
            mappedPCs.end()
        );
        std::vector<PCRange> result;
        for (size_t pc : mappedPCs) {
            if (result.empty() || pc > result.back().endPC) {
                result.emplace_back(pc, pc + 1);
            } else if (pc == result.back().endPC) {
                result.back().endPC = pc + 1;
            }
        }
        return result;
    };

    auto remapEnvelope = [&](size_t& startPC, size_t& endPC) {
        const auto ranges = remapIntervals({PCRange(startPC, endPC)});
        if (ranges.empty()) {
            endPC = startPC;
            return;
        }
        startPC = ranges.front().beginPC;
        endPC = ranges.back().endPC;
    };

    auto remapVariable = [&](VariableDebugInfo& variable) {
        if (!variable.hasExplicitAvailability) {
            return;
        }
        variable.availabilityRanges =
            remapIntervals(variable.availabilityRanges);
    };

    for (auto& [name, function] : functions) {
        (void)name;
        remapEnvelope(function.startPC, function.endPC);
        for (auto& parameter : function.parameters) {
            remapVariable(parameter);
        }
        for (auto& local : function.localVars) {
            remapVariable(local);
        }
    }
    for (auto& scope : scopes) {
        if (!scope) {
            continue;
        }
        scope->ranges = remapIntervals(effectiveRanges(*scope));
        if (scope->ranges.empty()) {
            scope->endPC = scope->startPC;
        } else {
            scope->startPC = scope->ranges.front().beginPC;
            scope->endPC = scope->ranges.back().endPC;
        }
        for (auto& variable : scope->variables) {
            remapVariable(variable);
        }
    }
    for (auto& [name, variable] : variables) {
        (void)name;
        remapVariable(variable);
    }
    buildLineToPC();
}

std::shared_ptr<DebugInfo> DebugInfo::remapped(
    const std::vector<size_t>& oldToNew,
    size_t newInstructionCount,
    const std::vector<std::vector<OriginRewriteRef>>& newToOldOrigins
) const
{
    if (!newToOldOrigins.empty() &&
        newToOldOrigins.size() != newInstructionCount) {
        return nullptr;
    }
    // Branch predicates use the IF/NOTIF instruction PC as their persistent
    // region identity. Every existing predicate must therefore be mappable in
    // this rewrite; otherwise accepting the candidate would leave stale guards.
    const size_t invalidPC = std::numeric_limits<size_t>::max();
    for (const auto& [pc, instruction] : instructions) {
        (void)pc;
        for (const auto& origin : instruction.origins) {
            for (const auto& predicate : origin.path) {
                if (predicate.region >
                    static_cast<ControlRegionId>(
                        std::numeric_limits<size_t>::max()
                    )) {
                    return nullptr;
                }
                const size_t oldRegion =
                    static_cast<size_t>(predicate.region);
                auto opener = instructions.find(oldRegion);
                if (oldRegion >= oldToNew.size() ||
                    oldToNew[oldRegion] == invalidPC ||
                    oldToNew[oldRegion] >= newInstructionCount ||
                    opener == instructions.end() ||
                    !isStructuredIfOpcode(opener->second.opcode)) {
                    return nullptr;
                }
            }
        }
    }

    auto candidate = fromJson(toJson());
    if (!candidate) {
        return nullptr;
    }
    candidate->remapPCs(oldToNew, newInstructionCount);

    if (!newToOldOrigins.empty()) {
        for (size_t newPC = 0; newPC < newToOldOrigins.size(); ++newPC) {
            std::vector<SourceOrigin> origins;
            for (const auto& reference : newToOldOrigins[newPC]) {
                if (reference.oldPC >= oldToNew.size()) {
                    return nullptr;
                }
                auto oldOrigins = getOriginsForPC(reference.oldPC);
                if (oldOrigins.empty()) {
                    return nullptr;
                }
                for (auto origin : oldOrigins) {
                    if (origin.originalPC == UNKNOWN_ORIGINAL_PC) {
                        origin.originalPC = reference.oldPC;
                    }
                    for (auto& predicate : origin.path) {
                        const size_t oldRegion =
                            static_cast<size_t>(predicate.region);
                        predicate.region = oldToNew[oldRegion];
                    }
                    for (const auto& predicate : reference.path) {
                        if (predicate.region >= newInstructionCount) {
                            return nullptr;
                        }
                        auto opener = candidate->instructions.find(
                            static_cast<size_t>(predicate.region)
                        );
                        if (opener == candidate->instructions.end() ||
                            !isStructuredIfOpcode(opener->second.opcode)) {
                            return nullptr;
                        }
                    }
                    mergePath(origin.path, reference.path);
                    appendUniqueOrigin(origins, origin);
                }
            }
            candidate->setInstructionOrigins(newPC, origins);
        }
    }
    return candidate->validate() ? candidate : nullptr;
}

bool DebugInfo::applyRemapTransactional(
    const std::vector<size_t>& oldToNew,
    size_t newInstructionCount,
    const std::vector<std::vector<OriginRewriteRef>>& newToOldOrigins
)
{
    auto candidate = remapped(
        oldToNew,
        newInstructionCount,
        newToOldOrigins
    );
    if (!candidate) {
        return false;
    }
    *this = std::move(*candidate);
    return true;
}

void DebugInfo::syncInstructionOpcodes(const std::vector<std::string>& bytecode)
{
    for (auto it = instructions.begin(); it != instructions.end();) {
        if (it->first >= bytecode.size()) {
            pcToSource.erase(it->first);
            it = instructions.erase(it);
            continue;
        }
        it->second.pc = it->first;
        it->second.opcode = bytecode[it->first];
        it->second.operand.clear();
        ++it;
    }
    buildLineToPC();
}

bool DebugInfo::validate(std::string* errorMessage) const
{
    if (version != "2.0") {
        return validate(std::vector<std::string>{}, errorMessage);
    }

    if (errorMessage) {
        errorMessage->clear();
    }
    if (!scopeNestingValid) {
        if (errorMessage) {
            *errorMessage = "作用域 enter/exit 不配对";
        }
        return false;
    }
    if (!validateV2Core()) {
        if (errorMessage) {
            *errorMessage = "DebugInfo V2 内部结构无效";
        }
        return false;
    }
    return true;
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

    if (version == "2.0") {
        if (!validate(errorMessage)) {
            return false;
        }
        if (bytecode.empty()) {
            return true;
        }

        size_t executableInstructionCount = bytecode.size();
        const auto padding = std::find(bytecode.begin(), bytecode.end(), "ff");
        if (padding != bytecode.end()) {
            executableInstructionCount = static_cast<size_t>(
                std::distance(bytecode.begin(), padding)
            );
        }
        for (const auto& [pc, location] : pcToSource) {
            (void)location;
            if (pc >= executableInstructionCount ||
                !instructions.contains(pc)) {
                return fail(
                    "源码映射 PC 超出真实可执行字节码范围: " +
                    std::to_string(pc)
                );
            }
        }
        for (const auto& [pc, instruction] : instructions) {
            if (pc >= executableInstructionCount) {
                return fail(
                    "调试指令 PC 超出真实可执行字节码范围: " +
                    std::to_string(pc)
                );
            }
            std::string expected = bytecode[pc];
            std::string actual = instruction.opcode;
            std::transform(
                expected.begin(), expected.end(), expected.begin(),
                [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                }
            );
            std::transform(
                actual.begin(), actual.end(), actual.begin(),
                [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                }
            );
            if (actual != expected) {
                return fail(
                    "调试指令与真实字节码不一致: PC " +
                    std::to_string(pc)
                );
            }
        }
        for (const auto& [name, function] : functions) {
            if (function.endPC > executableInstructionCount) {
                return fail("函数范围超出真实可执行字节码: " + name);
            }
        }
        for (const auto& scope : scopes) {
            if (!scope || scope->type == ScopeType::GLOBAL) {
                continue;
            }
            for (const auto& range : effectiveRanges(*scope)) {
                if (range.endPC > executableInstructionCount) {
                    return fail(
                        "作用域范围超出真实可执行字节码: " + scope->name
                    );
                }
            }
        }
        return true;
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

bool DebugInfo::validateV2Core() const
{
    if (sourceFilename.empty()) {
        return false;
    }
    for (const auto& [pc, location] : pcToSource) {
        (void)pc;
        if (!location.isValid()) {
            return false;
        }
    }
    for (const auto& [name, function] : functions) {
        (void)name;
        // A function may legitimately emit no runtime instruction (for
        // example, a setup helper containing only compile-time fixed data).
        // Half-open [startPC, endPC) ranges therefore allow equality; only a
        // reversed range is malformed.
        if (function.startPC > function.endPC) {
            return false;
        }
        if (std::any_of(
                function.parameters.begin(),
                function.parameters.end(),
                [](const VariableDebugInfo& variable) {
                    return !validVariableRanges(variable);
                }
            ) ||
            std::any_of(
                function.localVars.begin(),
                function.localVars.end(),
                [](const VariableDebugInfo& variable) {
                    return !validVariableRanges(variable);
                }
            )) {
            return false;
        }
    }

    std::set<ScopeId> scopeIds;
    for (const auto& scope : scopes) {
        if (!scope || scope->scopeId == INVALID_SCOPE_ID ||
            !scopeIds.insert(scope->scopeId).second) {
            return false;
        }
        const auto ranges = effectiveRanges(*scope);
        if (!std::is_sorted(
                ranges.begin(),
                ranges.end(),
                [](const auto& lhs, const auto& rhs) {
                    return lhs.beginPC < rhs.beginPC;
                }
            )) {
            return false;
        }
        if (std::any_of(
                scope->variables.begin(),
                scope->variables.end(),
                [](const VariableDebugInfo& variable) {
                    return !validVariableRanges(variable);
                }
            )) {
            return false;
        }
    }
    for (const auto& scope : scopes) {
        auto parent = scope ? scope->parent.lock() : nullptr;
        if (parent && !scopeIds.contains(parent->scopeId)) {
            return false;
        }
        std::unordered_set<const ScopeDebugInfo*> ancestry;
        for (auto current = scope; current;
             current = current->parent.lock()) {
            if (!ancestry.insert(current.get()).second) {
                return false;
            }
        }
    }
    for (const auto& [pc, instruction] : instructions) {
        if (instruction.pc != pc) {
            return false;
        }
        for (const auto& origin : instruction.origins) {
            if (!origin.location.isValid() ||
                hasDuplicatePathRegion(origin.path)) {
                return false;
            }
            if (origin.scopeId != INVALID_SCOPE_ID &&
                !scopeIds.contains(origin.scopeId)) {
                return false;
            }
            for (const auto& predicate : origin.path) {
                switch (predicate.arm) {
                    case BranchArm::Then:
                    case BranchArm::Else:
                        break;
                    default:
                        return false;
                }
                if (predicate.region >
                    static_cast<ControlRegionId>(
                        std::numeric_limits<size_t>::max()
                    )) {
                    return false;
                }
                const size_t regionPC =
                    static_cast<size_t>(predicate.region);
                const auto opener = instructions.find(regionPC);
                // Predicates live in final-PC space. originalPC describes the
                // pre-rewrite instruction and must not be used for ordering.
                if (regionPC >= pc || opener == instructions.end() ||
                    !isStructuredIfOpcode(opener->second.opcode)) {
                    return false;
                }
            }
        }
    }
    if (std::any_of(
            variables.begin(),
            variables.end(),
            [](const auto& entry) {
                return !validVariableRanges(entry.second);
            }
        )) {
        return false;
    }
    return true;
}

bool DebugInfo::save(const std::string& filename) const
{
    try {
        std::ofstream file(filename);
        if (!file.is_open()) {
            return false;
        }
        file << toJson();
        return file.good();
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
        return fromJson(content);
    } catch (...) {
        return nullptr;
    }
}

std::string DebugInfo::toJson() const
{
    json result;
    result["version"] = "2.0";
    result["format"] = "apc-debug";
    result["sourceFile"] = sourceFilename;
    result["contractName"] = contractName;
    result["scopeNestingValid"] = scopeNestingValid;

    json sourceMap = json::object();
    for (const auto& [pc, location] : pcToSource) {
        sourceMap[std::to_string(pc)] = sourceLocationToJson(location);
    }
    result["pcToSource"] = sourceMap;

    json lineMap = json::object();
    for (const auto& [line, pcs] : lineToPCs) {
        lineMap[std::to_string(line)] = pcs;
    }
    result["lineToPC"] = lineMap;

    result["instructions"] = json::array();
    for (const auto& [pc, instruction] : instructions) {
        (void)pc;
        json item{{"pc", instruction.pc},
                  {"opcode", instruction.opcode},
                  {"operand", instruction.operand},
                  {"affectedVars", instruction.affectedVars},
                  {"location", sourceLocationToJson(instruction.location)}};
        item["origins"] = json::array();
        for (const auto& origin : instruction.origins) {
            item["origins"].push_back(sourceOriginToJson(origin));
        }
        result["instructions"].push_back(std::move(item));
    }

    result["functions"] = json::array();
    for (const auto& [name, function] : functions) {
        (void)name;
        json item{{"name", function.name},
                  {"startPC", function.startPC},
                  {"endPC", function.endPC},
                  {"scopeId", function.scope
                                  ? function.scope->scopeId
                                  : function.scopeId},
                  {"isPublic", function.isPublic},
                  {"location", sourceLocationToJson(function.location)}};
        item["parameters"] = json::array();
        for (const auto& parameter : function.parameters) {
            item["parameters"].push_back(variableToJson(parameter));
        }
        item["localVars"] = json::array();
        for (const auto& local : function.localVars) {
            item["localVars"].push_back(variableToJson(local));
        }
        result["functions"].push_back(std::move(item));
    }

    result["localVars"] = json::array();
    for (const auto& [name, function] : functions) {
        (void)name;
        for (const auto& local : function.localVars) {
            result["localVars"].push_back(variableToJson(local));
        }
    }
    result["variables"] = json::array();
    for (const auto& [name, variable] : variables) {
        (void)name;
        result["variables"].push_back(variableToJson(variable));
    }

    std::map<const ScopeDebugInfo*, size_t> scopeIndices;
    for (size_t index = 0; index < scopes.size(); ++index) {
        if (scopes[index]) {
            scopeIndices[scopes[index].get()] = index;
        }
    }
    result["scopes"] = json::array();
    for (size_t index = 0; index < scopes.size(); ++index) {
        const auto& scope = scopes[index];
        if (!scope) {
            continue;
        }
        int parentIndex = -1;
        const auto parentScope = scope->parent.lock();
        if (parentScope) {
            auto parent = scopeIndices.find(parentScope.get());
            if (parent != scopeIndices.end()) {
                parentIndex = static_cast<int>(parent->second);
            }
        }
        json item{{"index", index},
                  {"scopeId", scope->scopeId},
                  {"name", scope->name},
                  {"type", scopeTypeToString(scope->type)},
                  {"location", sourceLocationToJson(scope->location)},
                  {"startPC", scope->startPC},
                  {"endPC", scope->endPC},
                  {"parentIndex", parentIndex},
                  {"parentScopeId", parentScope
                                        ? parentScope->scopeId
                                        : INVALID_SCOPE_ID}};
        item["ranges"] = json::array();
        for (const auto& range : effectiveRanges(*scope)) {
            item["ranges"].push_back(
                {{"beginPC", range.beginPC}, {"endPC", range.endPC}}
            );
        }
        item["variables"] = json::array();
        for (const auto& variable : scope->variables) {
            item["variables"].push_back(variableToJson(variable));
        }
        result["scopes"].push_back(std::move(item));
    }
    return result.dump(2);
}

std::shared_ptr<DebugInfo> DebugInfo::fromJson(const std::string& jsonString)
{
    try {
        const json input = json::parse(jsonString);
        if (input.contains("format") &&
            input.value("format", "apc-debug") != "apc-debug") {
            return nullptr;
        }

        auto info = std::make_shared<DebugInfo>();
        info->version = input.value("version", "1.0");
        info->sourceFilename = input.value("sourceFile", "");
        info->contractName = input.value("contractName", "");
        const bool serializedScopeNestingValid =
            input.value("scopeNestingValid", false);
        // Validate the upgraded graph independently of the legacy nesting
        // marker; callers still observe and enforce the serialized value.
        info->scopeNestingValid = true;

        if (input.contains("pcToSource")) {
            for (const auto& [pcString, locationJson] :
                 input["pcToSource"].items()) {
                info->pcToSource[std::stoull(pcString)] =
                    sourceLocationFromJson(locationJson);
            }
        }

        if (input.contains("instructions")) {
            for (const auto& item : input["instructions"]) {
                InstructionDebugInfo instruction;
                instruction.pc = item.value("pc", 0);
                instruction.opcode = item.value("opcode", "");
                instruction.operand = item.value("operand", "");
                if (item.contains("location")) {
                    instruction.location =
                        sourceLocationFromJson(item["location"]);
                } else if (auto source =
                               info->pcToSource.find(instruction.pc);
                           source != info->pcToSource.end()) {
                    instruction.location = source->second;
                }
                if (item.contains("affectedVars")) {
                    instruction.affectedVars =
                        item["affectedVars"].get<std::vector<std::string>>();
                }
                if (item.contains("origins")) {
                    for (const auto& originJson : item["origins"]) {
                        appendUniqueOrigin(
                            instruction.origins,
                            sourceOriginFromJson(originJson)
                        );
                    }
                }
                info->instructions[instruction.pc] = std::move(instruction);
            }
        }

        if (input.contains("functions")) {
            for (const auto& item : input["functions"]) {
                FunctionDebugInfo function;
                function.name = item.value("name", "");
                function.startPC = item.value("startPC", 0);
                function.endPC = item.value("endPC", 0);
                function.scopeId = item.value("scopeId", INVALID_SCOPE_ID);
                function.isPublic = item.value("isPublic", false);
                if (item.contains("location")) {
                    function.location = sourceLocationFromJson(item["location"]);
                }
                if (item.contains("parameters")) {
                    for (const auto& parameterJson : item["parameters"]) {
                        auto parameter = variableFromJson(parameterJson);
                        parameter.isParameter = true;
                        function.parameters.push_back(std::move(parameter));
                    }
                }
                if (item.contains("localVars")) {
                    for (const auto& localJson : item["localVars"]) {
                        function.localVars.push_back(
                            variableFromJson(localJson)
                        );
                    }
                }
                info->functions[function.name] = std::move(function);
            }
        }

        if (input.contains("variables")) {
            for (const auto& item : input["variables"]) {
                auto variable = variableFromJson(item);
                info->variables[variable.name] = std::move(variable);
            }
        }

        std::vector<int> parentIndices;
        std::vector<ScopeId> parentScopeIds;
        ScopeId nextGeneratedScopeId = 1;
        if (input.contains("scopes")) {
            for (const auto& item : input["scopes"]) {
                auto scope = std::make_shared<ScopeDebugInfo>();
                scope->scopeId = item.value("scopeId", INVALID_SCOPE_ID);
                if (scope->scopeId == INVALID_SCOPE_ID) {
                    scope->scopeId = nextGeneratedScopeId;
                }
                nextGeneratedScopeId =
                    std::max(nextGeneratedScopeId, scope->scopeId + 1);
                scope->name = item.value("name", "");
                scope->type =
                    stringToScopeType(item.value("type", "block"));
                if (item.contains("location")) {
                    scope->location = sourceLocationFromJson(item["location"]);
                }
                scope->startPC = item.value("startPC", 0);
                scope->endPC = item.value("endPC", 0);
                if (item.contains("ranges")) {
                    for (const auto& rangeJson : item["ranges"]) {
                        scope->addRange(
                            rangeJson.value("beginPC", 0),
                            rangeJson.value("endPC", 0)
                        );
                    }
                }
                if (scope->ranges.empty() && scope->startPC < scope->endPC) {
                    scope->ranges.emplace_back(scope->startPC, scope->endPC);
                }
                if (item.contains("variables")) {
                    for (const auto& variableJson : item["variables"]) {
                        auto variable = variableFromJson(variableJson);
                        if (variable.scopeId == INVALID_SCOPE_ID) {
                            variable.scopeId = scope->scopeId;
                        }
                        if (variable.scopeName.empty()) {
                            variable.scopeName = scope->name;
                        }
                        scope->variables.push_back(std::move(variable));
                    }
                }
                parentIndices.push_back(item.value("parentIndex", -1));
                parentScopeIds.push_back(
                    item.value("parentScopeId", INVALID_SCOPE_ID)
                );
                info->scopes.push_back(scope);
                if (scope->type == ScopeType::GLOBAL) {
                    info->globalScope = scope;
                }
            }

            std::map<ScopeId, size_t> scopeIndexById;
            for (size_t index = 0; index < info->scopes.size(); ++index) {
                if (!scopeIndexById
                         .emplace(info->scopes[index]->scopeId, index)
                         .second) {
                    return nullptr;
                }
            }

            const size_t noParent = info->scopes.size();
            std::vector<size_t> resolvedParents(
                info->scopes.size(), noParent
            );
            for (size_t index = 0; index < info->scopes.size(); ++index) {
                if (parentScopeIds[index] != INVALID_SCOPE_ID) {
                    auto parent = scopeIndexById.find(parentScopeIds[index]);
                    if (parent == scopeIndexById.end()) {
                        return nullptr;
                    }
                    resolvedParents[index] = parent->second;
                } else if (parentIndices[index] >= 0) {
                    const size_t parentIndex =
                        static_cast<size_t>(parentIndices[index]);
                    if (parentIndex >= info->scopes.size()) {
                        return nullptr;
                    }
                    resolvedParents[index] = parentIndex;
                }
                if (resolvedParents[index] == index) {
                    return nullptr;
                }
            }

            // Validate the parent graph before creating shared ownership
            // links; otherwise a rejected cycle would itself leak.
            std::vector<unsigned char> parentState(info->scopes.size(), 0);
            std::function<bool(size_t)> visitParent = [&](size_t index) {
                if (parentState[index] == 1) {
                    return false;
                }
                if (parentState[index] == 2) {
                    return true;
                }
                parentState[index] = 1;
                const size_t parent = resolvedParents[index];
                if (parent != noParent && !visitParent(parent)) {
                    return false;
                }
                parentState[index] = 2;
                return true;
            };
            for (size_t index = 0; index < info->scopes.size(); ++index) {
                if (!visitParent(index)) {
                    return nullptr;
                }
            }

            for (size_t index = 0; index < info->scopes.size(); ++index) {
                if (resolvedParents[index] == noParent) {
                    continue;
                }
                auto parent = info->scopes[resolvedParents[index]];
                info->scopes[index]->parent = parent;
                parent->children.push_back(info->scopes[index]);
            }
        }

        for (auto& [name, function] : info->functions) {
            if (function.scopeId != INVALID_SCOPE_ID) {
                function.scope = info->getScopeById(function.scopeId);
            }
            if (!function.scope) {
                for (const auto& scope : info->scopes) {
                    if (scope && scope->type == ScopeType::FUNCTION &&
                        scope->name == name &&
                        scope->startPC == function.startPC) {
                        function.scope = scope;
                        function.scopeId = scope->scopeId;
                        break;
                    }
                }
            }
        }

        // Upgrade V1 instructions into the V2 one-origin representation and
        // infer stable scope/function identities after all scopes are loaded.
        for (auto& [pc, instruction] : info->instructions) {
            if (instruction.origins.empty() && instruction.location.isValid()) {
                SourceOrigin origin(instruction.location);
                origin.originalPC = pc;
                origin.affectedVars = instruction.affectedVars;
                instruction.origins.push_back(std::move(origin));
            }
            for (auto& origin : instruction.origins) {
                if (origin.originalPC == UNKNOWN_ORIGINAL_PC) {
                    origin.originalPC = pc;
                }
                if (origin.scopeId == INVALID_SCOPE_ID) {
                    // No path is needed for V1, so range lookup is unambiguous.
                    std::shared_ptr<ScopeDebugInfo> best;
                    size_t bestCoverage = std::numeric_limits<size_t>::max();
                    size_t bestDepth = 0;
                    for (const auto& scope : info->scopes) {
                        if (!scope || !scope->containsPC(pc)) {
                            continue;
                        }
                        const size_t coverage =
                            scope->coveredInstructionCount();
                        const size_t depth = scopeDepth(scope);
                        if (!best || coverage < bestCoverage ||
                            (coverage == bestCoverage && depth > bestDepth)) {
                            best = scope;
                            bestCoverage = coverage;
                            bestDepth = depth;
                        }
                    }
                    if (best) {
                        origin.scopeId = best->scopeId;
                    }
                }
                if (origin.functionName.empty()) {
                    if (const auto* function = info->getFunctionAtPC(pc)) {
                        origin.functionName = function->name;
                    }
                }
            }
            if (!instruction.origins.empty()) {
                instruction.location = instruction.origins.front().location;
                if (instruction.affectedVars.empty()) {
                    instruction.affectedVars =
                        instruction.origins.front().affectedVars;
                }
            }
            if (instruction.location.isValid()) {
                info->pcToSource[pc] = instruction.location;
            }
        }

        info->buildLineToPC();
        if (!info->validateV2Core()) {
            return nullptr;
        }
        info->scopeNestingValid = serializedScopeNestingValid;
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
    }
    return "unknown";
}

ScopeType stringToScopeType(const std::string& value)
{
    if (value == "global")
        return ScopeType::GLOBAL;
    if (value == "contract")
        return ScopeType::CONTRACT;
    if (value == "function")
        return ScopeType::FUNCTION;
    if (value == "loop")
        return ScopeType::LOOP;
    if (value == "conditional")
        return ScopeType::CONDITIONAL;
    return ScopeType::BLOCK;
}

} // namespace apc_debug

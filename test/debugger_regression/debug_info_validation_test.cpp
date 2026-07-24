#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "debugger/core/debugger_core.h"
#include "debugger/info/debug_info.h"

namespace
{
using apc_debug::DebugInfo;
using apc_debug::FunctionDebugInfo;
using apc_debug::InstructionDebugInfo;
using apc_debug::ScopeDebugInfo;
using apc_debug::ScopeType;
using apc_debug::SourceLocation;
using apc_debug::VariableDebugInfo;

const std::vector<std::string> kBytecode{"51", "52", "93"};

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "debug-info validation regression failed: " << message
              << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

VariableDebugInfo makeVariable(
    const std::string& name,
    const std::string& scopeName,
    size_t line,
    bool parameter = false
)
{
    VariableDebugInfo variable;
    variable.name = name;
    variable.type = "int";
    variable.scopeName = scopeName;
    variable.declLine = line;
    variable.declColumn = 5;
    variable.isStackVar = true;
    variable.stackOffset = parameter ? 0 : -1;
    variable.isParameter = parameter;
    return variable;
}

std::shared_ptr<DebugInfo> makeValidInfo()
{
    auto info = std::make_shared<DebugInfo>();
    info->sourceFilename = "debug_validation.ct";
    info->contractName = "DebugValidation";
    info->scopeNestingValid = true;

    auto global = std::make_shared<ScopeDebugInfo>(
        "global", ScopeType::GLOBAL
    );
    auto function = std::make_shared<ScopeDebugInfo>(
        "test", ScopeType::FUNCTION
    );
    function->location = SourceLocation("debug_validation.ct", 2, 5);
    function->startPC = 0;
    function->endPC = 3;
    function->parent = global;
    global->children.push_back(function);

    auto block = std::make_shared<ScopeDebugInfo>("body", ScopeType::BLOCK);
    block->location = SourceLocation("debug_validation.ct", 3, 9);
    block->startPC = 0;
    block->endPC = 3;
    block->parent = function;
    function->children.push_back(block);

    auto loop = std::make_shared<ScopeDebugInfo>("loop", ScopeType::LOOP);
    loop->location = SourceLocation("debug_validation.ct", 4, 9);
    loop->startPC = 1;
    loop->endPC = 2;
    loop->parent = block;
    block->children.push_back(loop);

    const auto parameter = makeVariable("input", "test", 2, true);
    const auto loopVariable = makeVariable("i", "loop", 4);
    function->variables.push_back(parameter);
    loop->variables.push_back(loopVariable);

    info->globalScope = global;
    info->scopes = {global, function, block, loop};
    info->variables[parameter.name] = parameter;
    info->variables[loopVariable.name] = loopVariable;

    FunctionDebugInfo functionInfo("test");
    functionInfo.location = function->location;
    functionInfo.startPC = 0;
    functionInfo.endPC = 3;
    functionInfo.parameters.push_back(parameter);
    functionInfo.localVars.push_back(loopVariable);
    functionInfo.scope = function;
    functionInfo.isPublic = true;
    info->functions.emplace(functionInfo.name, functionInfo);

    for (size_t pc = 0; pc < kBytecode.size(); ++pc) {
        InstructionDebugInfo instruction(
            pc, SourceLocation("debug_validation.ct", 3 + pc, 9)
        );
        instruction.opcode = kBytecode[pc];
        info->addInstruction(instruction);
    }

    return info;
}

void expectInvalid(
    const std::shared_ptr<DebugInfo>& info,
    const std::string& caseName
)
{
    std::string error;
    require(!info->validate(kBytecode, &error), caseName + " should fail");
    require(!error.empty(), caseName + " should report a reason");
}
} // namespace

int main()
{
    {
        auto info = makeValidInfo();
        std::string error;
        require(info->validate(kBytecode, &error), "valid loop scope: " + error);
        require(
            apc_debug::DebuggerCore::validateDebugInfo(
                info, "515293", &error
            ),
            "valid final bytecode: " + error
        );
        auto roundTrip = DebugInfo::fromJson(info->toJson());
        require(roundTrip != nullptr, "valid JSON round trip should parse");
        require(
            roundTrip->validate(kBytecode, &error),
            "valid JSON round trip: " + error
        );
    }

    {
        namespace fs = std::filesystem;
        const fs::path sourceRoot =
            fs::current_path() / "debugger_source_mapping_fixture";
        const fs::path primarySource = sourceRoot / "main.ct";
        const fs::path importedSource = sourceRoot / "imported.ct";

        DebugInfo info;
        info.sourceFilename = primarySource.string();
        info.addSourceMapping(
            1,
            SourceLocation(primarySource.string(), 10, 1)
        );
        info.addSourceMapping(
            2,
            SourceLocation(importedSource.string(), 10, 1)
        );
        info.addSourceMapping(
            3,
            SourceLocation(importedSource.string(), 11, 1)
        );
        info.addSourceMapping(
            4,
            SourceLocation(primarySource.string(), 12, 1)
        );
        info.addSourceMapping(5, SourceLocation("", 14, 1));
        info.addSourceMapping(6, SourceLocation("main.ct", 15, 1));
        info.addSourceMapping(
            7,
            SourceLocation(primarySource.string(), 1, 1)
        );

        require(
            info.getPCsForLine(10) == std::vector<size_t>({1, 2}),
            "line-only lookup should include primary and imported sources"
        );
        require(
            info.getPCsForSourceLine(primarySource.string(), 10) ==
                std::vector<size_t>({1}),
            "primary source lookup should not include imported source"
        );
        require(
            info.getPCsForSourceLine(importedSource.string(), 10) ==
                std::vector<size_t>({2}),
            "imported source lookup should not include primary source"
        );
        require(
            info.getPCsForSourceLine(
                (sourceRoot / "nested" / ".." / "main.ct").string(),
                10
            ) == std::vector<size_t>({1}),
            "lexically equivalent source path should resolve"
        );
        require(
            info.getPCsForSourceLine(
                (sourceRoot / "missing.ct").string(),
                10
            ).empty(),
            "wrong source path should not resolve"
        );
        require(
            info.findNearestValidSourceLine(
                primarySource.string(),
                11,
                2
            ) == std::vector<size_t>({4}),
            "nearest lookup should stay in requested source"
        );
        require(
            info.getPCsForSourceLine(primarySource.string(), 14) ==
                std::vector<size_t>({5}),
            "empty mapping filename should fall back to primary source"
        );
        require(
            info.getPCsForSourceLine(
                primarySource.filename().string(),
                15
            ) == std::vector<size_t>({6}),
            "relative source path should resolve against primary source"
        );
        require(
            info.findNearestValidSourceLine(
                primarySource.string(),
                std::numeric_limits<size_t>::max(),
                2
            ).empty(),
            "nearest source lookup should not wrap an oversized line number"
        );
        require(
            info.findNearestValidLine(
                std::numeric_limits<size_t>::max(),
                2
            ).empty(),
            "legacy nearest lookup should not wrap an oversized line number"
        );
    }

    {
        auto info = makeValidInfo();
        info->pcToSource.emplace(3, SourceLocation("debug_validation.ct", 9, 1));
        info->lineToPCs[9].push_back(3);
        expectInvalid(info, "out-of-range source PC");
    }

    {
        auto info = makeValidInfo();
        info->instructions.at(1).opcode = "51";
        expectInvalid(info, "stale instruction opcode");
    }

    {
        auto info = makeValidInfo();
        info->functions.at("test").endPC = 4;
        expectInvalid(info, "out-of-range function");
    }

    {
        auto info = makeValidInfo();
        auto ghost = std::make_shared<ScopeDebugInfo>(
            "ghost", ScopeType::FUNCTION
        );
        ghost->location = SourceLocation("debug_validation.ct", 8, 1);
        ghost->startPC = 2;
        ghost->endPC = 2;
        ghost->parent = info->globalScope;
        info->globalScope->children.push_back(ghost);
        info->scopes.push_back(ghost);
        expectInvalid(info, "function scope without function record");
    }

    {
        auto info = makeValidInfo();
        auto loop = info->scopes.at(3);
        auto block = info->scopes.at(2);
        block->children.clear();
        loop->parent = info->globalScope;
        info->globalScope->children.push_back(loop);
        expectInvalid(info, "loop owned by global scope");
    }

    {
        auto info = makeValidInfo();
        auto block = info->scopes.at(2);
        auto overlapping = std::make_shared<ScopeDebugInfo>(
            "overlap", ScopeType::BLOCK
        );
        overlapping->location = SourceLocation("debug_validation.ct", 8, 9);
        overlapping->startPC = 0;
        overlapping->endPC = 2;
        overlapping->parent = block;
        block->children.push_back(overlapping);
        info->scopes.push_back(overlapping);
        expectInvalid(info, "overlapping sibling scopes");
    }

    {
        auto info = makeValidInfo();
        info->scopes.at(3)->variables.at(0).scopeName = "body";
        expectInvalid(info, "variable with wrong scopeName");
    }

    {
        auto info = makeValidInfo();
        std::string error;
        require(
            !apc_debug::DebuggerCore::validateDebugInfo(
                info, "4c02aa", &error
            ),
            "truncated PUSHDATA must fail"
        );
        require(!error.empty(), "truncated PUSHDATA should report a reason");
    }

    {
        auto info = makeValidInfo();
        InstructionDebugInfo padding(
            3, SourceLocation("debug_validation.ct", 9, 1)
        );
        padding.opcode = "ff";
        info->addInstruction(padding);
        std::string error;
        require(
            !info->validate({"51", "52", "93", "ff"}, &error),
            "padding PC must not be source mapped"
        );
        require(!error.empty(), "padding PC should report a reason");
    }

    {
        auto serialized = nlohmann::json::parse(makeValidInfo()->toJson());
        serialized.erase("scopeNestingValid");
        auto legacy = DebugInfo::fromJson(serialized.dump());
        require(legacy != nullptr, "legacy JSON should still parse");
        require(
            !legacy->scopeNestingValid,
            "missing scopeNestingValid must default to false"
        );
        expectInvalid(legacy, "missing scopeNestingValid");
    }

    std::cout << "DebugInfo validation regression checks passed.\n";
    return 0;
}

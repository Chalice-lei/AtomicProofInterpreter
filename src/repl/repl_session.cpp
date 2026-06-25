#include "repl_session.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <utility>

#include "../compiler/frontend_pipeline.h"
#include "../error/error_manager.h"
#include "../util/string_utils.h"

namespace apc_repl
{
namespace
{

constexpr const char* kSessionContractName = "ReplSession";
constexpr const char* kCellFunctionName = "__repl_cell__";

using apc::util::trim;

bool startsWithKeyword(const std::string& text, const std::string& keyword)
{
    if (text == keyword) {
        return true;
    }
    if (text.size() <= keyword.size()) {
        return false;
    }
    if (text.rfind(keyword, 0) != 0) {
        return false;
    }
    const char next = text[keyword.size()];
    return next == ' ' || next == '\t' || next == '(';
}

std::string indentSource(const std::string& source, int spaces)
{
    std::istringstream input(source);
    std::ostringstream output;
    std::string line;
    const std::string padding(static_cast<size_t>(spaces), ' ');

    while (std::getline(input, line)) {
        output << padding << line << '\n';
    }

    if (!source.empty() && source.back() == '\n') {
        return output.str();
    }
    if (source.empty()) {
        output << padding << '\n';
    }
    return output.str();
}

std::string memberKey(const ASTNode& node)
{
    if (const auto* function = dynamic_cast<const FunctionNode*>(&node)) {
        return "function:" + function->name;
    }
    if (const auto* structDef = dynamic_cast<const StructDefNode*>(&node)) {
        return "struct:" + structDef->name;
    }
    return "";
}

std::vector<std::string> defaultCompletionWords()
{
    return {
        "exit",
        "quit",
        "help",
        "history",
        "clear",
        "%run",
        "%load",
        "%debug",
        "%reset",
        "%who",
        "%help",
        "%quit",
        "def",
        "Struct",
        "if",
        "else",
        "for",
        "in",
        "Return",
        "return",
        "Range",
        "true",
        "false",
        "int",
        "bool",
        "string",
        "hex",
        "bytes",
        "address",
        "uint64",
        "uint32",
        "uint16",
        "uint8",
    };
}

} // namespace

bool looksLikeDefinitionCell(const std::string& source)
{
    std::istringstream input(source);
    std::string line;
    while (std::getline(input, line)) {
        const std::string stripped = trim(line);
        if (stripped.empty()) {
            continue;
        }
        return startsWithKeyword(stripped, "def") ||
               startsWithKeyword(stripped, "Struct");
    }
    return false;
}

ReplSession::ReplSession() : m_contract(kSessionContractName)
{
    m_interpreter.beginSession(m_contract);
}

ReplCellResult ReplSession::executeCell(const std::string& source)
{
    if (trim(source).empty()) {
        ReplCellResult result;
        result.success = true;
        return result;
    }

    if (looksLikeDefinitionCell(source)) {
        return executeDefinitionCell(source);
    }
    return executeStatementCell(source);
}

ReplCellResult ReplSession::loadFile(const std::string& filename)
{
    ReplCellResult result;

    std::ifstream input(filename);
    if (!input.is_open()) {
        result.errorMessage = "failed to open file '" + filename + "'";
        return result;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    auto parsed = parseWrappedSource(filename, buffer.str(), result);
    if (!parsed) {
        return result;
    }

    const std::string contractName = parsed->name;
    mergeMembers(*parsed);
    m_interpreter.refreshSessionProgram(m_contract);
    m_lastLoadedFile = filename;

    result.success = true;
    result.message = "Loaded contract " + contractName + " from " + filename;
    return result;
}

void ReplSession::reset()
{
    m_contract.members.clear();
    m_contract.libraries.clear();
    m_lastLoadedFile.clear();
    m_cellCounter = 0;
    m_interpreter.beginSession(m_contract);
}

std::vector<std::string> ReplSession::completionWords() const
{
    std::set<std::string> words;
    for (const auto& word : defaultCompletionWords()) {
        words.insert(word);
    }
    for (const auto& name : m_interpreter.globalNames()) {
        words.insert(name);
    }
    for (const auto& member : m_contract.members) {
        if (const auto* function = dynamic_cast<const FunctionNode*>(member.get())) {
            words.insert(function->name);
        } else if (const auto* structDef =
                       dynamic_cast<const StructDefNode*>(member.get())) {
            words.insert(structDef->name);
        }
    }
    return {words.begin(), words.end()};
}

std::vector<std::string> ReplSession::userNames() const
{
    std::set<std::string> names;
    for (const auto& name : m_interpreter.globalNames()) {
        names.insert(name);
    }
    for (const auto& member : m_contract.members) {
        if (const auto* function =
                dynamic_cast<const FunctionNode*>(member.get())) {
            names.insert(function->name);
        } else if (const auto* structDef =
                       dynamic_cast<const StructDefNode*>(member.get())) {
            names.insert(structDef->name);
        }
    }
    return {names.begin(), names.end()};
}

std::string ReplSession::wrapDefinitionCell(const std::string& source) const
{
    std::ostringstream wrapped;
    wrapped << "Contract " << kSessionContractName << ":\n";
    wrapped << indentSource(source, 4);
    return wrapped.str();
}

std::string ReplSession::wrapStatementCell(const std::string& source) const
{
    std::ostringstream wrapped;
    wrapped << "Contract " << kSessionContractName << ":\n";
    wrapped << "    def " << kCellFunctionName << "():\n";
    wrapped << indentSource(source, 8);
    return wrapped.str();
}

ReplCellResult ReplSession::executeDefinitionCell(const std::string& source)
{
    ReplCellResult result;
    const std::string filename =
        "<repl-cell-" + std::to_string(++m_cellCounter) + ">";
    auto parsed = parseWrappedSource(filename, wrapDefinitionCell(source), result);
    if (!parsed) {
        return result;
    }

    mergeMembers(*parsed);
    m_interpreter.refreshSessionProgram(m_contract);

    result.success = true;
    return result;
}

ReplCellResult ReplSession::executeStatementCell(const std::string& source)
{
    ReplCellResult result;
    const std::string filename =
        "<repl-cell-" + std::to_string(++m_cellCounter) + ">";
    auto parsed = parseWrappedSource(filename, wrapStatementCell(source), result);
    if (!parsed) {
        return result;
    }

    FunctionNode* cellFunction = findCellFunction(*parsed);
    if (!cellFunction || !cellFunction->block) {
        result.errorMessage = "internal REPL cell function was not parsed";
        return result;
    }

    m_interpreter.refreshSessionProgram(m_contract);
    auto execResult = m_interpreter.executeReplBlock(*cellFunction->block);
    result.success = execResult.success;
    result.errorMessage = std::move(execResult.errorMessage);
    result.hasOutput = execResult.hasOutput;
    result.outputValues = std::move(execResult.outputValues);
    return result;
}

std::shared_ptr<ContractNode> ReplSession::parseWrappedSource(
    const std::string& filename,
    const std::string& wrappedSource,
    ReplCellResult& result
) const
{
    ErrorManager::getInstance().clear();
    ErrorManager::getInstance().setColorOutput(false);
    ErrorManager::getInstance().setShowContext(false);
    ErrorManager::getInstance().setSourceContent(filename, wrappedSource);

    apc_frontend::FrontendOptions options;
    options.mergeLibraries = true;
    auto frontendResult =
        apc_frontend::compileFrontendToAst(filename, wrappedSource, options);

    if (!frontendResult.success || !frontendResult.ast) {
        result.errorMessage = frontendResult.errorMessage.empty()
                                  ? "front-end analysis failed"
                                  : frontendResult.errorMessage;
        return nullptr;
    }
    return frontendResult.ast;
}

FunctionNode* ReplSession::findCellFunction(ContractNode& contract) const
{
    for (const auto& member : contract.members) {
        if (auto* function = dynamic_cast<FunctionNode*>(member.get())) {
            if (function->name == kCellFunctionName) {
                return function;
            }
        }
    }
    return nullptr;
}

void ReplSession::mergeMembers(ContractNode& parsedContract)
{
    for (auto& member : parsedContract.members) {
        const std::string key = memberKey(*member);
        if (!key.empty()) {
            auto& members = m_contract.members;
            members.erase(
                std::remove_if(
                    members.begin(),
                    members.end(),
                    [&](const std::unique_ptr<ASTNode>& existing) {
                        return memberKey(*existing) == key;
                    }
                ),
                members.end()
            );
        }
        m_contract.members.push_back(std::move(member));
    }
    parsedContract.members.clear();
}

} // namespace apc_repl

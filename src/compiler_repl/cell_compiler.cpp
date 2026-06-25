#include "cell_compiler.h"

#include <algorithm>
#include <map>
#include <optional>
#include <regex>
#include <sstream>

#include "../util/string_utils.h"

#ifdef ENABLE_DEBUGGER
#include "../debugger/vm/bvm_simulator.h"
#endif

namespace apc::repl
{
namespace
{
using apc::util::startsWith;
using apc::util::trim;

size_t countNewlines(const std::string& text)
{
    return static_cast<size_t>(std::count(text.begin(), text.end(), '\n'));
}

class SourceBuilder
{
public:
    void append(const std::string& text)
    {
        m_out << text;
        m_nextLine += countNewlines(text);
    }

    size_t nextLine() const
    {
        return m_nextLine;
    }

    std::string str() const
    {
        return m_out.str();
    }

private:
    std::ostringstream m_out;
    size_t m_nextLine = 1;
};

std::string firstLogicalLine(const std::string& text)
{
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (!line.empty()) {
            return line;
        }
    }
    return "";
}

bool hasMultipleLines(const std::string& text)
{
    return text.find('\n') != std::string::npos;
}

bool looksLikeDeclaration(const std::string& line)
{
    static const std::regex declRegex(
        R"(^[A-Za-z_][A-Za-z0-9_]*\s*:\s*.+)"
    );
    return std::regex_match(line, declRegex);
}

bool looksLikeAssignment(const std::string& line)
{
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '=') {
            continue;
        }

        char prev = (i > 0) ? line[i - 1] : '\0';
        char next = (i + 1 < line.size()) ? line[i + 1] : '\0';
        if (prev == '=' || prev == '!' || prev == '<' || prev == '>' ||
            next == '=') {
            continue;
        }
        return true;
    }
    return false;
}

bool looksLikeSideEffectCall(const std::string& line)
{
    static const std::regex sideEffectRegex(
        R"(^(SetAlt|SetMain|Keep|Delete|Pop|EqualVerify|Verify)\s*\()"
    );
    return std::regex_search(line, sideEffectRegex);
}

bool isNumberLiteral(const std::string& text)
{
    static const std::regex numberRegex(R"(^[+-]?[0-9]+$)");
    return std::regex_match(trim(text), numberRegex);
}

std::string indentBlock(const std::string& text, int spaces)
{
    std::string indent(static_cast<size_t>(spaces), ' ');
    std::istringstream iss(text);
    std::ostringstream out;
    std::string line;
    while (std::getline(iss, line)) {
        if (trim(line).empty()) {
            out << "\n";
        } else {
            out << indent << line << "\n";
        }
    }
    return out.str();
}

size_t findMatchingParen(const std::string& text, size_t openPos)
{
    int depth = 0;
    bool inString = false;
    bool escaped = false;

    for (size_t i = openPos; i < text.size(); ++i) {
        char c = text[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
        } else if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }

    return std::string::npos;
}

std::vector<std::string> splitTopLevelArgs(const std::string& args)
{
    std::vector<std::string> result;
    size_t start = 0;
    int depth = 0;
    bool inString = false;
    bool escaped = false;

    for (size_t i = 0; i < args.size(); ++i) {
        char c = args[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
        } else if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            --depth;
        } else if (c == ',' && depth == 0) {
            result.push_back(args.substr(start, i - start));
            start = i + 1;
        }
    }

    result.push_back(args.substr(start));
    return result;
}

std::string wrapNumericArgsForAliasCall(
    const std::string& text,
    const std::string& privateName
)
{
    std::string result;
    size_t pos = 0;
    std::string needle = privateName + "(";

    while (true) {
        size_t callPos = text.find(needle, pos);
        if (callPos == std::string::npos) {
            result += text.substr(pos);
            break;
        }

        size_t openPos = callPos + privateName.size();
        size_t closePos = findMatchingParen(text, openPos);
        if (closePos == std::string::npos) {
            result += text.substr(pos);
            break;
        }

        result += text.substr(pos, callPos - pos);
        result += privateName;
        result += "(";

        auto args = splitTopLevelArgs(text.substr(openPos + 1,
                                                  closePos - openPos - 1));
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) {
                result += ", ";
            }
            std::string arg = trim(args[i]);
            if (isNumberLiteral(arg)) {
                result += "Push(" + arg + ")";
            } else {
                result += arg;
            }
        }

        result += ")";
        pos = closePos + 1;
    }

    return result;
}

std::map<std::string, std::string>
collectFunctionAliases(const std::vector<std::string>& members)
{
    std::map<std::string, std::string> aliases;
    static const std::regex defRegex(
        R"(^\s*def\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()"
    );

    for (const auto& member : members) {
        std::istringstream iss(member);
        std::string line;
        while (std::getline(iss, line)) {
            std::smatch match;
            if (!std::regex_search(line, match, defRegex)) {
                continue;
            }

            std::string name = match[1].str();
            if (!name.empty() && name[0] != '_' && name != "__init__") {
                aliases[name] = "_" + name;
            }
        }
    }

    return aliases;
}

std::string rewriteFunctionDefinitions(
    const std::string& text,
    const std::map<std::string, std::string>& aliases
)
{
    static const std::regex defLineRegex(
        R"(^(\s*def\s+)([A-Za-z_][A-Za-z0-9_]*)(\s*\(.*)$)"
    );

    std::istringstream iss(text);
    std::ostringstream out;
    std::string line;
    while (std::getline(iss, line)) {
        std::smatch match;
        if (std::regex_match(line, match, defLineRegex)) {
            std::string name = match[2].str();
            auto it = aliases.find(name);
            if (it != aliases.end()) {
                out << match[1].str() << it->second << match[3].str()
                    << "\n";
                continue;
            }
        }
        out << line << "\n";
    }
    return out.str();
}

std::string rewriteFunctionCalls(
    std::string text,
    const std::map<std::string, std::string>& aliases
)
{
    for (const auto& [publicName, privateName] : aliases) {
        std::regex callRegex("\\b" + publicName + R"(\s*\()");
        text = std::regex_replace(text, callRegex, privateName + "(");
        text = wrapNumericArgsForAliasCall(text, privateName);
    }
    return text;
}

std::string rewriteMember(
    const std::string& text,
    const std::map<std::string, std::string>& aliases
)
{
    return rewriteFunctionCalls(rewriteFunctionDefinitions(text, aliases), aliases);
}

struct BytecodeRange
{
    size_t start = 0;
    size_t end = 0;

    bool valid() const
    {
        return start < end;
    }
};

bool isZeroPush(const std::string& instruction)
{
    return instruction == "OP_0" || instruction == "OP_FALSE" ||
           instruction == "00";
}

bool isReturnInstruction(const std::string& instruction)
{
    return instruction == "OP_RETURN" || instruction == "6a" ||
           instruction == "6A";
}

std::optional<size_t>
findLastReturnPC(const std::vector<std::string>& instructions)
{
    for (size_t i = instructions.size(); i > 0; --i) {
        size_t pc = i - 1;
        if (isReturnInstruction(instructions[pc])) {
            return pc;
        }
    }
    return std::nullopt;
}

std::optional<size_t>
findFinalReturnValueStart(const std::vector<std::string>& instructions)
{
    auto returnPC = findLastReturnPC(instructions);
    if (!returnPC.has_value()) {
        return std::nullopt;
    }

    if (*returnPC > 0 && isZeroPush(instructions[*returnPC - 1])) {
        return *returnPC - 1;
    }
    return *returnPC;
}

std::optional<size_t>
findBodyEnd(const std::vector<std::string>& instructions)
{
    auto returnPC = findLastReturnPC(instructions);
    if (!returnPC.has_value()) {
        return std::nullopt;
    }
    return *returnPC + 1;
}

std::optional<BytecodeRange> findBytecodeRangeFromBaseline(
    const CompilerResult& baseline,
    const CompilerResult& current,
    CellKind kind
)
{
    auto start = findFinalReturnValueStart(baseline.asmInstructions);
    if (!start.has_value()) {
        return std::nullopt;
    }

    std::optional<size_t> end;
    if (kind == CellKind::Statement) {
        end = findFinalReturnValueStart(current.asmInstructions);
    } else {
        end = findBodyEnd(current.asmInstructions);
    }

    if (!end.has_value() || *start >= *end ||
        *end > current.asmInstructions.size()) {
        return std::nullopt;
    }

    return BytecodeRange{*start, *end};
}

#ifdef ENABLE_DEBUGGER
std::string formatStackElement(const apc_debug::StackElement& element)
{
    bool printable = !element.data.empty();
    std::string text;
    text.reserve(element.data.size());
    for (uint8_t b : element.data) {
        if (b < 32 || b > 126) {
            printable = false;
            break;
        }
        text.push_back(static_cast<char>(b));
    }

    if (printable && element.data.size() > 1) {
        return "\"" + text + "\"";
    }

    auto intValue = element.toInt();
    if (intValue.has_value()) {
        return std::to_string(*intValue);
    }

    return element.toHexString(true);
}
#endif
} // namespace

const char* cellKindName(CellKind kind)
{
    switch (kind) {
        case CellKind::Empty:
            return "empty";
        case CellKind::Import:
            return "import";
        case CellKind::Member:
            return "member";
        case CellKind::Statement:
            return "statement";
        case CellKind::Expression:
            return "expression";
        case CellKind::Contract:
            return "contract";
    }
    return "unknown";
}

CellKind CellCompiler::classify(const std::string& cellText) const
{
    std::string text = trim(cellText);
    if (text.empty()) {
        return CellKind::Empty;
    }

    std::string first = firstLogicalLine(text);
    if (startsWith(first, "Contract ")) {
        return CellKind::Contract;
    }
    if (startsWith(first, "import ")) {
        return CellKind::Import;
    }
    if (startsWith(first, "def ") || startsWith(first, "Struct ")) {
        return CellKind::Member;
    }
    if (hasMultipleLines(text) || startsWith(first, "if ") ||
        startsWith(first, "for ") || startsWith(first, "Return ") ||
        startsWith(first, "return ")) {
        return CellKind::Statement;
    }
    if (looksLikeDeclaration(first) || looksLikeAssignment(first) ||
        looksLikeSideEffectCall(first)) {
        return CellKind::Statement;
    }

    return CellKind::Expression;
}

CellExecutionResult CellCompiler::executeCell(
    ReplSession& session,
    int inputIndex,
    const std::string& cellText
) const
{
    CellExecutionResult execResult;
    execResult.kind = classify(cellText);

    if (execResult.kind == CellKind::Empty) {
        execResult.success = true;
        return execResult;
    }

    std::string entryFunction;
    size_t cellStartLine = 0;
    size_t cellEndLine = 0;
    std::string source = renderSessionSource(
        session, cellText, execResult.kind, inputIndex, &entryFunction,
        &cellStartLine, &cellEndLine
    );
    std::string sourceFile =
        "<repl-cell-" + std::to_string(inputIndex) + ">";

    std::optional<BytecodeRange> pcRange;
    if (execResult.kind == CellKind::Statement ||
        execResult.kind == CellKind::Expression) {
        std::string baselineEntryFunction;
        std::string baselineSource = renderSessionSource(
            session, "", CellKind::Statement, inputIndex,
            &baselineEntryFunction
        );
        CellExecutionResult baselineResult = compileSourceForCell(
            baselineSource,
            "<repl-baseline-" + std::to_string(inputIndex) + ">"
        );
        if (!baselineResult.success) {
            execResult.success = false;
            execResult.errorMessage =
                "current session failed to compile before this cell: " +
                baselineResult.errorMessage;
            return execResult;
        }

        execResult = compileSourceForCell(source, sourceFile);
        if (execResult.success) {
            pcRange = findBytecodeRangeFromBaseline(
                baselineResult.compileResult, execResult.compileResult,
                classify(cellText)
            );
        }
    } else {
        execResult = compileSourceForCell(source, sourceFile);
    }
    execResult.kind = classify(cellText);
    execResult.syntheticSource = source;
    execResult.entryFunction = entryFunction;

    if (!execResult.success) {
        return execResult;
    }

#ifdef ENABLE_DEBUGGER
    apc_debug::StackState initialMainStack;
    apc_debug::StackState initialAltStack;
    if (session.hasVmState()) {
        initialMainStack = session.mainStack();
        initialAltStack = session.altStack();
    }
#endif

    if (execResult.kind == CellKind::Statement ||
        execResult.kind == CellKind::Expression) {
        bool ok = false;
        std::string errorMessage;
        bool commitStacks = execResult.kind == CellKind::Statement;
        std::string output = runCellRange(
            execResult.compileResult, session, entryFunction,
            pcRange.has_value(), pcRange ? pcRange->start : 0,
            pcRange ? pcRange->end : 0, commitStacks, ok, errorMessage
        );
        if (!ok) {
            execResult.success = false;
            execResult.errorMessage = errorMessage;
            return execResult;
        }

        if (execResult.kind == CellKind::Expression) {
            execResult.hasOutput = !output.empty();
            execResult.output = output;
        }
    } else if (execResult.kind == CellKind::Contract) {
        bool ok = false;
        std::string errorMessage;
        std::string output = runEntryFunction(
            execResult.compileResult, entryFunction, ok, errorMessage
        );
        if (!ok) {
            execResult.success = false;
            execResult.errorMessage = errorMessage;
            return execResult;
        }
        execResult.hasOutput = !output.empty();
        execResult.output = output;
    }

    switch (execResult.kind) {
        case CellKind::Import:
            session.addImport(cellText);
            break;
        case CellKind::Member:
            session.addMember(cellText);
            break;
        case CellKind::Statement:
            session.addStatement(cellText);
            break;
        case CellKind::Expression:
            if (execResult.hasOutput) {
                session.setOutput(inputIndex, execResult.output);
            }
            break;
        case CellKind::Contract:
        case CellKind::Empty:
            break;
    }

    session.setLastCompile(
        execResult.compileResult, execResult.syntheticSource, entryFunction,
        cellStartLine > 0 && cellEndLine >= cellStartLine, cellStartLine,
        cellEndLine, pcRange.has_value(), pcRange ? pcRange->start : 0,
        pcRange ? pcRange->end : 0
    );
#ifdef ENABLE_DEBUGGER
    if (execResult.kind == CellKind::Statement ||
        execResult.kind == CellKind::Expression) {
        session.setLastInitialVmState(initialMainStack, initialAltStack);
    } else {
        session.clearLastInitialVmState();
    }
#endif
    return execResult;
}

CompilerResult CellCompiler::compileSession(
    const ReplSession& session,
    bool enableDebug
) const
{
    std::string entryFunction;
    std::string source = renderSessionSource(
        session, "", CellKind::Statement, session.nextInputIndex(),
        &entryFunction
    );

    CompilerOptions options;
    options.enableDebug = enableDebug;
    options.colorDiagnostics = true;
    options.showDiagnosticContext = true;
    options.codeFileName = "apc_repl_session";

    return CompilerDriver::compileSource("<repl-session>", source, options);
}

std::string CellCompiler::renderSessionSource(
    const ReplSession& session,
    const std::string& candidate,
    CellKind kind,
    int inputIndex,
    std::string* outEntryFunction,
    size_t* outCellStartLine,
    size_t* outCellEndLine
) const
{
    if (outCellStartLine) {
        *outCellStartLine = 0;
    }
    if (outCellEndLine) {
        *outCellEndLine = 0;
    }

    if (kind == CellKind::Contract) {
        if (outEntryFunction) {
            *outEntryFunction = "";
        }
        if (outCellStartLine) {
            *outCellStartLine = trim(candidate).empty() ? 0 : 1;
        }
        if (outCellEndLine) {
            *outCellEndLine =
                trim(candidate).empty() ? 0 : countNewlines(candidate) + 1;
        }
        return candidate;
    }

    SourceBuilder out;
    for (const auto& importLine : session.imports()) {
        out.append(importLine + "\n");
    }
    if (kind == CellKind::Import && !trim(candidate).empty()) {
        if (outCellStartLine) {
            *outCellStartLine = out.nextLine();
        }
        out.append(candidate + "\n");
        if (outCellEndLine) {
            *outCellEndLine = out.nextLine() - 1;
        }
    }

    std::string entryFunction = "repl_cell_" + std::to_string(inputIndex);
    if (outEntryFunction) {
        *outEntryFunction = entryFunction;
    }

    std::vector<std::string> aliasMembers = session.members();
    if (kind == CellKind::Member && !trim(candidate).empty()) {
        aliasMembers.push_back(candidate);
    }
    auto aliases = collectFunctionAliases(aliasMembers);

    out.append("\nContract __ReplSession:\n\n");
    for (const auto& member : session.members()) {
        std::string rewritten = rewriteMember(member, aliases);
        if (!trim(rewritten).empty()) {
            out.append(indentBlock(rewritten, 4) + "\n");
        }
    }
    if (kind == CellKind::Member && !trim(candidate).empty()) {
        if (outCellStartLine) {
            *outCellStartLine = out.nextLine();
        }
        out.append(indentBlock(rewriteMember(candidate, aliases), 4) + "\n");
        if (outCellEndLine) {
            *outCellEndLine = out.nextLine() - 1;
        }
    }

    out.append("    def " + entryFunction + "():\n");
    for (const auto& statement : session.statements()) {
        std::string rewritten = rewriteFunctionCalls(statement, aliases);
        if (!trim(rewritten).empty()) {
            out.append(indentBlock(rewritten, 8) + "\n");
        }
    }

    if (kind == CellKind::Statement && !trim(candidate).empty()) {
        if (outCellStartLine) {
            *outCellStartLine = out.nextLine();
        }
        out.append(indentBlock(rewriteFunctionCalls(candidate, aliases), 8));
        if (outCellEndLine) {
            *outCellEndLine = out.nextLine() - 1;
        }
        out.append("        Return 0\n");
    } else if (kind == CellKind::Expression && !trim(candidate).empty()) {
        if (outCellStartLine) {
            *outCellStartLine = out.nextLine();
        }
        out.append(
            "        Return " +
            trim(rewriteFunctionCalls(candidate, aliases)) + "\n"
        );
        if (outCellEndLine) {
            *outCellEndLine = out.nextLine() - 1;
        }
    } else {
        out.append("        Return 0\n");
    }

    return out.str();
}

CellExecutionResult CellCompiler::compileSourceForCell(
    const std::string& source,
    const std::string& sourceFile
) const
{
    CellExecutionResult execResult;

    CompilerOptions options;
    options.enableDebug = true;
    options.colorDiagnostics = true;
    options.showDiagnosticContext = true;
    options.codeFileName = "apc_repl_cell";

    execResult.compileResult =
        CompilerDriver::compileSource(sourceFile, source, options);
    if (!execResult.compileResult.success) {
        execResult.errorMessage = execResult.compileResult.errorMessage;
        return execResult;
    }

    execResult.success = true;
    return execResult;
}

std::string CellCompiler::runCellRange(
    const CompilerResult& result,
    ReplSession& session,
    const std::string& entryFunction,
    bool hasPCRange,
    size_t pcStart,
    size_t pcEnd,
    bool commitStacks,
    bool& ok,
    std::string& errorMessage
) const
{
#ifndef ENABLE_DEBUGGER
    (void)result;
    (void)session;
    (void)entryFunction;
    (void)hasPCRange;
    (void)pcStart;
    (void)pcEnd;
    (void)commitStacks;
    ok = false;
    errorMessage = "REPL execution requires BUILD_DEBUGGER=ON";
    return "";
#else
    if (result.asmInstructions.empty()) {
        ok = false;
        errorMessage = "no bytecode instructions to execute";
        return "";
    }

    (void)entryFunction;

    if (!hasPCRange || pcStart >= pcEnd ||
        pcEnd > result.asmInstructions.size()) {
        if (commitStacks) {
            apc_debug::StackState main;
            apc_debug::StackState alt;
            if (session.hasVmState()) {
                main = session.mainStack();
                alt = session.altStack();
            }
            session.setVmState(main, alt);
            ok = true;
            return "";
        }

        ok = false;
        errorMessage = "no executable bytecode found for this cell";
        return "";
    }

    auto vm = std::make_shared<apc_debug::BVMSimulator>(
        result.asmInstructions, result.debugInfo
    );

    if (session.hasVmState()) {
        vm->setInitialStacks(session.mainStack(), session.altStack());
    }

    try {
        vm->setExecutionRange(pcStart, pcEnd);
    } catch (const std::exception& e) {
        ok = false;
        errorMessage = e.what();
        return "";
    }

    vm->run();
    if (vm->hasError()) {
        ok = false;
        errorMessage = vm->getLastError();
        return "";
    }

    if (commitStacks) {
        session.setVmState(vm->getMainStack(), vm->getAltStack());
    }

    ok = true;
    if (vm->getMainStack().empty()) {
        return "";
    }
    return formatStackElement(vm->getMainStack().peek(0));
#endif
}

std::string CellCompiler::runEntryFunction(
    const CompilerResult& result,
    const std::string& entryFunction,
    bool& ok,
    std::string& errorMessage
) const
{
#ifndef ENABLE_DEBUGGER
    (void)result;
    (void)entryFunction;
    ok = false;
    errorMessage = "REPL execution requires BUILD_DEBUGGER=ON";
    return "";
#else
    if (result.asmInstructions.empty()) {
        ok = false;
        errorMessage = "no bytecode instructions to execute";
        return "";
    }

    auto vm = std::make_shared<apc_debug::BVMSimulator>(
        result.asmInstructions, result.debugInfo
    );

    if (!entryFunction.empty() && result.debugInfo) {
        auto it = result.debugInfo->functions.find(entryFunction);
        if (it != result.debugInfo->functions.end()) {
            size_t startPC = it->second.startPC;
            size_t endPC = it->second.endPC;
            if (endPC == 0 || endPC > result.asmInstructions.size()) {
                endPC = result.asmInstructions.size();
            }
            if (startPC < endPC) {
                try {
                    vm->setExecutionRange(startPC, endPC);
                } catch (const std::exception& e) {
                    ok = false;
                    errorMessage = e.what();
                    return "";
                }
            }
        }
    }

    vm->run();
    if (vm->hasError()) {
        ok = false;
        errorMessage = vm->getLastError();
        return "";
    }

    ok = true;
    if (vm->getMainStack().empty()) {
        return "";
    }
    return formatStackElement(vm->getMainStack().peek(0));
#endif
}

} // namespace apc::repl

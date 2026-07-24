#ifndef DEBUG_INFO_H
#define DEBUG_INFO_H

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace apc_debug
{

// 行/列从 1 开始
struct SourceLocation
{
    std::string filename;
    size_t line;
    size_t column;
    size_t endLine;
    size_t endColumn;

    SourceLocation() : line(0), column(0), endLine(0), endColumn(0)
    {}

    SourceLocation(const std::string& file, size_t l, size_t c)
        : filename(file), line(l), column(c), endLine(l), endColumn(c)
    {}

    SourceLocation(
        const std::string& file,
        size_t l,
        size_t c,
        size_t el,
        size_t ec
    )
        : filename(file), line(l), column(c), endLine(el), endColumn(ec)
    {}

    bool isValid() const
    {
        return line > 0;
    }

    std::string toString() const;
};

struct VariableDebugInfo
{
    std::string name;
    std::string type;
    std::string scopeName;
    size_t declLine;
    size_t declColumn;
    bool isStackVar;
    int stackOffset;       // -1 表示未知
    bool isParameter;

    VariableDebugInfo()
        : declLine(0), declColumn(0), isStackVar(true), stackOffset(-1),
          isParameter(false)
    {}
};

struct InstructionDebugInfo
{
    size_t pc;
    SourceLocation location;
    std::string opcode;
    std::string operand;
    std::vector<std::string> affectedVars;

    InstructionDebugInfo() : pc(0)
    {}
    InstructionDebugInfo(size_t p, const SourceLocation& loc)
        : pc(p), location(loc)
    {}
};

enum class ScopeType { GLOBAL, CONTRACT, FUNCTION, BLOCK, LOOP, CONDITIONAL };

struct ScopeDebugInfo
{
    std::string name;
    ScopeType type;
    SourceLocation location;
    size_t startPC;
    size_t endPC;
    std::vector<VariableDebugInfo> variables;
    std::shared_ptr<ScopeDebugInfo> parent;
    std::vector<std::shared_ptr<ScopeDebugInfo>> children;

    ScopeDebugInfo() : type(ScopeType::BLOCK), startPC(0), endPC(0)
    {}

    ScopeDebugInfo(const std::string& n, ScopeType t)
        : name(n), type(t), startPC(0), endPC(0)
    {}

    void addVariable(const VariableDebugInfo& var);

    // 含父作用域
    const VariableDebugInfo* findVariable(const std::string& name) const;
};

struct FunctionDebugInfo
{
    std::string name;
    SourceLocation location;
    size_t startPC;
    size_t endPC;
    std::vector<VariableDebugInfo> parameters;
    std::vector<VariableDebugInfo> localVars;
    std::shared_ptr<ScopeDebugInfo> scope;
    bool isPublic;

    FunctionDebugInfo() : startPC(0), endPC(0), isPublic(false)
    {}

    FunctionDebugInfo(const std::string& n)
        : name(n), startPC(0), endPC(0), isPublic(false)
    {}
};

class DebugInfo
{
public:
    DebugInfo() = default;

    // DebugInfoGenerator 在无法安全恢复的 enter/exit 错配时置 false。
    bool scopeNestingValid{true};
    std::string sourceFilename;
    std::string contractName;
    std::string version;

    std::map<size_t, SourceLocation> pcToSource;
    std::map<size_t, std::vector<size_t>> lineToPCs;

    std::map<size_t, InstructionDebugInfo> instructions;

    std::map<std::string, FunctionDebugInfo> functions;

    std::vector<std::shared_ptr<ScopeDebugInfo>> scopes;
    std::shared_ptr<ScopeDebugInfo> globalScope;

    std::map<std::string, VariableDebugInfo> variables;

    void addSourceMapping(size_t pc, const SourceLocation& loc);

    void addInstruction(const InstructionDebugInfo& info);
    void addInstruction(
        size_t pc,
        const std::string& opcode,
        const std::string& operand,
        const SourceLocation& loc
    );

    void addFunction(const FunctionDebugInfo& info);
    void addVariable(const VariableDebugInfo& info);
    void addScope(std::shared_ptr<ScopeDebugInfo> scope);

    SourceLocation getSourceLocation(size_t pc) const;
    std::vector<size_t> getPCsForLine(size_t line) const;
    std::vector<size_t> getPCsForSourceLine(
        const std::string& filename,
        size_t line
    ) const;

    // 用于断点解析
    std::vector<size_t>
    findNearestValidLine(size_t line, size_t maxDistance = 10) const;
    std::vector<size_t> findNearestValidSourceLine(
        const std::string& filename,
        size_t line,
        size_t maxDistance = 10
    ) const;

    const FunctionDebugInfo* getFunctionAtPC(size_t pc) const;

    // PC 所在作用域内可见的全部变量
    std::vector<VariableDebugInfo> getVariablesInScope(size_t pc) const;

    const VariableDebugInfo* getVariableInfo(const std::string& varName) const;
    std::shared_ptr<ScopeDebugInfo> getScopeAtPC(size_t pc) const;

    bool save(const std::string& filename) const;
    static std::shared_ptr<DebugInfo> load(const std::string& filename);
    std::string toJson() const;
    static std::shared_ptr<DebugInfo> fromJson(const std::string& jsonStr);

    size_t getTotalInstructions() const
    {
        return instructions.size();
    }
    size_t getTotalFunctions() const
    {
        return functions.size();
    }
    size_t getTotalVariables() const
    {
        return variables.size();
    }

    // 校验调试元数据的内部结构。传入最终字节码指令后，还会校验所有
    // PC 都落在真实指令边界上，并核对指令内容。
    bool validate(std::string* errorMessage = nullptr) const;
    bool validate(
        const std::vector<std::string>& bytecode,
        std::string* errorMessage = nullptr
    ) const;

    // DebugInfoGenerator 用于构建反向索引
    void buildLineToPC();

    // oldToNew[old_pc] == SIZE_MAX 表示该旧 PC 已被优化删除。
    void remapPCs(
        const std::vector<size_t>& oldToNew,
        size_t newInstructionCount
    );

    void syncInstructionOpcodes(const std::vector<std::string>& bytecode);

private:
};

std::string scopeTypeToString(ScopeType type);
ScopeType stringToScopeType(const std::string& str);

} // namespace apc_debug

#endif // DEBUG_INFO_H

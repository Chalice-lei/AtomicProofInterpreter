#ifndef DEBUG_INFO_H
#define DEBUG_INFO_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace apc_debug
{

using ScopeId = std::uint64_t;
using ControlRegionId = std::uint64_t;

inline constexpr ScopeId INVALID_SCOPE_ID = 0;
inline constexpr size_t UNKNOWN_ORIGINAL_PC =
    std::numeric_limits<size_t>::max();

enum class BranchArm { Then, Else };

struct BranchPredicate
{
    ControlRegionId region = 0;
    BranchArm arm = BranchArm::Then;

    bool operator==(const BranchPredicate&) const = default;
};

// Records the branch actually selected for each structured control-flow region.
using BranchTrace = std::map<ControlRegionId, BranchArm>;

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

struct SourceOrigin
{
    SourceLocation location;
    ScopeId scopeId = INVALID_SCOPE_ID;
    std::string functionName;
    size_t originalPC = UNKNOWN_ORIGINAL_PC;
    std::vector<BranchPredicate> path;
    std::vector<std::string> affectedVars;

    SourceOrigin() = default;
    explicit SourceOrigin(const SourceLocation& loc) : location(loc)
    {}
};

struct PCRange
{
    size_t beginPC = 0;
    size_t endPC = 0;

    PCRange() = default;
    PCRange(size_t begin, size_t end) : beginPC(begin), endPC(end)
    {}

    bool isValid() const
    {
        return beginPC < endPC;
    }

    bool contains(size_t pc) const
    {
        return pc >= beginPC && pc < endPC;
    }

    bool operator==(const PCRange&) const = default;
};

// Neutral rewrite input used to transfer provenance without coupling the
// debugger to a particular optimizer implementation.
struct OriginRewriteRef
{
    size_t oldPC = UNKNOWN_ORIGINAL_PC;
    std::vector<BranchPredicate> path;
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
    ScopeId scopeId;
    // Half-open intervals in final bytecode PC space. Older debug files do
    // not carry this information; in that case the lexical scope remains the
    // availability boundary.
    bool hasExplicitAvailability;
    std::vector<PCRange> availabilityRanges;

    VariableDebugInfo()
        : declLine(0), declColumn(0), isStackVar(true), stackOffset(-1),
          isParameter(false), scopeId(INVALID_SCOPE_ID),
          hasExplicitAvailability(false)
    {}

    bool isAvailableAtPC(size_t pc) const;
    void setAvailabilityRange(size_t beginPC, size_t endPC);
};

struct InstructionDebugInfo
{
    size_t pc;
    SourceLocation location;
    std::string opcode;
    std::string operand;
    std::vector<std::string> affectedVars;
    // origins[0] is the compatibility/primary origin. A rewritten instruction
    // may have more than one path-qualified source origin.
    std::vector<SourceOrigin> origins;

    InstructionDebugInfo() : pc(0)
    {}
    InstructionDebugInfo(size_t p, const SourceLocation& loc)
        : pc(p), location(loc)
    {}
};

enum class ScopeType { GLOBAL, CONTRACT, FUNCTION, BLOCK, LOOP, CONDITIONAL };

struct ScopeDebugInfo;

// V2 keeps the reverse scope link weak to avoid parent/child ownership cycles.
// Interpreter code predating V2 also treated `parent` like a shared_ptr, so
// this adapter preserves that source-level API while retaining weak ownership.
class ScopeParentRef
{
public:
    ScopeParentRef() = default;
    ScopeParentRef(const std::shared_ptr<ScopeDebugInfo>& parent)
        : m_parent(parent)
    {}

    ScopeParentRef& operator=(const std::shared_ptr<ScopeDebugInfo>& parent)
    {
        m_parent = parent;
        return *this;
    }

    std::shared_ptr<ScopeDebugInfo> lock() const noexcept
    {
        return m_parent.lock();
    }

    bool expired() const noexcept
    {
        return m_parent.expired();
    }

    void reset() noexcept
    {
        m_parent.reset();
    }

    explicit operator bool() const noexcept
    {
        return !m_parent.expired();
    }

    operator std::shared_ptr<ScopeDebugInfo>() const noexcept
    {
        return lock();
    }

    std::shared_ptr<ScopeDebugInfo> operator->() const noexcept
    {
        return lock();
    }

    friend bool operator==(
        const ScopeParentRef& lhs,
        const std::shared_ptr<ScopeDebugInfo>& rhs
    ) noexcept
    {
        return lhs.lock() == rhs;
    }

    friend bool operator==(
        const std::shared_ptr<ScopeDebugInfo>& lhs,
        const ScopeParentRef& rhs
    ) noexcept
    {
        return lhs == rhs.lock();
    }

private:
    std::weak_ptr<ScopeDebugInfo> m_parent;
};

struct ScopeDebugInfo
{
    ScopeId scopeId;
    std::string name;
    ScopeType type;
    SourceLocation location;
    size_t startPC;
    size_t endPC;
    // V2 representation. All intervals are half-open. startPC/endPC remain as
    // the compatibility envelope for V1 consumers.
    std::vector<PCRange> ranges;
    std::vector<VariableDebugInfo> variables;
    // A scope owns its children. The reverse link is observational only and
    // must not create a shared_ptr ownership cycle.
    ScopeParentRef parent;
    std::vector<std::shared_ptr<ScopeDebugInfo>> children;

    ScopeDebugInfo()
        : scopeId(INVALID_SCOPE_ID), type(ScopeType::BLOCK), startPC(0),
          endPC(0)
    {}

    ScopeDebugInfo(const std::string& n, ScopeType t)
        : scopeId(INVALID_SCOPE_ID), name(n), type(t), startPC(0), endPC(0)
    {}

    void addVariable(const VariableDebugInfo& var);
    void setRange(size_t beginPC, size_t endPC);
    void addRange(size_t beginPC, size_t endPC);
    bool containsPC(size_t pc) const;
    size_t coveredInstructionCount() const;

    // 含父作用域
    const VariableDebugInfo* findVariable(const std::string& name) const;
    const VariableDebugInfo*
    findVariable(const std::string& name, size_t pc) const;
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
    ScopeId scopeId;
    bool isPublic;

    FunctionDebugInfo()
        : startPC(0), endPC(0), scopeId(INVALID_SCOPE_ID), isPublic(false)
    {}

    FunctionDebugInfo(const std::string& n)
        : name(n), startPC(0), endPC(0), scopeId(INVALID_SCOPE_ID),
          isPublic(false)
    {}
};

class DebugInfo
{
public:
    DebugInfo() = default;

    // Interpreter compatibility: the existing generator and validation
    // pipeline use this flag to detect unbalanced enter/exit notifications.
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
    void addInstructionOrigin(size_t pc, const SourceOrigin& origin);
    void setInstructionOrigins(
        size_t pc,
        const std::vector<SourceOrigin>& origins
    );

    void addFunction(const FunctionDebugInfo& info);
    void addVariable(const VariableDebugInfo& info);
    void addScope(std::shared_ptr<ScopeDebugInfo> scope);

    SourceLocation getSourceLocation(size_t pc) const;
    SourceLocation getSourceLocation(
        size_t pc,
        const BranchTrace& branchTrace
    ) const;
    std::vector<SourceOrigin> getOriginsForPC(size_t pc) const;
    const SourceOrigin* resolveOrigin(
        size_t pc,
        const BranchTrace& branchTrace
    ) const;
    bool hasActiveSourceOrigin(
        size_t pc,
        const std::string& filename,
        size_t line,
        const BranchTrace& branchTrace
    ) const;
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
    std::vector<VariableDebugInfo> getVariablesInScope(
        size_t pc,
        const BranchTrace& branchTrace
    ) const;

    const VariableDebugInfo* getVariableInfo(const std::string& varName) const;
    const VariableDebugInfo* getVariableInfo(
        const std::string& varName,
        size_t pc,
        const BranchTrace& branchTrace
    ) const;
    std::shared_ptr<ScopeDebugInfo> getScopeAtPC(size_t pc) const;
    std::shared_ptr<ScopeDebugInfo> getScopeAtPC(
        size_t pc,
        const BranchTrace& branchTrace
    ) const;
    std::shared_ptr<ScopeDebugInfo> getScopeById(ScopeId scopeId) const;

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

    // Returns a deep, validated rewritten copy. newToOldOrigins may be empty;
    // otherwise each output PC explicitly names all old origins and their
    // branch paths. The receiver is never modified on failure.
    std::shared_ptr<DebugInfo> remapped(
        const std::vector<size_t>& oldToNew,
        size_t newInstructionCount,
        const std::vector<std::vector<OriginRewriteRef>>& newToOldOrigins = {}
    ) const;

    // Compatibility convenience around remapped(). Commits only after the
    // complete candidate has passed validation.
    bool applyRemapTransactional(
        const std::vector<size_t>& oldToNew,
        size_t newInstructionCount,
        const std::vector<std::vector<OriginRewriteRef>>& newToOldOrigins = {}
    );

    void syncInstructionOpcodes(const std::vector<std::string>& bytecode);

private:
    bool validateV2Core() const;
};

std::string scopeTypeToString(ScopeType type);
ScopeType stringToScopeType(const std::string& str);

} // namespace apc_debug

#endif // DEBUG_INFO_H

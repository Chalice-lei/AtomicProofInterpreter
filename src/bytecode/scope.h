#ifndef SCOPE_H
#define SCOPE_H

#include <any>
#include <memory>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

#include "../util/op_stack.h"
#include "byt_defs.h"
#include "symtab.h"

SPACE_TBC_START

enum class ControlFlowMismatch {
    NONE,
    MAIN_STACK_DEPTH,
    MAIN_STACK_LAYOUT,
    ALT_STACK_DEPTH,
    ALT_STACK_LAYOUT,
    FIXED_STORAGE,
    SYMBOL_METADATA,
    LEXICAL_SCOPE
};

enum class ControlFlowJoinPolicy {
    RUNTIME_LAYOUT,
    IMPLICIT_EMPTY_BRANCH
};

struct ControlFlowJoinResult
{
    ControlFlowMismatch mismatch{ControlFlowMismatch::NONE};
    std::string detail;

    bool compatible() const
    {
        return mismatch == ControlFlowMismatch::NONE;
    }
};

// SymbolTable deep-copies the main and fixed stacks, while snapshots inside
// one Scope share its alternative stack. Keep an explicit altstack copy so a
// later branch or rollback cannot overwrite the captured state.
struct ControlFlowStateSnapshot
{
    SymbolTable symbolTable;
    std::vector<StackElement> altStack;
    size_t mainCombinedSize{0};
    size_t altCombinedSize{0};
    size_t lexicalDepth{0};
};

class Scope
{
public:
    explicit Scope(){};
    ~Scope() = default;

    // 基本栈操作: 转发到 OpStack
    void push(
        const std::string& nameStr,
        const std::string& typeStr,
        const std::string dataStr = ""
    );
    void push(StackElement&& element);
    void push(const StackElement& element);
    void push(const valtype& data);

    std::optional<StackElement> pop();
    StackElement& top();

    const StackElement& stacktop(int index) const;
    StackElement& stacktop(int index);

    size_t size() const;
    bool empty() const;

    const StackElement& front() const;
    const StackElement& back() const;
    const StackElement& at(uint64_t index) const;
    StackElement& at(uint64_t index);

    void erase(int index);
    void swap(int index1, int index2);

    size_t getCombinedStackSize() const;
    size_t getMaxStackSize() const;

    void pick(const int offsetFromTop);
    void roll(const int offsetFromTop);
    // 删除栈中指定位置的元素 (offsetFromTop: 0=栈顶, 1=次栈顶 即 NIP)
    void dropAt(const int offsetFromTop);

    std::optional<int64_t>
    getPos(const std::string& name, bool isAltFlag = false) const;
    std::optional<int64_t> getPos(StackElement& name, bool isAltFlag = false)
        const;
    void stackStatus(std::string& statusStr);

    void setFixed(StackElement element);
    std::optional<StackElement> getFixed(std::string& elementStr);
    void removeFixed(std::string& elementStr);

    // 移到副栈; moveOnlyMatch=true 仅返回该符号的偏移
    int32_t setAlt(std::string& symbolName, bool moveOnlyMatch = false);

    // 从副栈移回主栈; 返回搬动的副栈元素数 (symbolName 之上的会回到副栈)
    int32_t setMain(std::string& symbolName);

    SymbolTable getCurrentSymtab() const
    {
        return m_currentSymtab;
    }

    SymbolTable& getCurrentSymtab()
    {
        return m_currentSymtab;
    }

    // 用显式合并后的符号表替换当前状态，不改变外围作用域栈。
    void replaceCurrentSymtab(const SymbolTable& symbolTable)
    {
        m_currentSymtab = symbolTable;
    }

    void clean();
    void enterScope();

    // 退出当前作用域; 返回当前符号表, 调用方据此发射操作码
    SymbolTable exitScope();

    // 弹出作用域栈顶, 不恢复符号表; 非空返回 true
    bool popScopeStack();

    ControlFlowStateSnapshot captureControlFlowState() const;

    // Capture the current block result and then leave that lexical block. The
    // returned depth is the continuation depth after exitScope().
    ControlFlowStateSnapshot captureControlFlowStateAndExitScope();

    // Restore compiler-visible state without changing the lexical-scope stack.
    void restoreControlFlowState(const ControlFlowStateSnapshot& snapshot);

    ControlFlowJoinResult compareControlFlowStates(
        const ControlFlowStateSnapshot& entry,
        const ControlFlowStateSnapshot& branch,
        ControlFlowJoinPolicy policy
    ) const;

    bool defineSymbol(
        std::string name,
        std::string type = "",
        std::string modifiers = ""
    );
    bool symbolExists(std::string& name) const;

    // 数组管理 (委托给 SymbolTable)
    bool defineArray(
        const std::string& name,
        const std::string& elementType,
        size_t size,
        bool isFixedSize = true
    );
    bool importExternalArrayView(
        const std::string& name,
        const std::string& elementType,
        size_t size,
        bool isFixedSize = true
    );

    bool isArraySymbol(const std::string& name) const;
    std::optional<ArrayInfo> getArrayInfo(const std::string& name) const;
    std::string getArrayElementLabel(const std::string& arrayName, size_t index)
        const;
    std::optional<int64_t>
    getArrayElementPos(const std::string& arrayName, size_t index) const;

    // 零成本重命名
    bool rename(const std::string& oldName, const std::string& newName);
    bool renameAtPosition(int position, const std::string& newName);

    // 初始化状态
    bool isSymbolInitialized(const std::string& name) const;
    void markSymbolInitialized(const std::string& name);

    // 复合类型 (委托给 SymbolTable)
    bool defineCompoundType(const CompoundTypeInfo& compoundInfo);
    bool isCompoundTypeSymbol(const std::string& name) const;
    std::optional<CompoundTypeInfo> getCompoundTypeInfo(const std::string& name
    ) const;
    bool isCompoundTypeSplitted(const std::string& name) const;
    void markCompoundTypeSplitted(const std::string& name);

    // 身份转移 / 零成本重命名 (委托给 SymbolTable)
    bool renameSymbolEntry(
        const std::string& oldName,
        const std::string& newName
    );
    bool renameArraySymbol(
        const std::string& oldName,
        const std::string& newName
    );
    bool renameCompoundSymbol(
        const std::string& oldName,
        const std::string& newName
    );
    void renameEntriesByPrefix(
        const std::string& oldPrefix,
        const std::string& newPrefix
    );

    void addBindSymbol(std::pair<std::string, std::string>& bindPair)
    {
        m_currentSymtab.addBindSymbol(bindPair);
    }

    void symbolStatus(std::string& newSymbolStr, std::string& statusStr);

    // 为 if-else 分支创建独立副栈
    void saveSharedAltStack()
    {
        m_currentSymtab.saveSharedAltStack();
    }

    void restoreSharedAltStack()
    {
        m_currentSymtab.restoreSharedAltStack();
    }

private:
    SymbolTable m_currentSymtab{};
    std::stack<SymbolTable> m_symtabScopes;
};

SPACE_TBC_END
#endif // SCOPE_H

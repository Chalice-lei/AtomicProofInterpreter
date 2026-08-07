// symtab.h
#ifndef SYMTAB_H
#define SYMTAB_H

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <vector>

#include "../util/op_stack.h"
#include "byt_data_types.h"        // CompoundFieldInfo
#include "byt_defs.h"
#include "bytecode_helper_fun.h"   // numberToScriptHex

SPACE_TBC_START

struct ArrayElementInfo
{
    std::string baseArrayName;
    std::string elementType;
    size_t index;
    std::string qualifiedName; // 例如 "arr[0]"
    size_t stackOffset;        // 兼容非标签模式

    ArrayElementInfo() = default;
    ArrayElementInfo(
        const std::string& baseArray,
        const std::string& elemType,
        size_t idx,
        size_t offset = 0
    )
        : baseArrayName(baseArray), elementType(elemType), index(idx),
          qualifiedName(
              baseArray + "[" + numberToScriptHex(static_cast<int64_t>(idx)) +
              "]"
          ),
          stackOffset(offset)
    {}

public:
};

struct ArrayInfo
{
    std::string name;
    std::string elementType;
    size_t size;
    bool isFixedSize;
    std::vector<ArrayElementInfo> elements;

    ArrayInfo() = default;
    ArrayInfo(
        const std::string& arrayName,
        const std::string& elemType,
        size_t arraySize,
        bool fixed = true
    )
        : name(arrayName), elementType(elemType), size(arraySize),
          isFixedSize(fixed)
    {
        // 预生成元素标签
        elements.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            elements.emplace_back(arrayName, elemType, i, i);
        }
    }

    std::string getElementLabel(size_t index) const
    {
        if (index < elements.size()) {
            return elements[index].qualifiedName;
        }
        // fallback: 与访问时格式一致
        return name + "[" + numberToScriptHex(static_cast<int64_t>(index)) +
               "]";
    }

    bool isValidIndex(size_t index) const
    {
        return index < size;
    }
};

// CompoundFieldInfo 定义在 byt_data_types.h
struct CompoundTypeInfo
{
    std::string name;
    std::vector<CompoundFieldInfo> fields;
    bool isStructField;
    bool isSplitted;

    CompoundTypeInfo() = default;
    CompoundTypeInfo(
        const std::string& varName,
        const std::vector<CompoundFieldInfo>& fieldList,
        bool structField = false
    )
        : name(varName), fields(fieldList), isStructField(structField),
          isSplitted(false)
    {}
};

class SymbolInfo
{
public:
    SymbolInfo() = default;
    SymbolInfo(
        std::string name,
        std::string type = "",
        std::string modifiers = "",
        bool isStackFlag = true
    )
        : m_isStackFlag(isStackFlag), m_stackElement(name, type, modifiers),
          m_isArray(false), m_isInitialized(false), m_isCompoundType(false)
    {}

    SymbolInfo(const ArrayInfo& arrayInfo)
        : m_isStackFlag(true),
          m_stackElement(arrayInfo.name, arrayInfo.elementType + "[]", ""),
          m_isArray(true), m_arrayInfo(arrayInfo), m_isInitialized(false),
          m_isCompoundType(false)
    {}

    SymbolInfo(const CompoundTypeInfo& compoundInfo)
        : m_isStackFlag(true),
          m_stackElement(compoundInfo.name, "__compound__", ""),
          m_isArray(false), m_isInitialized(false), m_isCompoundType(true),
          m_compoundInfo(compoundInfo)
    {}

    std::string getSymbolName() const
    {
        return m_stackElement.getName();
    }
    // 比较基于 stackElement 的名字
    bool operator==(const SymbolInfo& other) const
    {
        return m_stackElement == other.m_stackElement;
    }

    bool operator!=(const SymbolInfo& other) const
    {
        return !(m_stackElement == other.m_stackElement);
    }

    bool operator<(const SymbolInfo& other) const
    {
        return m_stackElement.getName() < other.m_stackElement.getName();
    }

    bool operator>(const SymbolInfo& other) const
    {
        return m_stackElement.getName() > other.m_stackElement.getName();
    }

    bool operator<=(const SymbolInfo& other) const
    {
        return m_stackElement.getName() <= other.m_stackElement.getName();
    }

    bool operator>=(const SymbolInfo& other) const
    {
        return m_stackElement.getName() >= other.m_stackElement.getName();
    }

    bool isArray() const
    {
        return m_isArray;
    }
    const ArrayInfo& getArrayInfo() const
    {
        return m_arrayInfo;
    }
    ArrayInfo& getArrayInfo()
    {
        return m_arrayInfo;
    }

    std::string getArrayElementLabel(size_t index) const
    {
        if (m_isArray) {
            return m_arrayInfo.getElementLabel(index);
        }
        return "";
    }

    bool isInitialized() const
    {
        return m_isInitialized;
    }

    void setInitialized(bool initialized = true)
    {
        m_isInitialized = initialized;
    }

    bool isCompoundType() const
    {
        return m_isCompoundType;
    }

    const CompoundTypeInfo& getCompoundInfo() const
    {
        return m_compoundInfo;
    }

    CompoundTypeInfo& getCompoundInfo()
    {
        return m_compoundInfo;
    }

public:
    bool m_isStackFlag{true};
    StackElement m_stackElement{};
    bool m_isArray{false};
    ArrayInfo m_arrayInfo;
    bool m_isInitialized{false};
    bool m_isCompoundType{false};
    CompoundTypeInfo m_compoundInfo;
    // Compiler-only ownership marker. An external array view describes
    // values restored from the shared altstack; it is not a lexical array
    // declaration owned by the current scope.
    bool m_isExternalArrayView{false};
};

class SymbolTable
{
public:
    SymbolTable()
    {
        m_stackPtr = std::make_shared<tbc::OpStack>();
        initSharedAltStack(); // 同一 Scope 的 SymbolTable 快照共享副栈
        m_fixedStackPtr = std::make_shared<tbc::OpStack>();
    };

    SymbolTable(const SymbolTable& other)
        : m_altStackPtr(other.m_altStackPtr),
          m_newSymbol(other.m_newSymbol),
          m_declaredSymbols(other.m_declaredSymbols),
          m_keepSymbol(other.m_keepSymbol),
          m_bindSymbol(other.m_bindSymbol), m_currentScope(other.m_currentScope),
          m_activeBindSymbolStart(other.m_activeBindSymbolStart),
          m_activeScopeEntryStart(other.m_activeScopeEntryStart),
          m_savedSharedAltStackElements(other.m_savedSharedAltStackElements),
          m_savedAltStackCombinedStackSize(
              other.m_savedAltStackCombinedStackSize
          ),
          m_savedCombinedStackSize(other.m_savedCombinedStackSize)
    {
        // 主栈深拷贝
        if (other.m_stackPtr) {
            m_stackPtr = std::make_shared<tbc::OpStack>();
            int64_t size = other.m_stackPtr->size();
            int64_t index{size - 1};
            for (; index >= 0; index--) {
                m_stackPtr->push(other.m_stackPtr->stacktop(index));
            }
        }

        // 同一编译会话的作用域/分支快照共享副栈, 不跨会话共享.
        initSharedAltStack();

        if (other.m_fixedStackPtr) {
            m_fixedStackPtr = std::make_shared<tbc::OpStack>();
            int64_t size = other.m_fixedStackPtr->size();
            int64_t index{size - 1};
            for (; index >= 0; index--) {
                m_fixedStackPtr->push(other.m_fixedStackPtr->stacktop(index));
            }
        }
    }

    SymbolTable& operator=(const SymbolTable& other)
    {
        if (this == &other) {
            return *this;
        }

        m_newSymbol = other.m_newSymbol;
        m_declaredSymbols = other.m_declaredSymbols;
        m_keepSymbol = other.m_keepSymbol;
        m_bindSymbol = other.m_bindSymbol;
        m_currentScope = other.m_currentScope;
        m_activeBindSymbolStart = other.m_activeBindSymbolStart;
        m_activeScopeEntryStart = other.m_activeScopeEntryStart;
        m_altStackPtr = other.m_altStackPtr;
        m_savedSharedAltStackElements =
            other.m_savedSharedAltStackElements;
        m_savedAltStackCombinedStackSize =
            other.m_savedAltStackCombinedStackSize;
        m_savedCombinedStackSize = other.m_savedCombinedStackSize;

        // 主栈深拷贝
        if (other.m_stackPtr) {
            m_stackPtr = std::make_shared<tbc::OpStack>();
            int64_t size = other.m_stackPtr->size();
            int64_t index{size - 1};
            for (; index >= 0; index--) {
                m_stackPtr->push(other.m_stackPtr->stacktop(index));
            }
        } else {
            m_stackPtr = nullptr;
        }

        initSharedAltStack();

        if (other.m_fixedStackPtr) {
            m_fixedStackPtr = std::make_shared<tbc::OpStack>();
            int64_t size = other.m_fixedStackPtr->size();
            int64_t index{size - 1};
            for (; index >= 0; index--) {
                m_fixedStackPtr->push(other.m_fixedStackPtr->stacktop(index));
            }
        } else {
            m_fixedStackPtr = nullptr;
        }

        return *this;
    }

    ~SymbolTable() = default;

    void push(
        const std::string& nameStr,
        const std::string& typeStr,
        const std::string& dataStr
    );
    void push(
        const valtype& name,
        const valtype& type,
        const valtype& data
    );
    void push(const StackElement& element, std::string* statusStr = nullptr);

    std::optional<StackElement> pop(std::string* statusStr = nullptr);

    // 清空一次公有函数独占的 main/fixed/作用域状态。共享副栈是语言规定
    // 的跨公有函数中继通道，必须保留到同一合约的后续函数。
    void resetFunctionState();

    // 检查栈指针、内存计数和作用域声明元数据的基本一致性。
    bool validateState(std::string* error = nullptr) const;

    void pick(const int offsetFromTop);
    void roll(const int offsetFromTop);
    // 删除指定位置的元素 (offsetFromTop: 0=栈顶, 1=次栈顶 即 NIP)
    void dropAt(const int offsetFromTop);

    std::optional<int64_t>
    getPos(const std::string& name, bool isAltFlag = false) const;
    std::optional<int64_t> getPos(StackElement& name, bool isAltFlag = false)
        const;
    // Look up an already-resolved physical slot name without applying the
    // current logical parameter/channel bindings again.
    std::optional<int64_t>
    getPhysicalPos(const std::string& name, bool isAltFlag = false) const;
    void stackStatus(std::string& statusStr);

    void setFixed(StackElement varValueStr);
    std::optional<StackElement> getFixed(std::string& varStr);
    void removeFixed(std::string& varStr);

    // 移到副栈; 返回该符号的偏移
    int32_t setAlt(std::string& symbolName);
    void setAlt(int32_t moveElementNum);

    // 从副栈移回主栈; 返回搬动的副栈元素数 (symbolName 上方的会回到副栈)
    int32_t setMain(std::string& symbolName);
    void setMain(int32_t moveElementNum);

    // 定义符号; 已存在返回 false
    bool defineSymbol(
        std::string name,
        std::string type = "",
        std::string modifiers = ""
    );
    bool symbolExists(std::string& name) const;
    bool isDeclaredInCurrentScope(const std::string& name) const;
    bool hasScopeEntry(const std::string& name) const;
    bool isExternalArrayView(const std::string& name) const;

    void removeSymbol(const std::string& name);

    std::vector<SymbolInfo> getCurrentScopeSymbols() const;

    // 数组
    bool defineArray(
        const std::string& name,
        const std::string& elementType,
        size_t size,
        bool isFixedSize = true
    );

    // Import compiler-only metadata for a value restored from the shared
    // altstack. Unlike defineArray(), this does not make the view owned by
    // the current lexical scope, so normal block cleanup will not consume the
    // restored runtime slots. The enclosing private-call snapshot removes
    // the view when the inline call returns.
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

    // 解析 arr[i] 表达式, 返回基础数组名和索引
    std::pair<std::string, std::optional<size_t>> parseArrayAccess(
        const std::string& expression
    ) const;

    std::optional<int64_t>
    getArrayElementPos(const std::string& arrayName, size_t index) const;

    bool isSymbolInitialized(const std::string& name) const;
    void markSymbolInitialized(const std::string& name);

    // 复合类型
    bool defineCompoundType(const CompoundTypeInfo& compoundInfo);
    bool isCompoundTypeSymbol(const std::string& name) const;
    std::optional<CompoundTypeInfo> getCompoundTypeInfo(const std::string& name
    ) const;
    bool isCompoundTypeSplitted(const std::string& name) const;
    void markCompoundTypeSplitted(const std::string& name);

    // 零成本重命名: 同步迁移作用域条目键、元数据与栈上槽位名
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
    // 结构体变量 old.* -> new.* 批量改名
    void renameEntriesByPrefix(
        const std::string& oldPrefix,
        const std::string& newPrefix
    );

    // 两个 SymbolTable 栈状态的一致度
    enum class ConsistencyLevel {
        PERFECT,     // 主栈/副栈/固定栈完全一致
        EXCELLENT,   // 主栈/副栈完全一致
        GOOD,        // 元素相同, 排列略有差异, 可通过调整一致
        UNACCEPTABLE // 不可调整一致
    };

    void addBindSymbol(std::pair<std::string, std::string>& bindPair)
    {
        m_bindSymbol.push_back(bindPair);
    }

    // Private calls append aliases and semantic entries to shared vectors.
    // These frame boundaries keep prefix-based operations such as
    // Delete(root) inside the innermost active call.
    size_t beginBindSymbolFrame() noexcept
    {
        const size_t previous = activeBindSymbolStart();
        m_activeBindSymbolStart = m_bindSymbol.size();
        return previous;
    }

    void restoreBindSymbolFrame(size_t start) noexcept
    {
        m_activeBindSymbolStart =
            start <= m_bindSymbol.size() ? start : m_bindSymbol.size();
    }

    size_t activeBindSymbolStart() const noexcept
    {
        return m_activeBindSymbolStart <= m_bindSymbol.size()
                   ? m_activeBindSymbolStart
                   : m_bindSymbol.size();
    }

    size_t beginScopeEntryFrame() noexcept
    {
        const size_t previous = activeScopeEntryStart();
        m_activeScopeEntryStart = m_currentScope.size();
        return previous;
    }

    void restoreScopeEntryFrame(size_t start) noexcept
    {
        m_activeScopeEntryStart =
            start <= m_currentScope.size() ? start : m_currentScope.size();
    }

    size_t activeScopeEntryStart() const noexcept
    {
        return m_activeScopeEntryStart <= m_currentScope.size()
                   ? m_activeScopeEntryStart
                   : m_currentScope.size();
    }

    // Resolve a parameter binding to the caller-visible symbol. Bindings can
    // be chained by nested private calls (inner.field -> outer.field ->
    // input.field), so resolution continues until it reaches a stable name.
    std::string resolveBindSymbol(const std::string& name) const;

    // 按参数名移除绑定 (形参 -> 实参)
    void removeBindSymbol(const std::string& paramName);

    void clearBindSymbols();

    void symbolStatus(std::string& newSymbolStr, std::string& statusStr);

    ConsistencyLevel compareStackState(const SymbolTable& other) const;

    bool operator==(const SymbolTable& other) const;

    std::shared_ptr<tbc::OpStack> getSharedAltStack()
    {
        initSharedAltStack();
        return m_altStackPtr;
    }

    void clearSharedAltStack()
    {
        if (m_altStackPtr) {
            m_altStackPtr->replaceStackContent({});
        }
    }

    // 保存当前共享副栈快照
    void saveSharedAltStack()
    {
        if (!m_altStackPtr || !m_stackPtr) {
            throw std::logic_error(
                "cannot save alt stack snapshot from an invalid symbol table"
            );
        }
        m_savedSharedAltStackElements.push(m_altStackPtr->getStackContent(
        ));
        m_savedAltStackCombinedStackSize.push(
            m_altStackPtr->getCombinedStackSize()
        );
        m_savedCombinedStackSize.push(m_stackPtr->getCombinedStackSize());
    }

    void restoreSharedAltStack()
    {
        if (!m_altStackPtr || !m_stackPtr ||
            m_savedSharedAltStackElements.empty() ||
            m_savedAltStackCombinedStackSize.empty() ||
            m_savedCombinedStackSize.empty()) {
            throw std::logic_error(
                "cannot restore alt stack: no complete snapshot is available"
            );
        }
        m_altStackPtr->replaceStackContent(
            m_savedSharedAltStackElements.top()
        );
        m_savedSharedAltStackElements.pop();
        m_altStackPtr->setCombinedStackSize(
            m_savedAltStackCombinedStackSize.top()
        );
        m_savedAltStackCombinedStackSize.pop();
        m_stackPtr->setCombinedStackSize(m_savedCombinedStackSize.top());
        m_savedCombinedStackSize.pop();
    }

private:
    void initSharedAltStack()
    {
        if (!m_altStackPtr) {
            m_altStackPtr = std::make_shared<tbc::OpStack>();
        }
    }

    void validateStackOperation(
        const int offsetFromTop,
        const char* operationName
    ) const;
    StackElement getElementAtOffset(const int offsetFromTop) const;

    // 通用栈操作: shouldRemoveOriginal=true 时移动 (roll), false 为复制 (pick)
    void performStackOperation(
        const int offsetFromTop,
        bool shouldRemoveOriginal,
        const char* operationName
    );
    void symbolStatus(std::string& symbolStr)
    {
        std::ostringstream oss;
        oss << "\nnew symbol:";
        for (auto it : m_newSymbol) {
            oss << it << " ";
        }
        oss << "\nkeep symbol:";
        for (auto it : m_keepSymbol) {
            oss << it << " ";
        }
        symbolStr = oss.str();
    }

public:
    std::shared_ptr<tbc::OpStack> m_stackPtr{nullptr};
    // 副栈仅在同一 Scope/编译会话的 SymbolTable 快照之间共享。
    // 不能使用进程级 static，否则并发编译会互相清空或污染状态。
    std::shared_ptr<tbc::OpStack> m_altStackPtr{nullptr};
    std::shared_ptr<tbc::OpStack> m_fixedStackPtr{nullptr};
    // 当前作用域内新压入的栈槽（包括变量值和表达式临时值）
    std::vector<std::string> m_newSymbol;
    // 当前作用域内通过声明或隐式声明新增的语义符号
    std::vector<std::string> m_declaredSymbols;
    // keep 标记的返回符号
    std::vector<std::string> m_keepSymbol;
    // 形参 -> 实参绑定
    std::vector<std::pair<std::string, std::string>> m_bindSymbol;
    std::vector<std::pair<std::string, SymbolInfo>> m_currentScope;

private:
    size_t m_activeBindSymbolStart{0};
    size_t m_activeScopeEntryStart{0};

    // 保存/恢复用的共享副栈快照
    std::stack<std::vector<StackElement>> m_savedSharedAltStackElements;
    std::stack<size_t> m_savedAltStackCombinedStackSize;
    std::stack<size_t> m_savedCombinedStackSize;
};
SPACE_TBC_END
#endif // SYMTAB_H

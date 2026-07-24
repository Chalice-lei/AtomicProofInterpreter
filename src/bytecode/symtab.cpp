#include "symtab.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

#include "../log/logger.h"

using namespace tbc;

void SymbolTable::push(
    const std::string& nameStr,
    const std::string& typeStr,
    const std::string& dataStr
)
{
    StackElement element(nameStr, typeStr, dataStr);
    push(element);
}

void SymbolTable::push(
    const valtype& name,
    const valtype& type,
    const valtype& data
)
{
    StackElement element(name, type, data);
    push(element);
}

void SymbolTable::
    push(const StackElement& element, std::string* statusStr /* = nullptr*/)
{
    auto elementStr = element.getName();
    m_newSymbol.push_back(elementStr);
    m_stackPtr->push(element, statusStr);
}

std::optional<StackElement>
SymbolTable::pop(std::string* statusStr /* = nullptr*/)
{
    if (!m_stackPtr || m_stackPtr->empty()) {
        return std::nullopt;
    }

    auto top = m_stackPtr->top();
    {
        auto it = std::find_if(
            m_newSymbol.rbegin(),
            m_newSymbol.rend(),
            [&top](const std::string& item) { return item == top.getName(); }
        );
        if (it != m_newSymbol.rend()) {
            auto forward_it = std::next(it).base();
            m_newSymbol.erase(forward_it);
        }
    }
    m_stackPtr->pop(statusStr);
    return top;
}

void SymbolTable::resetFunctionState()
{
    m_stackPtr = std::make_shared<tbc::OpStack>();
    initSharedAltStack();
    m_fixedStackPtr = std::make_shared<tbc::OpStack>();

    m_newSymbol.clear();
    m_declaredSymbols.clear();
    m_keepSymbol.clear();
    m_bindSymbol.clear();
    m_currentScope.clear();

    m_savedSharedAltStackElements = {};
    m_savedAltStackCombinedStackSize = {};
    m_savedCombinedStackSize = {};
}

bool SymbolTable::validateState(std::string* error) const
{
    auto fail = [error](const std::string& message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    if (!m_stackPtr) {
        return fail("main stack pointer is null");
    }
    if (!m_altStackPtr) {
        return fail("alternative stack pointer is null");
    }
    if (!m_fixedStackPtr) {
        return fail("fixed stack pointer is null");
    }

    auto validateAccounting = [&fail](
                                  const tbc::OpStack& stack,
                                  const char* stackName
                              ) {
        size_t expectedSize = 0;
        for (const auto& element : stack.getStackContent()) {
            expectedSize += element.getMemoryUsage();
        }
        if (stack.getCombinedStackSize() != expectedSize) {
            std::ostringstream oss;
            oss << stackName << " stack accounting mismatch: expected "
                << expectedSize << ", got " << stack.getCombinedStackSize();
            return fail(oss.str());
        }
        return true;
    };

    if (!validateAccounting(*m_stackPtr, "main") ||
        !validateAccounting(*m_altStackPtr, "alternative") ||
        !validateAccounting(*m_fixedStackPtr, "fixed")) {
        return false;
    }

    for (const auto& declared : m_declaredSymbols) {
        auto it = std::find_if(
            m_currentScope.begin(),
            m_currentScope.end(),
            [&declared](const std::pair<std::string, SymbolInfo>& symbol) {
                return symbol.first == declared;
            }
        );
        if (it == m_currentScope.end()) {
            return fail(
                "declared symbol is missing from the current scope: " + declared
            );
        }
    }

    if (m_savedSharedAltStackElements.size() !=
            m_savedAltStackCombinedStackSize.size() ||
        m_savedSharedAltStackElements.size() !=
            m_savedCombinedStackSize.size()) {
        return fail("incomplete alternative-stack snapshot metadata");
    }

    if (error) {
        error->clear();
    }
    return true;
}

void SymbolTable::pick(const int offsetFromTop)
{
    performStackOperation(offsetFromTop, false, "PICK");
}

void SymbolTable::roll(const int offsetFromTop)
{
    performStackOperation(offsetFromTop, true, "ROLL");
}

void SymbolTable::dropAt(const int offsetFromTop)
{
    // NIP 语义: 直接删除该位置, 不上栈顶
    validateStackOperation(offsetFromTop, "dropAt");
    m_stackPtr->erase(offsetFromTop);
}

std::optional<int64_t>
SymbolTable::getPos(const std::string& name, bool isAltFlag /*= false*/) const
{
    StackElement element(name);
    return getPos(element, isAltFlag);
}

std::optional<int64_t>
SymbolTable::getPos(StackElement& element, bool isAltFlag /*= false*/) const
{
    for (auto it : m_bindSymbol) {
        if (it.first == element.getName()) {
            element.setName(it.second);
        }
    }
    auto stackPtr = isAltFlag ? m_altStackPtr : m_stackPtr;
    if (!stackPtr) {
        std::ostringstream oss;
        oss << "SymbolTable::getPos() error: "
            << (isAltFlag ? "Alternative stack" : "Main stack")
            << " pointer is null. Cannot find position for element: "
            << element.getName();
        throw std::runtime_error(oss.str());
    }
    // at 使用栈顶为 0 的相对索引
    for (size_t i = 0; i < stackPtr->size(); ++i) {
        if (stackPtr->at(i) == element) {
            return static_cast<int64_t>(i);
        }
    }

    return std::nullopt;
}

void SymbolTable::removeBindSymbol(const std::string& paramName)
{
    m_bindSymbol.erase(
        std::remove_if(
            m_bindSymbol.begin(),
            m_bindSymbol.end(),
            [&paramName](const std::pair<std::string, std::string>& pair) {
                return pair.first == paramName;
            }
        ),
        m_bindSymbol.end()
    );
}

void SymbolTable::clearBindSymbols()
{
    m_bindSymbol.clear();
}

void SymbolTable::stackStatus(std::string& statusStr)
{
    statusStr += "\nstack:";
    m_stackPtr->stackStatus(statusStr);
    statusStr += "\naltStack:";
    m_altStackPtr->stackStatus(statusStr);
    statusStr += "\nfixedStack:";
    m_fixedStackPtr->stackStatus(statusStr);
}

void SymbolTable::setFixed(StackElement stackElement)
{
    auto stackPtr = m_fixedStackPtr;
    if (!stackPtr) {
        throw std::logic_error(
            "cannot store a fixed value: fixed stack pointer is null"
        );
    }
    int index = -1;
    for (size_t i = 0; i < stackPtr->size(); ++i) {
        if (stackPtr->at(i) == stackElement.getName()) {
            index = i;
        }
    }
    if (-1 != index) {
        stackPtr->erase(index);
    }
    stackPtr->push(stackElement);
}

std::optional<StackElement> SymbolTable::getFixed(std::string& elementStr)
{
    auto stackPtr = m_fixedStackPtr;
    if (!stackPtr) {
        return std::nullopt;
    }
    for (size_t i = 0; i < stackPtr->size(); ++i) {
        if (stackPtr->at(i).getName() == elementStr) {
            return stackPtr->at(i);
        }
    }

    return std::nullopt;
}

void SymbolTable::removeFixed(std::string& elementStr)
{
    auto stackPtr = m_fixedStackPtr;
    if (!stackPtr) {
        return;
    }
    for (size_t i = 0; i < stackPtr->size(); ++i) {
        if (stackPtr->at(i) == elementStr) {
            stackPtr->erase(i);
            return;
        }
    }
}

int32_t SymbolTable::setAlt(std::string& symbolName)
{
    int32_t num = 0;
    while (true) {
        if (m_stackPtr->empty()) {
            return num;
        }
        auto stackElementTop = m_stackPtr->top();
        m_stackPtr->pop();
        m_altStackPtr->push(stackElementTop);
        if (stackElementTop == symbolName) {
            return num;
        }
        num++;
    }
}

void SymbolTable::setAlt(int32_t moveElementNum)
{
    int32_t i = 0;
    for (; i < moveElementNum; i++) {
        if (m_stackPtr->empty()) {
            return;
        }
        auto stackElementTop = m_stackPtr->top();
        m_stackPtr->pop();
        m_altStackPtr->push(stackElementTop);
    }
}

int32_t SymbolTable::setMain(std::string& symbolName)
{
    int32_t num = 0;
    while (true) {
        if (m_altStackPtr->empty()) {
            return num;
        }
        auto altStackElementTop = m_altStackPtr->top();
        m_altStackPtr->pop();
        m_stackPtr->push(altStackElementTop);
        if (altStackElementTop == symbolName) {
            return num;
        }
        num++;
    }
}

void SymbolTable::setMain(int32_t moveElementNum)
{
    int32_t i = 0;
    for (; i < moveElementNum; i++) {
        if (m_altStackPtr->empty()) {
            return;
        }
        auto stackElementTop = m_altStackPtr->top();
        m_altStackPtr->pop();
        m_stackPtr->push(stackElementTop);
    }
}

bool SymbolTable::defineSymbol(
    std::string name,
    std::string type /* = ""*/,
    std::string modifiers /* = ""*/
)
{
    if (symbolExists(name)) {
        return false; // 不允许重名
    }

    m_currentScope.emplace_back(name, SymbolInfo{name, type, modifiers});
    m_declaredSymbols.push_back(name);
    return true;
}

bool SymbolTable::symbolExists(std::string& name) const
{
    auto it = std::find_if(
        m_currentScope.rbegin(),
        m_currentScope.rend(),
        [&name](const std::pair<std::string, SymbolInfo>& pair) {
            return pair.first == name;
        }
    );
    return it != m_currentScope.rend();
}

bool SymbolTable::isDeclaredInCurrentScope(const std::string& name) const
{
    return std::find(m_declaredSymbols.begin(), m_declaredSymbols.end(), name) !=
           m_declaredSymbols.end();
}

void SymbolTable::removeSymbol(const std::string& name)
{
    auto scopeIt = std::find_if(
        m_currentScope.begin(),
        m_currentScope.end(),
        [&name](const std::pair<std::string, SymbolInfo>& pair) {
            return pair.first == name;
        }
    );
    if (scopeIt != m_currentScope.end()) {
        m_currentScope.erase(scopeIt);
    }
    m_declaredSymbols.erase(
        std::remove(m_declaredSymbols.begin(), m_declaredSymbols.end(), name),
        m_declaredSymbols.end()
    );
}

std::vector<SymbolInfo> SymbolTable::getCurrentScopeSymbols() const
{
    std::vector<SymbolInfo> symbols;
    for (const auto& pair : m_currentScope) {
        symbols.push_back(pair.second);
    }
    return symbols;
}

void SymbolTable::validateStackOperation(
    const int offsetFromTop,
    const char* operationName
) const
{
    if (m_stackPtr->size() < 1) {
        throw std::invalid_argument(
            std::string("Stack operation error (") + operationName +
            "): insufficient stack elements"
        );
    }

    // offsetFromTop ∈ [0, size-1]
    if (offsetFromTop < 0 ||
        offsetFromTop >= static_cast<int>(m_stackPtr->size())) {
        throw std::invalid_argument(
            std::string("Stack operation error (") + operationName +
            "): invalid offset"
        );
    }
}

StackElement SymbolTable::getElementAtOffset(const int offsetFromTop) const
{
    return m_stackPtr->at(static_cast<uint64_t>(offsetFromTop));
}

void SymbolTable::performStackOperation(
    const int offsetFromTop,
    bool shouldRemoveOriginal,
    const char* operationName
)
{
    validateStackOperation(offsetFromTop, operationName);

    // ROLL 栈顶 = no-op
    if (shouldRemoveOriginal && offsetFromTop == 0) {
        return;
    }

    StackElement element = getElementAtOffset(offsetFromTop);

    if (shouldRemoveOriginal) {
        m_stackPtr->erase(offsetFromTop);
    }

    m_stackPtr->push(element);
    // 也可复用为 DUP(offset=0,copy) / OVER(offset=1,copy) 等
}

bool SymbolTable::defineArray(
    const std::string& name,
    const std::string& elementType,
    size_t size,
    bool isFixedSize
)
{
    if (symbolExists(const_cast<std::string&>(name))) {
        return false;
    }

    ArrayInfo arrayInfo(name, elementType, size, isFixedSize);
    SymbolInfo arraySymbol(arrayInfo);
    m_currentScope.emplace_back(name, arraySymbol);
    m_declaredSymbols.push_back(name);

    return true;
}

bool SymbolTable::isArraySymbol(const std::string& name) const
{
    for (const auto& entry : m_currentScope) {
        if (entry.first == name) {
            return entry.second.isArray();
        }
    }
    return false;
}

std::optional<ArrayInfo> SymbolTable::getArrayInfo(const std::string& name
) const
{
    for (const auto& entry : m_currentScope) {
        if (entry.first == name && entry.second.isArray()) {
            return entry.second.getArrayInfo();
        }
    }
    return std::nullopt;
}

std::string SymbolTable::getArrayElementLabel(
    const std::string& arrayName,
    size_t index
) const
{
    auto arrayInfoOpt = getArrayInfo(arrayName);
    if (arrayInfoOpt.has_value()) {
        return arrayInfoOpt.value().getElementLabel(index);
    }
    return arrayName + "[" + numberToScriptHex(static_cast<int64_t>(index)) +
           "]";
}

std::pair<std::string, std::optional<size_t>> SymbolTable::parseArrayAccess(
    const std::string& expression
) const
{
    // 解析 "arrayName[index]" 形式的表达式
    size_t bracketPos = expression.find('[');
    if (bracketPos == std::string::npos) {
        return {expression, std::nullopt};
    }

    size_t closeBracketPos = expression.find(']', bracketPos);
    if (closeBracketPos == std::string::npos) {
        return {expression, std::nullopt};
    }

    std::string arrayName = expression.substr(0, bracketPos);
    std::string indexStr =
        expression.substr(bracketPos + 1, closeBracketPos - bracketPos - 1);

    try {
        size_t index = std::stoull(indexStr);
        return {arrayName, index};
    } catch (const std::exception&) {
        // 索引不是数字字面量 (可能是变量)
        return {arrayName, std::nullopt};
    }
}

std::optional<int64_t> SymbolTable::getArrayElementPos(
    const std::string& arrayName,
    size_t index
) const
{
    std::string elementLabel = getArrayElementLabel(arrayName, index);
    return getPos(elementLabel);
}

bool SymbolTable::isSymbolInitialized(const std::string& name) const
{
    auto it = std::find_if(
        m_currentScope.rbegin(),
        m_currentScope.rend(),
        [&name](const std::pair<std::string, SymbolInfo>& pair) {
            return pair.first == name;
        }
    );

    if (it != m_currentScope.rend()) {
        return it->second.isInitialized();
    }

    return false; // 不存在视为未初始化
}

void SymbolTable::markSymbolInitialized(const std::string& name)
{
    auto it = std::find_if(
        m_currentScope.rbegin(),
        m_currentScope.rend(),
        [&name](std::pair<std::string, SymbolInfo>& pair) {
            return pair.first == name;
        }
    );

    if (it != m_currentScope.rend()) {
        it->second.setInitialized(true);
    }
}

void SymbolTable::symbolStatus(
    std::string& newSymbolStr,
    std::string& statusStr
)
{
    symbolStatus(newSymbolStr);
    stackStatus(statusStr);
}

SymbolTable::ConsistencyLevel SymbolTable::compareStackState(
    const SymbolTable& other
) const
{
    LOG_INFO("Start stack state comparison analysis");

    std::string currentStackStatus;
    const_cast<SymbolTable*>(this)->stackStatus(currentStackStatus);
    LOG_INFO("Current stack status:", currentStackStatus);

    std::string otherStackStatus;
    const_cast<SymbolTable&>(other).stackStatus(otherStackStatus);
    LOG_INFO("Comparing stack states:", otherStackStatus);

    // 主栈比较
    bool mainStackMatch = false;
    bool mainStackSizeMatch = false;
    bool mainStackElementsMatch = false;
    size_t mainStackMatchCount = 0;

    if (m_stackPtr && other.m_stackPtr) {
        size_t thisSize = m_stackPtr->size();
        size_t otherSize = other.m_stackPtr->size();

        LOG_INFO(
            "Main stack size comparison: Current =",
            thisSize,
            ", contrast=",
            otherSize
        );

        mainStackSizeMatch = (thisSize == otherSize);

        if (mainStackSizeMatch) {
            mainStackElementsMatch = true;
            for (size_t i = 0; i < thisSize; ++i) {
                const auto& thisElement = m_stackPtr->at(i);
                const auto& otherElement = other.m_stackPtr->at(i);

                if (thisElement == otherElement) {
                    mainStackMatchCount++;
                    LOG_DEBUG(
                        "Main stack location",
                        i,
                        "match: ",
                        thisElement.getName()
                    );
                } else {
                    mainStackElementsMatch = false;
                    LOG_DEBUG(
                        "Main stack position",
                        i,
                        "mismatch: current=",
                        thisElement.getName(),
                        ", compare=",
                        otherElement.getName()
                    );
                }
            }

            if (mainStackElementsMatch) {
                mainStackMatch = true;
                LOG_INFO("Main stack completely matched");
            } else {
                LOG_INFO(
                    "Main stack element match count: ",
                    mainStackMatchCount,
                    "/",
                    thisSize
                );
            }
        }
    } else if (!m_stackPtr && !other.m_stackPtr) {
        mainStackMatch = true;
        LOG_INFO("Both main stacks are empty, considered as matched");
    } else {
        LOG_WARNING(
            "Main stack pointer state inconsistent: current=",
            (m_stackPtr ? "valid" : "null"),
            ", compare=",
            (other.m_stackPtr ? "valid" : "null")
        );
    }

    // 副栈比较
    bool altStackMatch = false;
    bool altStackSizeMatch = false;
    bool altStackElementsMatch = false;
    size_t altStackMatchCount = 0;

    if (m_altStackPtr && other.m_altStackPtr) {
        size_t thisSize = m_altStackPtr->size();
        size_t otherSize = other.m_altStackPtr->size();

        LOG_INFO(
            "Alt stack size comparison: current=",
            thisSize,
            ", compare=",
            otherSize
        );

        altStackSizeMatch = (thisSize == otherSize);

        if (altStackSizeMatch) {
            altStackElementsMatch = true;
            for (size_t i = 0; i < thisSize; ++i) {
                const auto& thisElement = m_altStackPtr->at(i);
                const auto& otherElement = other.m_altStackPtr->at(i);

                if (thisElement == otherElement) {
                    altStackMatchCount++;
                    LOG_DEBUG(
                        "Alt stack position",
                        i,
                        "match: ",
                        thisElement.getName()
                    );
                } else {
                    altStackElementsMatch = false;
                    LOG_DEBUG(
                        "Alt stack position",
                        i,
                        "mismatch: current=",
                        thisElement.getName(),
                        ", compare=",
                        otherElement.getName()
                    );
                }
            }

            if (altStackElementsMatch) {
                altStackMatch = true;
                LOG_INFO("Alt stack completely matched");
            } else {
                LOG_INFO(
                    "Alt stack element match count: ",
                    altStackMatchCount,
                    "/",
                    thisSize
                );
            }
        }
    } else if (!m_altStackPtr && !other.m_altStackPtr) {
        altStackMatch = true;
        LOG_INFO("Both alt stacks are empty, considered as matched");
    } else {
        LOG_WARNING(
            "Alt stack pointer state inconsistent: current=",
            (m_altStackPtr ? "valid" : "null"),
            ", compare=",
            (other.m_altStackPtr ? "valid" : "null")
        );
    }

    // 固定栈比较
    bool fixedStackMatch = false;
    bool fixedStackSizeMatch = false;
    bool fixedStackElementsMatch = false;
    size_t fixedStackMatchCount = 0;

    if (m_fixedStackPtr && other.m_fixedStackPtr) {
        size_t thisSize = m_fixedStackPtr->size();
        size_t otherSize = other.m_fixedStackPtr->size();

        LOG_INFO(
            "Fixed stack size comparison: current=",
            thisSize,
            ", compare=",
            otherSize
        );

        fixedStackSizeMatch = (thisSize == otherSize);

        if (fixedStackSizeMatch) {
            fixedStackElementsMatch = true;
            for (size_t i = 0; i < thisSize; ++i) {
                const auto& thisElement = m_fixedStackPtr->at(i);
                const auto& otherElement = other.m_fixedStackPtr->at(i);

                if (thisElement == otherElement) {
                    fixedStackMatchCount++;
                    LOG_DEBUG(
                        "Fixed stack position",
                        i,
                        "match: ",
                        thisElement.getName()
                    );
                } else {
                    fixedStackElementsMatch = false;
                    LOG_DEBUG(
                        "Fixed stack position",
                        i,
                        "mismatch: current=",
                        thisElement.getName(),
                        ", compare=",
                        otherElement.getName()
                    );
                }
            }

            if (fixedStackElementsMatch) {
                fixedStackMatch = true;
                LOG_INFO("Fixed stack completely matched");
            } else {
                LOG_INFO(
                    "Fixed stack element match count: ",
                    fixedStackMatchCount,
                    "/",
                    thisSize
                );
            }
        }
    } else if (!m_fixedStackPtr && !other.m_fixedStackPtr) {
        fixedStackMatch = true;
        LOG_INFO("Both fixed stacks are empty, considered as matched");
    } else {
        LOG_WARNING(
            "Fixed stack pointer state inconsistent: current=",
            (m_fixedStackPtr ? "valid" : "null"),
            ", compare=",
            (other.m_fixedStackPtr ? "valid" : "null")
        );
    }

    ConsistencyLevel result;

    if (mainStackMatch && altStackMatch && fixedStackMatch) {
        result = ConsistencyLevel::PERFECT;
        LOG_INFO(
            "Stack state comparison result: PERFECT - All stacks completely "
            "consistent"
        );
    } else if (mainStackMatch && altStackMatch) {
        result = ConsistencyLevel::EXCELLENT;
        LOG_INFO(
            "Stack state comparison result: EXCELLENT - Main stack and alt "
            "stack completely consistent"
        );
    } else if (mainStackSizeMatch && altStackSizeMatch &&
               mainStackElementsMatch && altStackElementsMatch) {
        result = ConsistencyLevel::EXCELLENT;
        LOG_INFO(
            "Stack state comparison result: EXCELLENT - Main stack and alt "
            "stack length and elements completely consistent"
        );
    } else if (mainStackSizeMatch && altStackSizeMatch) {
        // 估算是否可通过栈调整对齐
        size_t totalMainElements = m_stackPtr ? m_stackPtr->size() : 0;
        size_t totalAltElements = m_altStackPtr ? m_altStackPtr->size() : 0;

        double mainMatchRatio = totalMainElements > 0
                                    ? static_cast<double>(mainStackMatchCount) /
                                          totalMainElements
                                    : 1.0;
        double altMatchRatio = totalAltElements > 0
                                   ? static_cast<double>(altStackMatchCount) /
                                         totalAltElements
                                   : 1.0;

        LOG_INFO("Main stack match ratio: ", mainMatchRatio * 100, "%");
        LOG_INFO("Alt stack match ratio: ", altMatchRatio * 100, "%");

        // 70% 以上匹配视为可调整对齐
        if (mainMatchRatio >= 0.7 && altMatchRatio >= 0.7) {
            result = ConsistencyLevel::GOOD;
            LOG_INFO(
                "Stack state comparison result: GOOD - Most elements "
                "consistent, can be made consistent through stack adjustment"
            );
        } else {
            result = ConsistencyLevel::UNACCEPTABLE;
            LOG_WARNING(
                "Stack state comparison result: UNACCEPTABLE - Element "
                "differences too large, cannot be made consistent through "
                "adjustment"
            );
        }
    } else {
        result = ConsistencyLevel::UNACCEPTABLE;
        LOG_WARNING(
            "Stack state comparison result: UNACCEPTABLE - Stack size or "
            "structure inconsistent"
        );
    }

    return result;
}

bool SymbolTable::operator==(const SymbolTable& other) const
{
    ConsistencyLevel level = compareStackState(other);

    // PERFECT / EXCELLENT 视为相等
    bool isEqual =
        (level == ConsistencyLevel::PERFECT ||
         level == ConsistencyLevel::EXCELLENT);

    LOG_INFO(
        "SymbolTable comparison result: ", isEqual ? "equal" : "not equal"
    );

    return isEqual;
}

bool SymbolTable::defineCompoundType(const CompoundTypeInfo& compoundInfo)
{
    std::string name = compoundInfo.name;
    if (symbolExists(name)) {
        LOG_WARNING("Compound type symbol already exists: ", name);
        return false;
    }

    SymbolInfo symbolInfo(compoundInfo);
    m_currentScope.emplace_back(name, symbolInfo);
    m_declaredSymbols.push_back(name);

    LOG_DEBUG(
        "Defined compound type: ",
        name,
        " with ",
        compoundInfo.fields.size(),
        " fields"
    );

    return true;
}

bool SymbolTable::isCompoundTypeSymbol(const std::string& name) const
{
    auto it = std::find_if(
        m_currentScope.rbegin(),
        m_currentScope.rend(),
        [&name](const std::pair<std::string, SymbolInfo>& pair) {
            return pair.first == name;
        }
    );

    if (it != m_currentScope.rend()) {
        return it->second.isCompoundType();
    }

    return false;
}

std::optional<CompoundTypeInfo> SymbolTable::getCompoundTypeInfo(
    const std::string& name
) const
{
    auto it = std::find_if(
        m_currentScope.rbegin(),
        m_currentScope.rend(),
        [&name](const std::pair<std::string, SymbolInfo>& pair) {
            return pair.first == name;
        }
    );

    if (it != m_currentScope.rend() && it->second.isCompoundType()) {
        return it->second.getCompoundInfo();
    }

    return std::nullopt;
}

bool SymbolTable::isCompoundTypeSplitted(const std::string& name) const
{
    auto it = std::find_if(
        m_currentScope.rbegin(),
        m_currentScope.rend(),
        [&name](const std::pair<std::string, SymbolInfo>& pair) {
            return pair.first == name;
        }
    );

    if (it != m_currentScope.rend() && it->second.isCompoundType()) {
        return it->second.getCompoundInfo().isSplitted;
    }

    return false;
}

void SymbolTable::markCompoundTypeSplitted(const std::string& name)
{
    auto it = std::find_if(
        m_currentScope.rbegin(),
        m_currentScope.rend(),
        [&name](std::pair<std::string, SymbolInfo>& pair) {
            return pair.first == name;
        }
    );

    if (it != m_currentScope.rend() && it->second.isCompoundType()) {
        it->second.getCompoundInfo().isSplitted = true;
        LOG_DEBUG("Marked compound type as splitted: ", name);
    }
}

// 在列表中将 oldName 改为 newName, 未命中则追加 newName
static void renameOrAppendInList(
    std::vector<std::string>& list,
    const std::string& oldName,
    const std::string& newName
)
{
    bool renamed = false;
    for (auto& entry : list) {
        if (entry == oldName) {
            entry = newName;
            renamed = true;
        }
    }
    if (!renamed) {
        list.push_back(newName);
    }
}

// 声明归属只在原符号确属当前作用域时迁移，不能因栈槽重命名凭空新增。
static void renameExistingInList(
    std::vector<std::string>& list,
    const std::string& oldName,
    const std::string& newName
)
{
    if (oldName == newName) {
        return;
    }

    const bool alreadyHasNew =
        std::find(list.begin(), list.end(), newName) != list.end();
    for (auto it = list.begin(); it != list.end();) {
        if (*it != oldName) {
            ++it;
            continue;
        }
        if (alreadyHasNew) {
            it = list.erase(it);
        } else {
            *it = newName;
            ++it;
        }
    }
}

// 身份转移前清除作用域中目标键的"骨架"条目, 避免重命名后键重复
// (如 Struct_T a 声明时预创建的空 a.fi 条目)
static void eraseScopeEntriesByKey(
    std::vector<std::pair<std::string, SymbolInfo>>& scope,
    const std::string& key
)
{
    scope.erase(
        std::remove_if(
            scope.begin(),
            scope.end(),
            [&key](const std::pair<std::string, SymbolInfo>& p) {
                return p.first == key;
            }
        ),
        scope.end()
    );
}

static void eraseScopeEntriesByPrefix(
    std::vector<std::pair<std::string, SymbolInfo>>& scope,
    const std::string& prefix
)
{
    scope.erase(
        std::remove_if(
            scope.begin(),
            scope.end(),
            [&prefix](const std::pair<std::string, SymbolInfo>& p) {
                const std::string& k = p.first;
                return k.size() >= prefix.size() &&
                       k.compare(0, prefix.size(), prefix) == 0;
            }
        ),
        scope.end()
    );
}

bool SymbolTable::renameSymbolEntry(
    const std::string& oldName,
    const std::string& newName
)
{
    // 先清掉 newName 的骨架条目, 避免键重复
    eraseScopeEntriesByKey(m_currentScope, newName);

    auto it = std::find_if(
        m_currentScope.begin(),
        m_currentScope.end(),
        [&oldName](const std::pair<std::string, SymbolInfo>& p) {
            return p.first == oldName;
        }
    );
    if (it == m_currentScope.end()) {
        return false;
    }
    it->first = newName;
    it->second.m_stackElement.setName(newName);
    renameOrAppendInList(m_newSymbol, oldName, newName);
    renameExistingInList(m_declaredSymbols, oldName, newName);
    if (m_stackPtr) {
        m_stackPtr->rename(oldName, newName);
    }
    return true;
}

bool SymbolTable::renameArraySymbol(
    const std::string& oldName,
    const std::string& newName
)
{
    eraseScopeEntriesByKey(m_currentScope, newName);

    auto it = std::find_if(
        m_currentScope.begin(),
        m_currentScope.end(),
        [&oldName](const std::pair<std::string, SymbolInfo>& p) {
            return p.first == oldName;
        }
    );
    if (it == m_currentScope.end() || !it->second.isArray()) {
        return false;
    }

    ArrayInfo& arrInfo = it->second.getArrayInfo();
    const size_t arraySize = arrInfo.size;

    // 1) 栈上 oldName[idx] -> newName[idx]
    if (m_stackPtr) {
        for (auto& elem : arrInfo.elements) {
            const std::string oldQual = elem.qualifiedName;
            const std::string newQual =
                newName + "[" +
                numberToScriptHex(static_cast<int64_t>(elem.index)) + "]";
            m_stackPtr->rename(oldQual, newQual);
        }
    }

    // 2) 重建 ArrayInfo
    arrInfo.name = newName;
    arrInfo.elements.clear();
    arrInfo.elements.reserve(arraySize);
    for (size_t i = 0; i < arraySize; ++i) {
        arrInfo.elements.emplace_back(newName, arrInfo.elementType, i, i);
    }

    // 3) 作用域键与 m_stackElement
    it->first = newName;
    it->second.m_stackElement.setName(newName);
    renameOrAppendInList(m_newSymbol, oldName, newName);
    renameExistingInList(m_declaredSymbols, oldName, newName);
    return true;
}

bool SymbolTable::renameCompoundSymbol(
    const std::string& oldName,
    const std::string& newName
)
{
    eraseScopeEntriesByKey(m_currentScope, newName);

    auto it = std::find_if(
        m_currentScope.begin(),
        m_currentScope.end(),
        [&oldName](const std::pair<std::string, SymbolInfo>& p) {
            return p.first == oldName;
        }
    );
    if (it == m_currentScope.end() || !it->second.isCompoundType()) {
        return false;
    }

    it->second.getCompoundInfo().name = newName;
    it->first = newName;
    it->second.m_stackElement.setName(newName);
    renameOrAppendInList(m_newSymbol, oldName, newName);
    renameExistingInList(m_declaredSymbols, oldName, newName);

    // 同步栈上的占位槽
    if (m_stackPtr) {
        m_stackPtr->rename(oldName, newName);
    }
    return true;
}

void SymbolTable::renameEntriesByPrefix(
    const std::string& oldPrefix,
    const std::string& newPrefix
)
{
    if (oldPrefix == newPrefix || oldPrefix.empty()) {
        return;
    }

    // 1) 栈上所有以 oldPrefix 开头的槽位改名
    if (m_stackPtr) {
        const size_t n = m_stackPtr->size();
        for (size_t i = 0; i < n; ++i) {
            auto& elem = m_stackPtr->at(i);
            const std::string& cur = elem.getName();
            if (cur.size() >= oldPrefix.size() &&
                cur.compare(0, oldPrefix.size(), oldPrefix) == 0) {
                elem.setName(newPrefix + cur.substr(oldPrefix.size()));
            }
        }
    }

    // 2) 作用域条目: 先清除 newPrefix 骨架, 再改键
    eraseScopeEntriesByPrefix(m_currentScope, newPrefix);
    for (auto& entry : m_currentScope) {
        const std::string& key = entry.first;
        if (key.size() >= oldPrefix.size() &&
            key.compare(0, oldPrefix.size(), oldPrefix) == 0) {
            std::string newKey = newPrefix + key.substr(oldPrefix.size());
            entry.first = newKey;
            entry.second.m_stackElement.setName(newKey);

            if (entry.second.isArray()) {
                ArrayInfo& ai = entry.second.getArrayInfo();
                ai.name = newKey;
                const size_t sz = ai.size;
                ai.elements.clear();
                ai.elements.reserve(sz);
                for (size_t i = 0; i < sz; ++i) {
                    ai.elements.emplace_back(newKey, ai.elementType, i, i);
                }
            } else if (entry.second.isCompoundType()) {
                entry.second.getCompoundInfo().name = newKey;
            }
        }
    }

    // 3) m_newSymbol 中的前缀匹配项同步
    for (auto& s : m_newSymbol) {
        if (s.size() >= oldPrefix.size() &&
            s.compare(0, oldPrefix.size(), oldPrefix) == 0) {
            s = newPrefix + s.substr(oldPrefix.size());
        }
    }

    // 4) 当前作用域声明归属同步迁移。
    for (auto& s : m_declaredSymbols) {
        if (s.size() >= oldPrefix.size() &&
            s.compare(0, oldPrefix.size(), oldPrefix) == 0) {
            s = newPrefix + s.substr(oldPrefix.size());
        }
    }
}

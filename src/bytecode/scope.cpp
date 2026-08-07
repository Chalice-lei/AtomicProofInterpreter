#include "scope.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "../log/logger.h"

using namespace tbc;

namespace
{

bool sameRuntimeSlot(
    const StackElement& lhs,
    const StackElement& rhs,
    bool compareType
)
{
    return lhs.getName() == rhs.getName() &&
           (!compareType || lhs.getType() == rhs.getType());
}

bool sameFixedBinding(const StackElement& lhs, const StackElement& rhs)
{
    return sameRuntimeSlot(lhs, rhs, true) && lhs.getData() == rhs.getData();
}

std::string describeStack(const std::vector<StackElement>& stack)
{
    std::ostringstream oss;
    oss << '[';
    for (size_t i = 0; i < stack.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << stack[i].getName();
        if (!stack[i].getType().empty()) {
            oss << ':' << stack[i].getType();
        }
    }
    oss << ']';
    return oss.str();
}

ControlFlowJoinResult compareRuntimeStack(
    const std::vector<StackElement>& entry,
    const std::vector<StackElement>& branch,
    ControlFlowMismatch depthMismatch,
    ControlFlowMismatch layoutMismatch,
    const char* stackName,
    bool compareType
)
{
    if (entry.size() != branch.size()) {
        std::ostringstream oss;
        oss << stackName << " depth differs: entry=" << entry.size()
            << ", branch=" << branch.size();
        return {depthMismatch, oss.str()};
    }

    for (size_t i = 0; i < entry.size(); ++i) {
        if (!sameRuntimeSlot(entry[i], branch[i], compareType)) {
            std::ostringstream oss;
            oss << stackName << " layout differs: entry="
                << describeStack(entry) << ", branch="
                << describeStack(branch);
            return {layoutMismatch, oss.str()};
        }
    }

    return {};
}

bool sameArrayInfo(const ArrayInfo& lhs, const ArrayInfo& rhs)
{
    if (lhs.name != rhs.name || lhs.elementType != rhs.elementType ||
        lhs.size != rhs.size || lhs.isFixedSize != rhs.isFixedSize ||
        lhs.elements.size() != rhs.elements.size()) {
        return false;
    }

    for (size_t i = 0; i < lhs.elements.size(); ++i) {
        const auto& lhsElement = lhs.elements[i];
        const auto& rhsElement = rhs.elements[i];
        if (lhsElement.baseArrayName != rhsElement.baseArrayName ||
            lhsElement.elementType != rhsElement.elementType ||
            lhsElement.index != rhsElement.index ||
            lhsElement.qualifiedName != rhsElement.qualifiedName ||
            lhsElement.stackOffset != rhsElement.stackOffset) {
            return false;
        }
    }
    return true;
}

bool sameCompoundInfo(
    const CompoundTypeInfo& lhs,
    const CompoundTypeInfo& rhs
)
{
    if (lhs.name != rhs.name || lhs.isStructField != rhs.isStructField ||
        lhs.isSplitted != rhs.isSplitted ||
        lhs.fields.size() != rhs.fields.size()) {
        return false;
    }

    for (size_t i = 0; i < lhs.fields.size(); ++i) {
        const auto& lhsField = lhs.fields[i];
        const auto& rhsField = rhs.fields[i];
        if (lhsField.name != rhsField.name || lhsField.type != rhsField.type ||
            lhsField.byteSize != rhsField.byteSize ||
            lhsField.isArray != rhsField.isArray ||
            lhsField.arraySize != rhsField.arraySize) {
            return false;
        }
    }
    return true;
}

bool sameSymbolMetadata(const SymbolInfo& lhs, const SymbolInfo& rhs)
{
    if (lhs.m_isStackFlag != rhs.m_isStackFlag ||
        lhs.m_stackElement.getName() != rhs.m_stackElement.getName() ||
        lhs.m_stackElement.getType() != rhs.m_stackElement.getType() ||
        lhs.m_stackElement.getData() != rhs.m_stackElement.getData() ||
        lhs.m_isArray != rhs.m_isArray ||
        lhs.m_isInitialized != rhs.m_isInitialized ||
        lhs.m_isCompoundType != rhs.m_isCompoundType ||
        lhs.m_isExternalArrayView != rhs.m_isExternalArrayView) {
        return false;
    }

    if (lhs.m_isArray && !sameArrayInfo(lhs.m_arrayInfo, rhs.m_arrayInfo)) {
        return false;
    }
    if (lhs.m_isCompoundType &&
        !sameCompoundInfo(lhs.m_compoundInfo, rhs.m_compoundInfo)) {
        return false;
    }
    return true;
}

std::map<std::string, SymbolInfo> visibleSymbols(const SymbolTable& table)
{
    std::map<std::string, SymbolInfo> visible;
    for (auto it = table.m_currentScope.rbegin();
         it != table.m_currentScope.rend(); ++it) {
        visible.emplace(it->first, it->second);
    }
    return visible;
}

bool belongsToVisibleSymbol(
    const std::string& storageName,
    const std::set<std::string>& visibleNames
)
{
    for (const auto& symbolName : visibleNames) {
        if (storageName == symbolName) {
            return true;
        }
        if (storageName.size() <= symbolName.size() ||
            storageName.compare(0, symbolName.size(), symbolName) != 0) {
            continue;
        }
        const char separator = storageName[symbolName.size()];
        if (separator == '.' || separator == '[') {
            return true;
        }
    }
    return false;
}

std::map<std::string, StackElement> visibleFixedBindings(
    const SymbolTable& table,
    const std::set<std::string>& visibleNames
)
{
    std::map<std::string, StackElement> bindings;
    if (!table.m_fixedStackPtr) {
        return bindings;
    }

    for (const auto& element : table.m_fixedStackPtr->getStackContent()) {
        if (belongsToVisibleSymbol(element.getName(), visibleNames)) {
            bindings.insert_or_assign(element.getName(), element);
        }
    }
    return bindings;
}

ControlFlowJoinResult compareEntryVisibleCompilerState(
    const SymbolTable& entry,
    const SymbolTable& branch
)
{
    const auto entrySymbols = visibleSymbols(entry);
    const auto branchSymbols = visibleSymbols(branch);
    std::set<std::string> visibleNames;

    for (const auto& [name, entryInfo] : entrySymbols) {
        visibleNames.insert(name);
        const auto branchIt = branchSymbols.find(name);
        if (branchIt == branchSymbols.end() ||
            !sameSymbolMetadata(entryInfo, branchIt->second)) {
            return {
                ControlFlowMismatch::SYMBOL_METADATA,
                "compiler metadata differs for entry-visible symbol '" +
                    name + "'"
            };
        }
    }

    if (entry.m_bindSymbol != branch.m_bindSymbol ||
        entry.activeBindSymbolStart() != branch.activeBindSymbolStart() ||
        entry.activeScopeEntryStart() != branch.activeScopeEntryStart()) {
        return {
            ControlFlowMismatch::SYMBOL_METADATA,
            "compiler symbol bindings differ at the join point"
        };
    }

    if (entry.m_keepSymbol != branch.m_keepSymbol) {
        return {
            ControlFlowMismatch::SYMBOL_METADATA,
            "compiler keep/return markers differ at the join point"
        };
    }

    const auto entryFixed = visibleFixedBindings(entry, visibleNames);
    const auto branchFixed = visibleFixedBindings(branch, visibleNames);
    if (entryFixed.size() != branchFixed.size()) {
        return {
            ControlFlowMismatch::FIXED_STORAGE,
            "fixed storage differs for entry-visible symbols"
        };
    }

    for (const auto& [name, entryElement] : entryFixed) {
        const auto branchIt = branchFixed.find(name);
        if (branchIt == branchFixed.end() ||
            !sameFixedBinding(entryElement, branchIt->second)) {
            return {
                ControlFlowMismatch::FIXED_STORAGE,
                "fixed storage differs for entry-visible symbol '" + name +
                    "'"
            };
        }
    }

    return {};
}

} // namespace

void Scope::push(
    const std::string& nameStr,
    const std::string& typeStr,
    const std::string dataStr /*=""*/
)
{
    StackElement element(nameStr, typeStr, dataStr);
    push(std::move(element));
}

void Scope::push(StackElement&& element)
{
    m_currentSymtab.push(element);
}

void Scope::push(const StackElement& element)
{
    m_currentSymtab.push(element);
}

void Scope::push(const valtype& data)
{
    StackElement element(data);
    push(std::move(element));
}

std::optional<StackElement> Scope::pop()
{
    auto stackTopOpt = m_currentSymtab.pop();
    return stackTopOpt;
}

StackElement& Scope::top()
{
    if (!m_currentSymtab.m_stackPtr ||
        m_currentSymtab.m_stackPtr->empty()) {
        throw std::underflow_error("cannot read the top of an empty main stack");
    }
    return m_currentSymtab.m_stackPtr->top();
}

const StackElement& Scope::stacktop(int index) const
{
    return m_currentSymtab.m_stackPtr->stacktop(index);
}

StackElement& Scope::stacktop(int index)
{
    return m_currentSymtab.m_stackPtr->stacktop(index);
}

size_t Scope::size() const
{
    return m_currentSymtab.m_stackPtr->size();
}

bool Scope::empty() const
{
    return m_currentSymtab.m_stackPtr->empty();
}

const StackElement& Scope::front() const
{
    if (!m_currentSymtab.m_stackPtr ||
        m_currentSymtab.m_stackPtr->empty()) {
        throw std::underflow_error("cannot read the front of an empty main stack");
    }
    return m_currentSymtab.m_stackPtr->front();
}

const StackElement& Scope::back() const
{
    if (!m_currentSymtab.m_stackPtr ||
        m_currentSymtab.m_stackPtr->empty()) {
        throw std::underflow_error("cannot read the back of an empty main stack");
    }
    return m_currentSymtab.m_stackPtr->back();
}

const StackElement& Scope::at(uint64_t index) const
{
    return m_currentSymtab.m_stackPtr->at(index);
}

StackElement& Scope::at(uint64_t index)
{
    return m_currentSymtab.m_stackPtr->at(index);
}

void Scope::erase(int index)
{
    m_currentSymtab.m_stackPtr->erase(index);
}

void Scope::swap(int index1, int index2)
{
    m_currentSymtab.m_stackPtr->swap(index1, index2);
}

size_t Scope::getCombinedStackSize() const
{
    return m_currentSymtab.m_stackPtr->getCombinedStackSize();
}

size_t Scope::getMaxStackSize() const
{
    return m_currentSymtab.m_stackPtr->getMaxStackSize();
}

void Scope::pick(const int offsetFromTop)
{
    m_currentSymtab.pick(offsetFromTop);
}

void Scope::roll(const int offsetFromTop)
{
    m_currentSymtab.roll(offsetFromTop);
}

void Scope::dropAt(const int offsetFromTop)
{
    m_currentSymtab.dropAt(offsetFromTop);
}

std::optional<int64_t>
Scope::getPos(const std::string& name, bool isAltFlag /*= false*/) const
{
    StackElement element(name);
    return getPos(element, isAltFlag);
}

std::optional<int64_t>
Scope::getPos(StackElement& element, bool isAltFlag /*= false*/) const
{
    return m_currentSymtab.getPos(element, isAltFlag);
}

void Scope::stackStatus(std::string& statusStr)
{
    m_currentSymtab.stackStatus(statusStr);
}

int32_t Scope::setAlt(std::string& symbolName, bool moveOnlyMatch /*= false*/)
{
    int32_t num = 0;
    if (moveOnlyMatch) {
        auto elementPosOpt = getPos(symbolName);
        if (!elementPosOpt.has_value()) {
            throw std::invalid_argument(
                "cannot move symbol to alt stack: symbol not found: " +
                symbolName
            );
        }
        num = elementPosOpt.value();
        roll(num);
        auto stackElementTopOpt = pop();
        if (!stackElementTopOpt.has_value()) {
            throw std::underflow_error(
                "cannot move symbol to alt stack: main stack is empty"
            );
        }
        m_currentSymtab.m_altStackPtr->push(stackElementTopOpt.value());
        return num;
    }
    return m_currentSymtab.setAlt(symbolName);
}

int32_t Scope::setMain(std::string& symbolName)
{
    return m_currentSymtab.setMain(symbolName);
}

void Scope::clean()
{
    m_currentSymtab.resetFunctionState();
    while (!m_symtabScopes.empty()) {
        m_symtabScopes.pop();
    }

    std::string validationError;
    if (!m_currentSymtab.validateState(&validationError)) {
        throw std::logic_error(
            "invalid symbol-table state after function reset: " +
            validationError
        );
    }
}

void Scope::enterScope()
{
    // 先保存父作用域完整状态，再清空子作用域的归属记录。若先清空，
    // 嵌套块会永久丢失父作用域的 m_newSymbol/m_declaredSymbols。
    m_symtabScopes.push(m_currentSymtab);
    m_currentSymtab.m_newSymbol.clear();
    m_currentSymtab.m_declaredSymbols.clear();
    LOG_DEBUG("Enter scope, symtabScopes size:", m_symtabScopes.size());
}

SymbolTable Scope::exitScope()
{
    auto currentSymtab = m_currentSymtab;
    if (!m_symtabScopes.empty()) {
        auto willIt = m_symtabScopes.top();
        m_currentSymtab = willIt;
        m_symtabScopes.pop();
    } else {
        m_currentSymtab = SymbolTable();
    }
    LOG_DEBUG("Exit scope, symtabScopes size:", m_symtabScopes.size());
    return currentSymtab;
}

bool Scope::popScopeStack()
{
    if (!m_symtabScopes.empty()) {
        m_symtabScopes.pop();
        LOG_DEBUG("Pop scope stack, symtabScopes size:", m_symtabScopes.size());
        return true;
    }
    LOG_DEBUG("Pop scope stack failed: stack is empty");
    return false;
}

ControlFlowStateSnapshot Scope::captureControlFlowState() const
{
    ControlFlowStateSnapshot snapshot;
    snapshot.symbolTable = m_currentSymtab;
    if (m_currentSymtab.m_altStackPtr) {
        snapshot.altStack =
            m_currentSymtab.m_altStackPtr->getStackContent();
        snapshot.altCombinedSize =
            m_currentSymtab.m_altStackPtr->getCombinedStackSize();
    }
    if (m_currentSymtab.m_stackPtr) {
        snapshot.mainCombinedSize =
            m_currentSymtab.m_stackPtr->getCombinedStackSize();
    }
    snapshot.lexicalDepth = m_symtabScopes.size();
    return snapshot;
}

ControlFlowStateSnapshot Scope::captureControlFlowStateAndExitScope()
{
    auto snapshot = captureControlFlowState();
    snapshot.symbolTable = exitScope();
    snapshot.lexicalDepth = m_symtabScopes.size();
    return snapshot;
}

void Scope::restoreControlFlowState(
    const ControlFlowStateSnapshot& snapshot
)
{
    if (m_symtabScopes.size() != snapshot.lexicalDepth) {
        std::ostringstream oss;
        oss << "Cannot restore control-flow state at lexical depth "
            << m_symtabScopes.size() << "; expected "
            << snapshot.lexicalDepth;
        throw std::runtime_error(oss.str());
    }

    m_currentSymtab = snapshot.symbolTable;
    if (m_currentSymtab.m_stackPtr) {
        m_currentSymtab.m_stackPtr->setCombinedStackSize(
            snapshot.mainCombinedSize
        );
    }
    if (m_currentSymtab.m_altStackPtr) {
        m_currentSymtab.m_altStackPtr->replaceStackContent(snapshot.altStack);
        m_currentSymtab.m_altStackPtr->setCombinedStackSize(
            snapshot.altCombinedSize
        );
    }
}

ControlFlowJoinResult Scope::compareControlFlowStates(
    const ControlFlowStateSnapshot& entry,
    const ControlFlowStateSnapshot& branch,
    ControlFlowJoinPolicy policy
) const
{
    if (entry.lexicalDepth != branch.lexicalDepth) {
        return {
            ControlFlowMismatch::LEXICAL_SCOPE,
            "lexical scope depth differs at the join point"
        };
    }

    if (!entry.symbolTable.m_stackPtr ||
        !branch.symbolTable.m_stackPtr) {
        return {
            ControlFlowMismatch::MAIN_STACK_LAYOUT,
            "main stack is missing at the join point"
        };
    }

    const auto& entryMain =
        entry.symbolTable.m_stackPtr->getStackContent();
    const auto& branchMain =
        branch.symbolTable.m_stackPtr->getStackContent();
    const bool compareTypes =
        policy == ControlFlowJoinPolicy::IMPLICIT_EMPTY_BRANCH;
    auto result = compareRuntimeStack(
        entryMain,
        branchMain,
        ControlFlowMismatch::MAIN_STACK_DEPTH,
        ControlFlowMismatch::MAIN_STACK_LAYOUT,
        "main stack",
        compareTypes
    );
    if (!result.compatible()) {
        return result;
    }

    result = compareRuntimeStack(
        entry.altStack,
        branch.altStack,
        ControlFlowMismatch::ALT_STACK_DEPTH,
        ControlFlowMismatch::ALT_STACK_LAYOUT,
        "alternative stack",
        compareTypes
    );
    if (!result.compatible()) {
        return result;
    }

    if (policy == ControlFlowJoinPolicy::IMPLICIT_EMPTY_BRANCH) {
        return compareEntryVisibleCompilerState(
            entry.symbolTable, branch.symbolTable
        );
    }
    return {};
}

bool Scope::defineSymbol(
    std::string name,
    std::string type /*= ""*/,
    std::string modifiers /*= ""*/
)
{
    return m_currentSymtab.defineSymbol(name, type, modifiers);
}

bool Scope::symbolExists(std::string& name) const
{
    return m_currentSymtab.symbolExists(name);
}

void Scope::setFixed(StackElement element)
{
    m_currentSymtab.setFixed(element);
}

std::optional<StackElement> Scope::getFixed(std::string& elementStr)
{
    return m_currentSymtab.getFixed(elementStr);
}

void Scope::removeFixed(std::string& elementStr)
{
    m_currentSymtab.removeFixed(elementStr);
}

bool Scope::defineArray(
    const std::string& name,
    const std::string& elementType,
    size_t size,
    bool isFixedSize
)
{
    return m_currentSymtab.defineArray(name, elementType, size, isFixedSize);
}

bool Scope::importExternalArrayView(
    const std::string& name,
    const std::string& elementType,
    size_t size,
    bool isFixedSize
)
{
    return m_currentSymtab.importExternalArrayView(
        name, elementType, size, isFixedSize
    );
}

bool Scope::isArraySymbol(const std::string& name) const
{
    return m_currentSymtab.isArraySymbol(name);
}

std::optional<ArrayInfo> Scope::getArrayInfo(const std::string& name) const
{
    return m_currentSymtab.getArrayInfo(name);
}

std::string
Scope::getArrayElementLabel(const std::string& arrayName, size_t index) const
{
    return m_currentSymtab.getArrayElementLabel(arrayName, index);
}

std::optional<int64_t>
Scope::getArrayElementPos(const std::string& arrayName, size_t index) const
{
    return m_currentSymtab.getArrayElementPos(arrayName, index);
}

bool Scope::rename(const std::string& oldName, const std::string& newName)
{
    if (m_currentSymtab.m_stackPtr) {
        return m_currentSymtab.m_stackPtr->renameTopToBottom(
            oldName, newName
        );
    }
    return false;
}

bool Scope::renameAtPosition(int position, const std::string& newName)
{
    if (m_currentSymtab.m_stackPtr) {
        return m_currentSymtab.m_stackPtr->renameAtPosition(position, newName);
    }
    return false;
}

bool Scope::isSymbolInitialized(const std::string& name) const
{
    return m_currentSymtab.isSymbolInitialized(name);
}

void Scope::markSymbolInitialized(const std::string& name)
{
    m_currentSymtab.markSymbolInitialized(name);
}

void Scope::symbolStatus(std::string& newSymbolStr, std::string& statusStr)
{
    m_currentSymtab.symbolStatus(newSymbolStr, statusStr);
}

bool Scope::defineCompoundType(const CompoundTypeInfo& compoundInfo)
{
    return m_currentSymtab.defineCompoundType(compoundInfo);
}

bool Scope::isCompoundTypeSymbol(const std::string& name) const
{
    return m_currentSymtab.isCompoundTypeSymbol(name);
}

std::optional<CompoundTypeInfo> Scope::getCompoundTypeInfo(
    const std::string& name
) const
{
    return m_currentSymtab.getCompoundTypeInfo(name);
}

bool Scope::isCompoundTypeSplitted(const std::string& name) const
{
    return m_currentSymtab.isCompoundTypeSplitted(name);
}

void Scope::markCompoundTypeSplitted(const std::string& name)
{
    m_currentSymtab.markCompoundTypeSplitted(name);
}

bool Scope::renameSymbolEntry(
    const std::string& oldName,
    const std::string& newName
)
{
    return m_currentSymtab.renameSymbolEntry(oldName, newName);
}

bool Scope::renameArraySymbol(
    const std::string& oldName,
    const std::string& newName
)
{
    return m_currentSymtab.renameArraySymbol(oldName, newName);
}

bool Scope::renameCompoundSymbol(
    const std::string& oldName,
    const std::string& newName
)
{
    return m_currentSymtab.renameCompoundSymbol(oldName, newName);
}

void Scope::renameEntriesByPrefix(
    const std::string& oldPrefix,
    const std::string& newPrefix
)
{
    m_currentSymtab.renameEntriesByPrefix(oldPrefix, newPrefix);
}

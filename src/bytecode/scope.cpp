#include "scope.h"

#include "../log/logger.h"

using namespace tbc;

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
    return m_currentSymtab.m_stackPtr->front();
}

const StackElement& Scope::back() const
{
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
            throw;
        }
        num = elementPosOpt.value();
        roll(num);
        auto stackElementTopOpt = pop();
        if (!stackElementTopOpt.has_value()) {
            throw;
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
    m_currentSymtab = SymbolTable();
    while (!m_symtabScopes.empty()) {
        m_symtabScopes.pop();
    }
}

void Scope::enterScope()
{
    m_currentSymtab.m_newSymbol.clear();
    m_currentSymtab.m_declaredSymbols.clear();
    m_symtabScopes.push(m_currentSymtab);
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
        return m_currentSymtab.m_stackPtr->rename(oldName, newName);
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

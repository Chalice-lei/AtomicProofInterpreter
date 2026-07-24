#include "op_stack.h"

#include <algorithm>
#include <sstream>

using namespace tbc;

// ===== StackElement =====

StackElement::StackElement(int64_t value)
{
    if (value == 0) {
        // Bitcoin 标准：0 用空数组表示
        m_data.clear();
    } else {
        CScriptNum scriptNum(value);
        m_data = scriptNum.getvch();
    }
}

std::string StackElement::getName() const
{
    return std::string(m_name.begin(), m_name.end());
}

std::string StackElement::getType() const
{
    return std::string(m_type.begin(), m_type.end());
}

std::string StackElement::getData() const
{
    return std::string(m_data.begin(), m_data.end());
}

void StackElement::minimally_encode()
{
    if (m_name.empty()) {
        return;
    }

    // 去掉前导零但保留符号位
    while (m_name.size() > 1 && m_name.back() == 0x00 &&
           (m_name[m_name.size() - 2] & 0x80) == 0) {
        m_name.pop_back();
    }

    // 最高位为 1 但实际为正数，补一个零字节
    if (!m_name.empty() && (m_name.back() & 0x80) != 0) {
        m_name.push_back(0x00);
    }
}

bool StackElement::is_minimally_encoded() const
{
    if (m_name.empty()) {
        return true;
    }

    if (m_name.size() > 1 && m_name.back() == 0x00 &&
        (m_name[m_name.size() - 2] & 0x80) == 0) {
        return false;
    }

    return true;
}

// ===== CScriptNum =====

CScriptNum::CScriptNum(const valtype& vch, bool fRequireMinimal)
{
    if (fRequireMinimal && !IsMinimallyEncoded(vch)) {
        throw script_num_error("Script number is not minimally encoded");
    }

    if (vch.empty()) {
        m_value = 0;
        return;
    }

    m_value = 0;
    for (size_t i = 0; i < vch.size(); ++i) {
        m_value |= static_cast<int64_t>(vch[i]) << (8 * i);
    }

    if (vch.back() & 0x80) {
        m_value &= ~(0x80ULL << (8 * (vch.size() - 1)));
        m_value = -m_value;
    }
}

CScriptNum::CScriptNum(const valtype& vch, bool fRequireMinimal, size_t maxSize)
{
    if (vch.size() > maxSize) {
        throw script_num_error("Script number overflow");
    }

    if (fRequireMinimal && !IsMinimallyEncoded(vch)) {
        throw script_num_error("Script number is not minimally encoded");
    }

    if (vch.empty()) {
        m_value = 0;
        return;
    }

    m_value = 0;
    for (size_t i = 0; i < vch.size(); ++i) {
        m_value |= static_cast<int64_t>(vch[i]) << (8 * i);
    }

    if (vch.back() & 0x80) {
        m_value &= ~(0x80ULL << (8 * (vch.size() - 1)));
        m_value = -m_value;
    }
}

bool CScriptNum::IsMinimallyEncoded(const valtype& data)
{
    if (data.empty()) {
        return true;
    }

    if (data.size() > 1 && data.back() == 0x00 &&
        (data[data.size() - 2] & 0x80) == 0) {
        return false;
    }

    return true;
}

valtype CScriptNum::Serialize(const int64_t& value)
{
    if (value == 0) {
        return valtype();
    }

    valtype result;
    const bool neg = value < 0;
    uint64_t absvalue = neg ? -value : value;

    while (absvalue) {
        result.push_back(absvalue & 0xff);
        absvalue >>= 8;
    }

    // 最高位已是 1 时需要额外字节存符号
    if (result.back() & 0x80) {
        result.push_back(neg ? 0x80 : 0);
    } else if (neg) {
        result.back() |= 0x80;
    }

    return result;
}

// ===== OpStack =====

OpStack::OpStack(size_t maxStackSize)
    : m_maxStackSize(maxStackSize), m_combinedStackSize(0),
      m_parentStack(nullptr)
{}

OpStack::OpStack(OpStack* parent)
    : m_maxStackSize(0), m_combinedStackSize(0), m_parentStack(parent)
{}

size_t OpStack::getCombinedStackSize() const
{
    if (m_parentStack != nullptr) {
        return m_parentStack->getCombinedStackSize();
    }
    return m_combinedStackSize;
}

void OpStack::setCombinedStackSize(size_t size)
{
    if (m_parentStack != nullptr) {
        m_parentStack->setCombinedStackSize(size);
    } else {
        m_combinedStackSize = size;
    }
}

void OpStack::replaceStackContent(
    const std::vector<StackElement>& newContent
)
{
    size_t oldContentSize = 0;
    for (const auto& element : m_stack) {
        oldContentSize += element.getMemoryUsage();
    }

    size_t newContentSize = 0;
    for (const auto& element : newContent) {
        newContentSize += element.getMemoryUsage();
    }

    const size_t combinedSize = getCombinedStackSize();
    if (combinedSize < oldContentSize) {
        throw std::logic_error(
            "cannot replace stack content: combined stack size is smaller "
            "than the current content"
        );
    }
    // 先完成可能抛出 bad_alloc 的复制，再修改共享计数；最终 swap 不抛。
    // increase/decrease 会沿 parent 链更新根计数并执行根栈上限检查。
    std::vector<StackElement> replacement(newContent);
    if (newContentSize > oldContentSize) {
        increaseCombinedStackSize(newContentSize - oldContentSize);
    } else if (oldContentSize > newContentSize) {
        decreaseCombinedStackSize(oldContentSize - newContentSize);
    }
    m_stack.swap(replacement);
}

void OpStack::push(const StackElement& element, std::string* statusStr)
{
    increaseCombinedStackSize(element.getMemoryUsage());
    m_stack.push_back(element);
    if (statusStr != nullptr) {
        stackStatus(*statusStr);
    }
}

void OpStack::push(StackElement&& element, std::string* statusStr)
{
    increaseCombinedStackSize(element.getMemoryUsage());
    m_stack.push_back(std::move(element));
    if (statusStr != nullptr) {
        stackStatus(*statusStr);
    }
}

void OpStack::push(const valtype& data)
{
    push(StackElement(data));
}

void OpStack::push(const std::string& str)
{
    push(StackElement(str));
}

void OpStack::push(int64_t value)
{
    push(StackElement(value));
}

void OpStack::push(bool value)
{
    push(StackElement(value));
}

void OpStack::pop(std::string* statusStr)
{
    if (!m_stack.empty()) {
        decreaseCombinedStackSize(m_stack.back().getMemoryUsage());
        m_stack.pop_back();
        if (statusStr != nullptr) {
            stackStatus(*statusStr);
        }
    }
}

const StackElement& OpStack::stacktop(int index) const
{
    if (index < STACK_TOP_POS) {
        throw std::invalid_argument(
            "Invalid argument - index should be >= STACK_TOP_POS."
        );
    }

    if (index >= static_cast<int>(m_stack.size())) {
        throw std::invalid_argument("Invalid argument - index out of range.");
    }

    return at(index);
}

StackElement& OpStack::stacktop(int index)
{
    if (index < STACK_TOP_POS) {
        throw std::invalid_argument(
            "Invalid argument - index should be >= STACK_TOP_POS."
        );
    }

    if (index >= static_cast<int>(m_stack.size())) {
        throw std::invalid_argument("Invalid argument - index out of range.");
    }

    return at(index);
}

const StackElement& OpStack::front() const
{
    return m_stack.front();
}

const StackElement& OpStack::back() const
{
    return m_stack.back();
}

const StackElement& OpStack::at(uint64_t index) const
{
    if (index < static_cast<uint64_t>(STACK_TOP_POS) ||
        index >= m_stack.size()) {
        throw std::invalid_argument("Invalid argument - index out of range.");
    }

    // 栈顶 index=0，转为数组下标
    int realIndex = static_cast<int>(m_stack.size()) - 1 - index;
    return m_stack.at(realIndex);
}

StackElement& OpStack::at(uint64_t index)
{
    if (index < static_cast<uint64_t>(STACK_TOP_POS) ||
        index >= m_stack.size()) {
        throw std::invalid_argument("Invalid argument - index out of range.");
    }

    // 栈顶 index=0，转为数组下标
    int realIndex = static_cast<int>(m_stack.size()) - 1 - index;
    return m_stack.at(realIndex);
}

void OpStack::erase(int index)
{
    if (index < STACK_TOP_POS) {
        throw std::invalid_argument(
            "Invalid argument - index should be >= STACK_TOP_POS."
        );
    }

    if (index >= static_cast<int>(m_stack.size())) {
        throw std::invalid_argument("Invalid argument - index out of range.");
    }

    int realIndex = static_cast<int>(m_stack.size()) - 1 - index;

    decreaseCombinedStackSize(m_stack[realIndex].getMemoryUsage());
    m_stack.erase(m_stack.begin() + realIndex);
}

void OpStack::insert(int position, const StackElement& element)
{
    if (position < STACK_TOP_POS) {
        throw std::invalid_argument(
            "Invalid argument - position should be >= STACK_TOP_POS."
        );
    }

    if (position > static_cast<int>(m_stack.size())) {
        throw std::invalid_argument("Invalid argument - position out of range."
        );
    }

    increaseCombinedStackSize(element.getMemoryUsage());

    int realPosition = static_cast<int>(m_stack.size()) - position;
    m_stack.insert(m_stack.begin() + realPosition, element);
}

void OpStack::swap(int index1, int index2)
{
    if (index1 < STACK_TOP_POS || index1 >= static_cast<int>(m_stack.size()) ||
        index2 < STACK_TOP_POS || index2 >= static_cast<int>(m_stack.size())) {
        throw std::invalid_argument("Invalid argument - indices out of range.");
    }

    int realIndex1 = static_cast<int>(m_stack.size()) - 1 - index1;
    int realIndex2 = static_cast<int>(m_stack.size()) - 1 - index2;

    std::swap(m_stack[realIndex1], m_stack[realIndex2]);
}

OpStack OpStack::makeChildStack()
{
    OpStack childStack(this);
    return childStack;
}

std::shared_ptr<OpStack> OpStack::makeChildStackPtr()
{
    // 私有构造无法被 make_shared 访问，改用裸指针构造
    return std::shared_ptr<OpStack>(new OpStack(this));
}

OpStack OpStack::makeRootStackCopy() const
{
    if (m_parentStack != nullptr) {
        throw std::runtime_error(
            "Parent stack must be null if you are creating stack copy."
        );
    }
    return *this;
}

void OpStack::moveTopToStack(OpStack& otherStack, bool forceAllow)
{
    // 仅允许父子栈关系，除非 forceAllow=true
    if (!forceAllow && m_parentStack != &otherStack &&
        otherStack.getParentStack() != this) {
        throw std::runtime_error("Method moveTopToStack is allowed only for "
                                 "relations parent-child.");
    }

    if (!otherStack.m_stack.empty()) {
        StackElement topElement = otherStack.m_stack.back();

        otherStack.decreaseCombinedStackSize(topElement.getMemoryUsage());
        increaseCombinedStackSize(topElement.getMemoryUsage());

        otherStack.m_stack.pop_back();
        m_stack.push_back(topElement);
    }
}

void OpStack::clear()
{
    for (const auto& element : m_stack) {
        decreaseCombinedStackSize(element.getMemoryUsage());
    }
    m_stack.clear();
}

bool OpStack::operator==(const OpStack& other) const
{
    if (m_stack.size() != other.m_stack.size()) {
        return false;
    }

    for (size_t i = 0; i < m_stack.size(); ++i) {
        if (m_stack[i] != other.m_stack[i]) {
            return false;
        }
    }

    return true;
}

void OpStack::stackStatus(std::string& statusStr)
{
    std::ostringstream oss;
    oss << this << " | ";
    for (size_t i = 0; i < size(); i++) {
        oss << std::to_string(i) << ": \""
            << stacktop(static_cast<int>(i)).getName() << "\"-\""
            << stacktop(static_cast<int>(i)).getType() << "\" | ";
    }
    statusStr += oss.str();
}

void OpStack::checkStackLimit(size_t additionalSize) const
{
    if (getCombinedStackSize() + additionalSize > m_maxStackSize &&
        m_parentStack == nullptr) {
        throw stack_overflow_error(
            "Stack operation would exceed maximum stack size"
        );
    }
}

void OpStack::increaseCombinedStackSize(size_t additionalSize)
{
    if (m_parentStack != nullptr) {
        m_parentStack->increaseCombinedStackSize(additionalSize);
    } else {
        checkStackLimit(additionalSize);
        m_combinedStackSize += additionalSize;
    }
}

void OpStack::decreaseCombinedStackSize(size_t additionalSize)
{
    if (m_parentStack != nullptr) {
        m_parentStack->decreaseCombinedStackSize(additionalSize);
    } else {
        m_combinedStackSize -= additionalSize;
    }
}

// ===== 重命名操作（零成本优化） =====

bool OpStack::rename(const std::string& oldName, const std::string& newName)
{
    for (auto& element : m_stack) {
        if (element.getName() == oldName) {
            element.setName(newName);
            return true;
        }
    }
    return false;
}

bool OpStack::renameTopToBottom(
    const std::string& oldName,
    const std::string& newName
)
{
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->getName() == oldName) {
            it->setName(newName);
            return true;
        }
    }
    return false;
}

bool OpStack::renameAtPosition(int position, const std::string& newName)
{
    if (position < STACK_TOP_POS ||
        position >= static_cast<int>(m_stack.size())) {
        return false;
    }

    int realIndex = static_cast<int>(m_stack.size()) - 1 - position;
    m_stack[realIndex].setName(newName);
    return true;
}

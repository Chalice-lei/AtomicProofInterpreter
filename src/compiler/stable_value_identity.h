#ifndef STABLE_VALUE_IDENTITY_H
#define STABLE_VALUE_IDENTITY_H

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace apc::compiler
{

// StableId deliberately has no implicit integer or cross-domain conversion.
// A raw value is meaningful only together with its tag, which prevents a
// binding identifier from being accidentally used as a value or storage atom.
template <typename Tag>
class StableId
{
public:
    using ValueType = uint64_t;

    constexpr StableId() noexcept = default;
    explicit constexpr StableId(ValueType value) noexcept : m_value(value)
    {}

    constexpr ValueType value() const noexcept
    {
        return m_value;
    }

    constexpr bool valid() const noexcept
    {
        return m_value != 0;
    }

    explicit constexpr operator bool() const noexcept
    {
        return valid();
    }

    friend constexpr auto operator<=>(const StableId&, const StableId&) =
        default;

private:
    ValueType m_value{0};
};

struct BindingIdTag;
struct ValueIdTag;
struct AtomIdTag;
struct InlineFrameIdTag;
struct CallSiteIdTag;
struct LoopIterationIdTag;
struct BasicBlockIdTag;

using BindingId = StableId<BindingIdTag>;
using ValueId = StableId<ValueIdTag>;
using AtomId = StableId<AtomIdTag>;
using InlineFrameId = StableId<InlineFrameIdTag>;
using CallSiteId = StableId<CallSiteIdTag>;
using LoopIterationId = StableId<LoopIterationIdTag>;
using BasicBlockId = StableId<BasicBlockIdTag>;

using AtomSet = std::set<AtomId>;
using LeafPath = std::vector<uint32_t>;

// A value can denote a scalar atom, a statically-known aggregate, or a set of
// atoms selected at run time.  Keeping the leaf path alongside the atom lets a
// frontend preserve field identity while atoms() remains the conservative
// interface used by lifetime analysis.
struct AtomLeaf
{
    LeafPath path;
    AtomId atom;

    friend auto operator<=>(const AtomLeaf&, const AtomLeaf&) = default;
};

template <typename Id>
struct StableIdHash
{
    size_t operator()(Id id) const noexcept
    {
        return std::hash<typename Id::ValueType>{}(id.value());
    }
};

struct ValueContext
{
    InlineFrameId inlineFrame;
    CallSiteId callSite;
    LoopIterationId loopIteration;

    friend bool operator==(const ValueContext&, const ValueContext&) = default;
};

struct ValueIdentity
{
    BindingId binding;
    ValueId value;
    // Compatibility scalar.  Aggregate identities leave this invalid and use
    // leaves; scalar identities may omit leaves.
    AtomId atom;
    ValueContext context;
    std::vector<AtomLeaf> leaves;

    ValueIdentity() = default;

    ValueIdentity(
        BindingId identityBinding,
        ValueId identityValue,
        AtomId scalarAtom,
        ValueContext identityContext = {},
        std::vector<AtomLeaf> identityLeaves = {}
    )
        : binding(identityBinding),
          value(identityValue),
          atom(scalarAtom),
          context(identityContext),
          leaves(std::move(identityLeaves))
    {}

    AtomSet atoms() const
    {
        AtomSet result;
        if (atom.valid()) {
            result.insert(atom);
        }
        for (const auto& leaf : leaves) {
            if (leaf.atom.valid()) {
                result.insert(leaf.atom);
            }
        }
        return result;
    }

    friend bool operator==(const ValueIdentity&, const ValueIdentity&) = default;
};

// Monotonic identifiers are deterministic for a deterministic traversal.  The
// counters are intentionally independent: equal raw values in different ID
// domains are still different types and cannot be mixed by the compiler.
class StableValueIdFactory
{
public:
    BindingId nextBinding()
    {
        return next<BindingId>(m_nextBinding);
    }
    ValueId nextValue()
    {
        return next<ValueId>(m_nextValue);
    }
    AtomId nextAtom()
    {
        return next<AtomId>(m_nextAtom);
    }
    InlineFrameId nextInlineFrame()
    {
        return next<InlineFrameId>(m_nextInlineFrame);
    }
    CallSiteId nextCallSite()
    {
        return next<CallSiteId>(m_nextCallSite);
    }
    LoopIterationId nextLoopIteration()
    {
        return next<LoopIterationId>(m_nextLoopIteration);
    }
    BasicBlockId nextBasicBlock()
    {
        return next<BasicBlockId>(m_nextBasicBlock);
    }

private:
    template <typename Id>
    static Id next(uint64_t& counter)
    {
        if (counter == std::numeric_limits<uint64_t>::max()) {
            throw std::overflow_error("stable value identifier space exhausted");
        }
        return Id(counter++);
    }

    uint64_t m_nextBinding{1};
    uint64_t m_nextValue{1};
    uint64_t m_nextAtom{1};
    uint64_t m_nextInlineFrame{1};
    uint64_t m_nextCallSite{1};
    uint64_t m_nextLoopIteration{1};
    uint64_t m_nextBasicBlock{1};
};

} // namespace apc::compiler

#endif // STABLE_VALUE_IDENTITY_H

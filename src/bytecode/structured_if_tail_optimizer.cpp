#include "structured_if_tail_optimizer.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <utility>

#include "bytecode_opcodes.h"

namespace tbc
{
namespace
{

constexpr size_t kInvalidPC = std::numeric_limits<size_t>::max();

std::string normalizeHex(std::string value)
{
    if (value.size() >= 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        value.erase(0, 2);
    }
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); }
    );
    return value;
}

bool isPureHex(const std::string& value)
{
    const std::string normalized = normalizeHex(value);
    return !normalized.empty() && (normalized.size() % 2) == 0 &&
           std::all_of(
               normalized.begin(), normalized.end(), [](unsigned char ch) {
                   return std::isxdigit(ch) != 0;
               }
           );
}

bool isSingleOpcode(const std::string& value, BytOpcode opcode)
{
    return isPureHex(value) &&
           normalizeHex(value) == opcodeToHex(opcode);
}

enum class ControlToken { NONE, IF, NOTIF, ELSE, ENDIF };

ControlToken controlToken(const std::string& value)
{
    if (isSingleOpcode(value, BytOpcode::OP_IF)) {
        return ControlToken::IF;
    }
    if (isSingleOpcode(value, BytOpcode::OP_NOTIF)) {
        return ControlToken::NOTIF;
    }
    if (isSingleOpcode(value, BytOpcode::OP_ELSE)) {
        return ControlToken::ELSE;
    }
    if (isSingleOpcode(value, BytOpcode::OP_ENDIF)) {
        return ControlToken::ENDIF;
    }
    return ControlToken::NONE;
}

bool isKnownDataPlaceholderEncoding(
    const std::string& value,
    const StructuredIfTailOptions& options
)
{
    if (options.knownDataPlaceholderLabels.empty() || value.size() < 3 ||
        value.front() != '<' || value.back() != '>') {
        return false;
    }
    // Treat only an entire placeholder instruction as inert pushed data.
    // Accepting mixed encodings such as "6a<self.x>" would hide an
    // OP_RETURN/OP_CODESEPARATOR prefix inside an otherwise movable atom.
    if (value.find('<', 1) != std::string::npos ||
        value.find('>', 0) != value.size() - 1) {
        return false;
    }
    return options.knownDataPlaceholderLabels.contains(
        value.substr(1, value.size() - 2)
    );
}

struct Atom
{
    std::string encoding;
    std::vector<InstructionOriginRef> origins;
    bool extractionSafe{false};
};

struct Node
{
    enum class Kind { ATOM, IF_REGION };

    Kind kind{Kind::ATOM};
    Atom atom;

    Atom opener;
    std::vector<Node> thenBody;
    std::optional<Atom> elseMarker;
    std::vector<Node> elseBody;
    Atom endMarker;
};

using Sequence = std::vector<Node>;

Atom makeAtom(
    const std::string& encoding,
    size_t oldPC,
    const std::vector<BranchPredicate>& path,
    const StructuredIfTailOptions& options
)
{
    Atom atom;
    atom.encoding = encoding;
    atom.origins.push_back(InstructionOriginRef{oldPC, path});
    atom.extractionSafe =
        (isPureHex(encoding) &&
         !isSingleOpcode(encoding, BytOpcode::OP_RETURN) &&
         !isSingleOpcode(encoding, BytOpcode::OP_CODESEPARATOR)) ||
        isKnownDataPlaceholderEncoding(encoding, options);
    return atom;
}

class Parser
{
public:
    Parser(
        const std::vector<std::string>& instructions,
        const StructuredIfTailOptions& options
    )
        : m_instructions(instructions), m_options(options)
    {}

    bool parse(Sequence& result)
    {
        size_t index = 0;
        if (!parseSequence(index, result, {})) {
            return false;
        }
        return index == m_instructions.size();
    }

private:
    bool parseSequence(
        size_t& index,
        Sequence& result,
        const std::vector<BranchPredicate>& path
    )
    {
        while (index < m_instructions.size()) {
            const ControlToken token = controlToken(m_instructions[index]);
            if (token == ControlToken::ELSE || token == ControlToken::ENDIF) {
                return true;
            }

            if (token != ControlToken::IF && token != ControlToken::NOTIF) {
                Node atomNode;
                atomNode.kind = Node::Kind::ATOM;
                atomNode.atom =
                    makeAtom(m_instructions[index], index, path, m_options);
                result.push_back(std::move(atomNode));
                ++index;
                continue;
            }

            Node ifNode;
            ifNode.kind = Node::Kind::IF_REGION;
            const size_t openerPC = index;
            ifNode.opener =
                makeAtom(m_instructions[index], index, path, m_options);
            ++index;

            auto thenPath = path;
            thenPath.push_back(
                BranchPredicate{openerPC, BranchArm::THEN}
            );
            if (!parseSequence(index, ifNode.thenBody, thenPath) ||
                index >= m_instructions.size()) {
                return false;
            }

            ControlToken stop = controlToken(m_instructions[index]);
            if (stop == ControlToken::ELSE) {
                ifNode.elseMarker = makeAtom(
                    m_instructions[index], index, path, m_options
                );
                ++index;
                auto elsePath = path;
                elsePath.push_back(
                    BranchPredicate{openerPC, BranchArm::ELSE}
                );
                if (!parseSequence(index, ifNode.elseBody, elsePath) ||
                    index >= m_instructions.size()) {
                    return false;
                }
                stop = controlToken(m_instructions[index]);
            }

            if (stop != ControlToken::ENDIF) {
                return false;
            }
            ifNode.endMarker = makeAtom(
                m_instructions[index], index, path, m_options
            );
            ++index;
            result.push_back(std::move(ifNode));
        }
        return true;
    }

    const std::vector<std::string>& m_instructions;
    const StructuredIfTailOptions& m_options;
};

bool equivalent(const Atom& lhs, const Atom& rhs)
{
    if (isPureHex(lhs.encoding) && isPureHex(rhs.encoding)) {
        return normalizeHex(lhs.encoding) == normalizeHex(rhs.encoding);
    }
    return lhs.encoding == rhs.encoding;
}

bool equivalent(const Node& lhs, const Node& rhs);

bool equivalent(const Sequence& lhs, const Sequence& rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t index = 0; index < lhs.size(); ++index) {
        if (!equivalent(lhs[index], rhs[index])) {
            return false;
        }
    }
    return true;
}

bool equivalent(const Node& lhs, const Node& rhs)
{
    if (lhs.kind != rhs.kind) {
        return false;
    }
    if (lhs.kind == Node::Kind::ATOM) {
        return equivalent(lhs.atom, rhs.atom);
    }

    return equivalent(lhs.opener, rhs.opener) &&
           lhs.elseMarker.has_value() == rhs.elseMarker.has_value() &&
           (!lhs.elseMarker.has_value() ||
            equivalent(lhs.elseMarker.value(), rhs.elseMarker.value())) &&
           equivalent(lhs.endMarker, rhs.endMarker) &&
           equivalent(lhs.thenBody, rhs.thenBody) &&
           equivalent(lhs.elseBody, rhs.elseBody);
}

bool extractionSafe(const Node& node)
{
    if (node.kind == Node::Kind::ATOM) {
        return node.atom.extractionSafe;
    }

    if (!node.opener.extractionSafe || !node.endMarker.extractionSafe ||
        (node.elseMarker.has_value() &&
         !node.elseMarker->extractionSafe)) {
        return false;
    }
    return std::all_of(
               node.thenBody.begin(),
               node.thenBody.end(),
               [](const Node& child) { return extractionSafe(child); }
           ) &&
           std::all_of(
               node.elseBody.begin(),
               node.elseBody.end(),
               [](const Node& child) { return extractionSafe(child); }
           );
}

bool extractionSafe(const Sequence& sequence)
{
    return std::all_of(
        sequence.begin(), sequence.end(), [](const Node& node) {
            return extractionSafe(node);
        }
    );
}

bool branchPredicateLess(
    const BranchPredicate& lhs,
    const BranchPredicate& rhs
)
{
    if (lhs.ifPC != rhs.ifPC) {
        return lhs.ifPC < rhs.ifPC;
    }
    return static_cast<unsigned int>(lhs.arm) <
           static_cast<unsigned int>(rhs.arm);
}

bool originLess(
    const InstructionOriginRef& lhs,
    const InstructionOriginRef& rhs
)
{
    if (lhs.oldPC != rhs.oldPC) {
        return lhs.oldPC < rhs.oldPC;
    }
    return std::lexicographical_compare(
        lhs.path.begin(), lhs.path.end(),
        rhs.path.begin(), rhs.path.end(),
        branchPredicateLess
    );
}

void mergeOrigins(Atom& kept, const Atom& duplicate)
{
    kept.origins.insert(
        kept.origins.end(), duplicate.origins.begin(), duplicate.origins.end()
    );
    std::sort(kept.origins.begin(), kept.origins.end(), originLess);
    kept.origins.erase(
        std::unique(kept.origins.begin(), kept.origins.end()),
        kept.origins.end()
    );
}

void mergeOrigins(Node& kept, const Node& duplicate);

void mergeOrigins(Sequence& kept, const Sequence& duplicate)
{
    for (size_t index = 0; index < kept.size(); ++index) {
        mergeOrigins(kept[index], duplicate[index]);
    }
}

void mergeOrigins(Node& kept, const Node& duplicate)
{
    if (kept.kind == Node::Kind::ATOM) {
        mergeOrigins(kept.atom, duplicate.atom);
        return;
    }

    mergeOrigins(kept.opener, duplicate.opener);
    mergeOrigins(kept.thenBody, duplicate.thenBody);
    if (kept.elseMarker.has_value()) {
        mergeOrigins(kept.elseMarker.value(), duplicate.elseMarker.value());
    }
    mergeOrigins(kept.elseBody, duplicate.elseBody);
    mergeOrigins(kept.endMarker, duplicate.endMarker);
}

Sequence optimizeSequence(
    Sequence input,
    StructuredIfTailRewriteResult& result
)
{
    Sequence output;
    output.reserve(input.size());

    for (Node& node : input) {
        if (node.kind == Node::Kind::ATOM) {
            output.push_back(std::move(node));
            continue;
        }

        node.thenBody = optimizeSequence(std::move(node.thenBody), result);
        node.elseBody = optimizeSequence(std::move(node.elseBody), result);

        Sequence commonTail;
        if (node.elseMarker.has_value()) {
            size_t commonCount = 0;
            const bool branchesAreSafe = extractionSafe(node.thenBody) &&
                                         extractionSafe(node.elseBody);
            if (!branchesAreSafe) {
                if (!node.thenBody.empty() && !node.elseBody.empty() &&
                    equivalent(node.thenBody.back(), node.elseBody.back())) {
                    ++result.skippedUnsafeTailCount;
                }
            } else {
                while (commonCount < node.thenBody.size() &&
                       commonCount < node.elseBody.size()) {
                    const Node& thenCandidate =
                        node.thenBody[node.thenBody.size() - 1 - commonCount];
                    const Node& elseCandidate =
                        node.elseBody[node.elseBody.size() - 1 - commonCount];
                    if (!equivalent(thenCandidate, elseCandidate)) {
                        break;
                    }
                    ++commonCount;
                }
            }

            if (commonCount != 0) {
                const size_t thenStart = node.thenBody.size() - commonCount;
                const size_t elseStart = node.elseBody.size() - commonCount;
                commonTail.reserve(commonCount);
                for (size_t offset = 0; offset < commonCount; ++offset) {
                    Node combined = std::move(node.thenBody[thenStart + offset]);
                    mergeOrigins(combined, node.elseBody[elseStart + offset]);
                    commonTail.push_back(std::move(combined));
                }
                node.thenBody.resize(thenStart);
                node.elseBody.resize(elseStart);
                result.changed = true;
                ++result.mergedIfCount;
            }
        }

        output.push_back(std::move(node));
        output.insert(
            output.end(),
            std::make_move_iterator(commonTail.begin()),
            std::make_move_iterator(commonTail.end())
        );
    }

    return output;
}

void flattenAtom(
    const Atom& atom,
    StructuredIfTailRewriteResult& result
)
{
    const size_t newPC = result.instructions.size();
    result.instructions.push_back(atom.encoding);
    result.rewritePlan.newToOld.push_back(atom.origins);
    for (const InstructionOriginRef& origin : atom.origins) {
        if (origin.oldPC < result.rewritePlan.oldToNew.size()) {
            result.rewritePlan.oldToNew[origin.oldPC] = newPC;
        }
    }
}

void flatten(const Sequence& sequence, StructuredIfTailRewriteResult& result)
{
    for (const Node& node : sequence) {
        if (node.kind == Node::Kind::ATOM) {
            flattenAtom(node.atom, result);
            continue;
        }

        flattenAtom(node.opener, result);
        flatten(node.thenBody, result);
        if (node.elseMarker.has_value()) {
            flattenAtom(node.elseMarker.value(), result);
            flatten(node.elseBody, result);
        }
        flattenAtom(node.endMarker, result);
    }
}

bool validBranchArm(BranchArm arm)
{
    switch (arm) {
    case BranchArm::THEN:
    case BranchArm::ELSE:
        return true;
    }
    return false;
}

bool normalizeBranchPaths(
    InstructionRewritePlan& plan,
    const std::vector<std::string>& oldInstructions,
    const std::vector<std::string>& newInstructions
)
{
    for (auto& origins : plan.newToOld) {
        for (InstructionOriginRef& origin : origins) {
            for (BranchPredicate& predicate : origin.path) {
                if (!validBranchArm(predicate.arm) ||
                    predicate.ifPC >= oldInstructions.size() ||
                    predicate.ifPC >= plan.oldToNew.size()) {
                    return false;
                }

                const ControlToken oldToken =
                    controlToken(oldInstructions[predicate.ifPC]);
                if (oldToken != ControlToken::IF &&
                    oldToken != ControlToken::NOTIF) {
                    return false;
                }

                const size_t mappedIfPC = plan.oldToNew[predicate.ifPC];
                if (mappedIfPC >= newInstructions.size()) {
                    return false;
                }
                const ControlToken newToken =
                    controlToken(newInstructions[mappedIfPC]);
                if (newToken != oldToken) {
                    return false;
                }
                predicate.ifPC = mappedIfPC;
            }

            std::unordered_set<ControlRegionId> regions;
            for (const BranchPredicate& predicate : origin.path) {
                if (!regions.insert(predicate.ifPC).second) {
                    return false;
                }
            }
        }

        std::sort(origins.begin(), origins.end(), originLess);
    }
    return true;
}

void populateCompatibilityViews(StructuredIfTailRewriteResult& result)
{
    result.oldToNew = result.rewritePlan.oldToNew;
    result.newToOldOrigins.clear();
    result.newToOldOrigins.reserve(result.rewritePlan.newToOld.size());
    for (const auto& originRefs : result.rewritePlan.newToOld) {
        std::vector<size_t> oldPCs;
        oldPCs.reserve(originRefs.size());
        for (const InstructionOriginRef& origin : originRefs) {
            oldPCs.push_back(origin.oldPC);
        }
        std::sort(oldPCs.begin(), oldPCs.end());
        oldPCs.erase(
            std::unique(oldPCs.begin(), oldPCs.end()), oldPCs.end()
        );
        result.newToOldOrigins.push_back(std::move(oldPCs));
    }
}

StructuredIfTailRewriteResult unchangedResult(
    const std::vector<std::string>& instructions,
    bool structurallyValid,
    bool rewritePlanValid = true
)
{
    StructuredIfTailRewriteResult result;
    result.instructions = instructions;
    result.structurallyValid = structurallyValid;
    result.rewritePlanValid = rewritePlanValid;
    result.rewritePlan.oldToNew.resize(instructions.size());
    result.rewritePlan.newToOld.resize(instructions.size());
    for (size_t pc = 0; pc < instructions.size(); ++pc) {
        result.rewritePlan.oldToNew[pc] = pc;
        result.rewritePlan.newToOld[pc].push_back(
            InstructionOriginRef{pc, {}}
        );
    }
    populateCompatibilityViews(result);
    return result;
}

} // namespace

bool validateInstructionRewritePlan(
    const InstructionRewritePlan& plan,
    const std::vector<std::string>& oldInstructions,
    const std::vector<std::string>& newInstructions
)
{
    if (plan.oldToNew.size() != oldInstructions.size() ||
        plan.newToOld.size() != newInstructions.size()) {
        return false;
    }

    std::vector<size_t> oldOriginCounts(oldInstructions.size(), 0);
    for (size_t newPC = 0; newPC < plan.newToOld.size(); ++newPC) {
        const auto& origins = plan.newToOld[newPC];
        if (origins.empty()) {
            return false;
        }

        for (size_t index = 0; index < origins.size(); ++index) {
            const InstructionOriginRef& origin = origins[index];
            if (origin.oldPC >= oldInstructions.size() ||
                plan.oldToNew[origin.oldPC] != newPC) {
                return false;
            }
            ++oldOriginCounts[origin.oldPC];

            for (const BranchPredicate& predicate : origin.path) {
                if (!validBranchArm(predicate.arm) ||
                    predicate.ifPC >= newInstructions.size() ||
                    predicate.ifPC >= newPC) {
                    return false;
                }
                const ControlToken token =
                    controlToken(newInstructions[predicate.ifPC]);
                if (token != ControlToken::IF &&
                    token != ControlToken::NOTIF) {
                    return false;
                }
            }
            std::unordered_set<ControlRegionId> regions;
            for (const BranchPredicate& predicate : origin.path) {
                if (!regions.insert(predicate.ifPC).second) {
                    return false;
                }
            }
        }
    }

    for (size_t oldPC = 0; oldPC < oldInstructions.size(); ++oldPC) {
        if (oldOriginCounts[oldPC] == 0) {
            if (plan.oldToNew[oldPC] != kInvalidPC) {
                return false;
            }
            continue;
        }
        if (oldOriginCounts[oldPC] != 1 ||
            plan.oldToNew[oldPC] >= newInstructions.size()) {
            return false;
        }
    }
    return true;
}

StructuredIfTailRewriteResult StructuredIfTailOptimizer::optimize(
    const std::vector<std::string>& instructions,
    const StructuredIfTailOptions& options
)
{
    Sequence parsed;
    Parser parser(instructions, options);
    if (!parser.parse(parsed)) {
        return unchangedResult(instructions, false);
    }

    StructuredIfTailRewriteResult result;
    result.rewritePlan.oldToNew.assign(instructions.size(), kInvalidPC);
    Sequence optimized = optimizeSequence(std::move(parsed), result);
    flatten(optimized, result);

    if (!normalizeBranchPaths(
            result.rewritePlan, instructions, result.instructions
        ) ||
        !validateInstructionRewritePlan(
            result.rewritePlan, instructions, result.instructions
        )) {
        return unchangedResult(instructions, true, false);
    }
    populateCompatibilityViews(result);

    result.removedInstructionCount =
        instructions.size() - result.instructions.size();
    return result;
}

} // namespace tbc

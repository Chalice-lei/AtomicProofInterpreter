#ifndef BYTECODE_ARTIFACT_REWRITER_H
#define BYTECODE_ARTIFACT_REWRITER_H

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bytecode_ir.h"
#include "legacy_bytecode_adapter.h"

namespace tbc
{

// Keeps the typed artifact synchronized while a legacy compatibility pass is
// being migrated.  The string vector is only a temporary algorithm view; the
// returned artifact retains stable instruction origins and typed regions.
class BytecodeArtifactRewriter final
{
public:
    static std::vector<std::vector<size_t>> reverseMapping(
        const std::vector<size_t>& oldToNew,
        size_t newInstructionCount
    )
    {
        std::vector<std::vector<size_t>> result(newInstructionCount);
        for (size_t oldPC = 0; oldPC < oldToNew.size(); ++oldPC) {
            const size_t newPC = oldToNew[oldPC];
            if (newPC == std::numeric_limits<size_t>::max()) {
                continue;
            }
            if (newPC >= newInstructionCount) {
                return {};
            }
            result[newPC].push_back(oldPC);
        }
        return result;
    }

    static std::optional<BytecodeArtifact> rewrite(
        const BytecodeArtifact& oldArtifact,
        const std::vector<std::string>& newInstructions,
        const std::vector<std::vector<size_t>>& newToOld,
        std::string* error = nullptr
    )
    {
        if (newToOld.size() != newInstructions.size()) {
            setError(error, "rewrite provenance size does not match output");
            return std::nullopt;
        }

        LegacyBytecode legacy{newInstructions, oldArtifact.unlockingScripts};
        BytecodeArtifact rewritten = LegacyBytecodeAdapter::import(legacy);
        rewritten.format = oldArtifact.format;
        rewritten.constructorSchema = oldArtifact.constructorSchema;
        rewritten.layout = oldArtifact.layout;

        std::unordered_map<std::string, std::optional<size_t>> knownSizes;
        InstructionId nextSyntheticInstruction = 1;
        OriginId nextSyntheticOrigin = 1;
        bool syntheticInstructionAvailable = true;
        bool syntheticOriginAvailable = true;
        std::unordered_set<InstructionId> oldInstructionIds;
        for (const auto& instruction : oldArtifact.lockingScript) {
            if (!oldInstructionIds.insert(instruction.id).second) {
                setError(error, "typed artifact contains duplicate instruction IDs");
                return std::nullopt;
            }
            if (instruction.id ==
                std::numeric_limits<InstructionId>::max()) {
                syntheticInstructionAvailable = false;
            } else if (syntheticInstructionAvailable) {
                nextSyntheticInstruction = std::max(
                    nextSyntheticInstruction, instruction.id + 1
                );
            }
            const std::vector<OriginId> effectiveOrigins =
                instruction.origins.empty()
                    ? std::vector<OriginId>{
                          static_cast<OriginId>(instruction.id)}
                    : instruction.origins;
            for (OriginId origin : effectiveOrigins) {
                if (origin == std::numeric_limits<OriginId>::max()) {
                    syntheticOriginAvailable = false;
                } else if (syntheticOriginAvailable) {
                    nextSyntheticOrigin = std::max(
                        nextSyntheticOrigin, origin + 1
                    );
                }
            }
            if (const auto* placeholder =
                    std::get_if<PlaceholderPushInstruction>(
                        &instruction.body
                    )) {
                const auto [known, inserted] = knownSizes.emplace(
                    placeholder->label,
                    placeholder->expectedPayloadSize
                );
                if (!inserted) {
                    if (known->second.has_value() &&
                        placeholder->expectedPayloadSize.has_value() &&
                        known->second !=
                            placeholder->expectedPayloadSize) {
                        setError(
                            error,
                            "typed artifact contains conflicting placeholder sizes"
                        );
                        return std::nullopt;
                    }
                    if (!known->second.has_value()) {
                        known->second = placeholder->expectedPayloadSize;
                    }
                }
            }
        }

        std::vector<bool> oldInstructionClaimed(
            oldArtifact.lockingScript.size(), false
        );
        std::unordered_set<InstructionId> outputInstructionIds;
        for (size_t newPC = 0; newPC < rewritten.lockingScript.size();
             ++newPC) {
            auto& output = rewritten.lockingScript[newPC];
            std::set<OriginId> origins;
            std::vector<size_t> sourcePCs = newToOld[newPC];
            std::sort(sourcePCs.begin(), sourcePCs.end());
            if (std::adjacent_find(sourcePCs.begin(), sourcePCs.end()) !=
                sourcePCs.end()) {
                setError(error, "rewrite provenance repeats an input PC");
                return std::nullopt;
            }

            const BytecodeInstruction* firstInput = nullptr;
            bool allBodiesEqual = true;
            bool encodingMatchesSource = false;
            std::optional<ScriptRegion> sourceRegion;
            for (size_t oldPC : sourcePCs) {
                if (oldPC >= oldArtifact.lockingScript.size()) {
                    setError(error, "rewrite provenance references invalid PC");
                    return std::nullopt;
                }
                if (oldInstructionClaimed[oldPC]) {
                    setError(
                        error,
                        "rewrite provenance maps one input PC to multiple outputs"
                    );
                    return std::nullopt;
                }
                oldInstructionClaimed[oldPC] = true;
                const auto& input = oldArtifact.lockingScript[oldPC];
                if (!firstInput) {
                    firstInput = &input;
                    sourceRegion = input.region;
                } else {
                    allBodiesEqual = allBodiesEqual &&
                                     input.body == firstInput->body;
                    if (input.region != *sourceRegion) {
                        setError(
                            error,
                            "rewrite merges instructions from different script regions"
                        );
                        return std::nullopt;
                    }
                }
                encodingMatchesSource = encodingMatchesSource ||
                    LegacyBytecodeAdapter::serializeInstructionPreserving(
                        input
                    ) == newInstructions[newPC];
                if (input.origins.empty()) {
                    origins.insert(static_cast<OriginId>(input.id));
                } else {
                    origins.insert(input.origins.begin(), input.origins.end());
                }
            }

            if (firstInput) {
                output.id = firstInput->id;
                output.region = *sourceRegion;

                // A one-to-one keep or a structured merge of identical typed
                // atoms must retain its semantic body. Re-importing the legacy
                // spelling would turn an immutable PushData into RawSuffix and
                // make CanonicalV2 output depend on whether an unrelated
                // peephole happened elsewhere in the script.
                if (allBodiesEqual && encodingMatchesSource) {
                    output.body = firstInput->body;
                }
            } else {
                // Empty reverse provenance is reserved for a genuinely
                // synthetic layout atom (for example Finalize padding). IDs
                // and origins have independent namespaces and allocators.
                if (!syntheticInstructionAvailable ||
                    !syntheticOriginAvailable) {
                    setError(error, "synthetic bytecode identity overflow");
                    return std::nullopt;
                }
                output.id = nextSyntheticInstruction;
                origins.insert(nextSyntheticOrigin);
                if (nextSyntheticInstruction ==
                    std::numeric_limits<InstructionId>::max()) {
                    syntheticInstructionAvailable = false;
                } else {
                    ++nextSyntheticInstruction;
                }
                if (nextSyntheticOrigin ==
                    std::numeric_limits<OriginId>::max()) {
                    syntheticOriginAvailable = false;
                } else {
                    ++nextSyntheticOrigin;
                }
            }
            if (!outputInstructionIds.insert(output.id).second) {
                setError(error, "rewrite produced duplicate instruction IDs");
                return std::nullopt;
            }
            output.origins.assign(origins.begin(), origins.end());
            output.legacyEncoding = newInstructions[newPC];

            if (auto* placeholder = std::get_if<PlaceholderPushInstruction>(
                    &output.body
                )) {
                const auto known = knownSizes.find(placeholder->label);
                if (known != knownSizes.end()) {
                    placeholder->expectedPayloadSize = known->second;
                }
            }
        }

        setError(error, {});
        return rewritten;
    }

private:
    static void setError(std::string* target, std::string value)
    {
        if (target) {
            *target = std::move(value);
        }
    }
};

} // namespace tbc

#endif // BYTECODE_ARTIFACT_REWRITER_H

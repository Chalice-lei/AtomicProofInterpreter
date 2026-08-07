#ifndef AST_TO_BYTECODE_CONVERTER_H
#define AST_TO_BYTECODE_CONVERTER_H

#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

#include "../ast/ast.h"
#include "../bytecode/bytecode_generator.h"
#include "../bytecode/script_codec.h"
#include "../log/logger.h"
#include "ast_to_bytecode_visitor.h"
#include "ast_lifetime_planner.h"
#include "collect_symbols_visitor.h"
#include "constant_folder.h"
#include "global_constant_resolver.h"
#include "pre_analysis_visitor.h"
#include "static_info_visitor.h"

#ifdef ENABLE_DEBUGGER
#include "../debugger/info/debug_info_generator.h"
#endif

class ASTToBytecodeConverter
{
public:
    explicit ASTToBytecodeConverter(const std::string& sourceFile = "")
        : m_sourceFile(sourceFile)
    {}
    ~ASTToBytecodeConverter() = default;

    std::pair<std::vector<std::string>,
              std::unordered_map<std::string, std::string>>
    convert(ContractNode& ast, bool allowSubscopeAltstack = false)
    {
        // 公共 frontend 会先解析一次；直接 bytecode pipeline 从
        // ParserPass 进入此处，因此 converter 保留幂等的兜底解析。
        GlobalConstantResolver globalResolver;
        auto globalResult = globalResolver.resolve(ast);
        if (!globalResult.success()) {
            LOG_ERROR(
                "Global constant resolution found " +
                std::to_string(globalResult.errors.size()) + " error(s)"
            );
            throw std::runtime_error("Global constant resolution failed");
        }
        LOG_DEBUG(
            "Resolved " +
            std::to_string(globalResult.resolvedReferences) +
            " global constant reference(s)"
        );

        CollectSymbolsVisitor symbolChecker;
        symbolChecker.checkUniqueness(ast);

        StaticInfoVisitor staticInfoVisitor;
        staticInfoVisitor.visit(ast);
        m_abiJson = staticInfoVisitor.getAbiJson();
        m_constructorParamsJson = staticInfoVisitor.getConstructorParamsJson();
        m_structJson = staticInfoVisitor.getStructJson();
        m_allFunctionsJson = staticInfoVisitor.getAllFunctionsJson();

        LOG_INFO("Starting pre-analysis...");
        PreAnalysisVisitor preAnalyzer;
        preAnalyzer.setAllowSubscopeAltstack(allowSubscopeAltstack);
        bool analysisSuccess = preAnalyzer.analyze(ast);

        const auto& errors = preAnalyzer.getErrors();
        const auto& warnings = preAnalyzer.getWarnings();

        if (!errors.empty()) {
            LOG_ERROR("Pre-analysis found " + std::to_string(errors.size()) +
                      " errors:");
            for (const auto& error : errors) {
                LOG_ERROR("  - " + error);
            }
        }

        if (!warnings.empty()) {
            LOG_WARNING("Pre-analysis found " +
                        std::to_string(warnings.size()) + " warnings:");
            for (const auto& warning : warnings) {
                LOG_WARNING("  - [pre_analysis_visitor] " + warning);
            }
        }

        if (!analysisSuccess) {
            LOG_ERROR("Pre-analysis failed, compilation terminated");
            throw std::runtime_error(
                "Pre-analysis failed, ownership issues detected");
        }

        LOG_INFO("Pre-analysis completed, no ownership issues found");

        // 常量折叠: 把纯字面量表达式重写为 LiteralNode, 使下游 peephole
        // (例如 x+1 -> OP_1ADD) 能看到折叠后的 Literal 子节点.
        ConstantFolder folder;
        auto foldResult = folder.fold(ast);
        if (foldResult.hasError) {
            LOG_ERROR(
                "Constant folding found " +
                std::to_string(foldResult.errors.size()) + " error(s):"
            );
            for (const auto& e : foldResult.errors) {
                LOG_ERROR("  - " + e);
            }
            throw std::runtime_error("Constant folding failed");
        }

        const auto lifetimePlans =
            apc::compiler::planContractLifetimesAfterConstantFolding(ast);

        struct GeneratedCandidate
        {
            tbc::BytecodeGenerator generator;
            std::unordered_map<std::string, size_t> selfPlaceholderLengths;
            size_t cleanupCount{0};
#ifdef ENABLE_DEBUGGER
            std::shared_ptr<apc_debug::DebugInfoGenerator> debugInfo;
#endif
        };

        auto generate = [&](const apc::compiler::ContractLifetimePlans* plans) {
            GeneratedCandidate generated;
            ASTToBytecodeVisitor visitor(generated.generator, m_sourceFile);
            visitor.setStructDefinitions(symbolChecker.getStructs());
            visitor.setLifetimePlans(plans);
            visitor.visit(ast);
            generated.selfPlaceholderLengths =
                visitor.getSelfPlaceholderLengths();
            generated.cleanupCount = visitor.lifetimeCleanupCount();

#ifdef ENABLE_DEBUGGER
            generated.debugInfo = visitor.getDebugInfoGenerator();
            // The callback captures visitor and is valid only while it emits.
            generated.generator.setDebugInfoCallback({});
#endif

            return generated;
        };

        GeneratedCandidate optimized = generate(&lifetimePlans);
        const auto optimizedLegacy = optimized.generator.instructions();
        std::optional<GeneratedCandidate> baseline;
        std::optional<std::pair<
            std::vector<std::string>,
            std::unordered_map<std::string, std::string>>> baselineLegacy;
        bool useOptimized = false;
        if (optimized.cleanupCount != 0) {
            baseline.emplace(generate(nullptr));
            baselineLegacy = baseline->generator.instructions();
            useOptimized = isStrictlySmallerComparableScript(
                *baselineLegacy, optimizedLegacy
            );
        }

        const size_t baselineKnownBytes = baselineLegacy.has_value()
            ? comparableKnownByteSize(baselineLegacy->first).value_or(0)
            : comparableKnownByteSize(optimizedLegacy.first).value_or(0);
        const size_t candidateKnownBytes =
            comparableKnownByteSize(optimizedLegacy.first).value_or(0);
        LOG_DEBUG(
            "Lifetime cleanup candidate: cleanups=",
            optimized.cleanupCount,
            ", baseline_known_bytes=",
            baselineKnownBytes,
            ", candidate_known_bytes=",
            candidateKnownBytes,
            ", accepted=",
            useOptimized ? "true" : "false"
        );

        GeneratedCandidate& selected =
            useOptimized || !baseline.has_value() ? optimized : *baseline;
        m_lifetimeOptimizationApplied = useOptimized;
        m_lifetimeCleanupCount = useOptimized ? optimized.cleanupCount : 0;
        if (useOptimized) {
            LOG_INFO(
                "Applied ",
                optimized.cleanupCount,
                " lifetime cleanup(s): locking script bytes ",
                baselineKnownBytes,
                " -> ",
                candidateKnownBytes
            );
        }

        m_generator = std::move(selected.generator);
        m_selfPlaceholderLengths =
            std::move(selected.selfPlaceholderLengths);

#ifdef ENABLE_DEBUGGER
        m_debugInfoGen = std::move(selected.debugInfo);
#endif

        LOG_DEBUG("Generated Bitcoin Script:\n", m_generator.str());
        return m_generator.instructions();
    }

    nlohmann::ordered_json getConstructorParamsJson() const
    {
        return m_constructorParamsJson;
    }

    nlohmann::ordered_json getAbiJson() const
    {
        return m_abiJson;
    };

    nlohmann::ordered_json getStructJson() const
    {
        return m_structJson;
    };
    
    nlohmann::ordered_json getAllFunctionsJson() const
    {
        return m_allFunctionsJson;
    };

    const std::unordered_map<std::string, size_t>& getSelfPlaceholderLengths()
        const
    {
        return m_selfPlaceholderLengths;
    }

    bool lifetimeOptimizationApplied() const noexcept
    {
        return m_lifetimeOptimizationApplied;
    }

    size_t appliedLifetimeCleanupCount() const noexcept
    {
        return m_lifetimeCleanupCount;
    }

    tbc::BytecodeArtifact getBytecodeArtifact() const
    {
        auto artifact = m_generator.artifact();
        artifact.layout.executableAlignment = 64;

        for (auto& instruction : artifact.lockingScript) {
            auto* placeholder = std::get_if<tbc::PlaceholderPushInstruction>(
                &instruction.body
            );
            if (!placeholder) {
                continue;
            }

            artifact.layout.requiresMaterialization = true;
            auto exact = m_selfPlaceholderLengths.find(placeholder->label);
            if (exact != m_selfPlaceholderLengths.end()) {
                placeholder->expectedPayloadSize = exact->second;
                continue;
            }

            // Legacy templates append the fixed byte count to self labels.
            // Field names may themselves end in digits, so match the longest
            // declared label whose decimal size suffix consumes the rest.
            const std::string* matchedBase = nullptr;
            size_t matchedLength = 0;
            for (const auto& [base, payloadLength] :
                 m_selfPlaceholderLengths) {
                const std::string suffix = std::to_string(payloadLength);
                if (placeholder->label.size() !=
                        base.size() + suffix.size() ||
                    placeholder->label.compare(0, base.size(), base) != 0 ||
                    placeholder->label.compare(
                        base.size(), suffix.size(), suffix
                    ) != 0) {
                    continue;
                }
                if (!matchedBase || base.size() > matchedBase->size()) {
                    matchedBase = &base;
                    matchedLength = payloadLength;
                }
            }
            if (matchedBase) {
                // The decimal suffix is a LegacyV1 spelling convention.
                // Canonical typed relocations carry the size separately, so
                // executable and immutable-suffix references must share the
                // same base label and therefore one deployment binding.
                placeholder->label = *matchedBase;
                placeholder->expectedPayloadSize = matchedLength;
            }
        }
        return artifact;
    }
    
#ifdef ENABLE_DEBUGGER
    std::shared_ptr<apc_debug::DebugInfoGenerator> getDebugInfoGenerator() const {
        return m_debugInfoGen;
    }
#endif

private:
    static std::optional<size_t> comparableKnownByteSize(
        const std::vector<std::string>& instructions,
        std::vector<std::string>* symbolicAtoms = nullptr
    )
    {
        size_t total = 0;
        for (const auto& instruction : instructions) {
            tbc::ScriptCodecError error = tbc::ScriptCodecError::None;
            const auto bytes =
                tbc::ScriptCodec::hexToBytes(instruction, &error);
            if (!bytes.has_value()) {
                if (symbolicAtoms) {
                    symbolicAtoms->push_back(instruction);
                }
                continue;
            }
            if (bytes->size() >
                std::numeric_limits<size_t>::max() - total) {
                return std::nullopt;
            }
            total += bytes->size();
        }
        return total;
    }

    static bool isStrictlySmallerComparableScript(
        const std::pair<
            std::vector<std::string>,
            std::unordered_map<std::string, std::string>>& baseline,
        const std::pair<
            std::vector<std::string>,
            std::unordered_map<std::string, std::string>>& candidate
    )
    {
        if (baseline.second != candidate.second) {
            return false;
        }
        std::vector<std::string> baselineSymbols;
        std::vector<std::string> candidateSymbols;
        const auto baselineBytes = comparableKnownByteSize(
            baseline.first, &baselineSymbols
        );
        const auto candidateBytes = comparableKnownByteSize(
            candidate.first, &candidateSymbols
        );
        return baselineBytes.has_value() && candidateBytes.has_value() &&
               baselineSymbols == candidateSymbols &&
               *candidateBytes < *baselineBytes;
    }

    tbc::BytecodeGenerator m_generator;
    std::string m_sourceFile;
    nlohmann::ordered_json m_constructorParamsJson;
    nlohmann::ordered_json m_abiJson;
    nlohmann::ordered_json m_structJson;
    nlohmann::ordered_json m_allFunctionsJson;
    std::unordered_map<std::string, size_t> m_selfPlaceholderLengths;
    bool m_lifetimeOptimizationApplied{false};
    size_t m_lifetimeCleanupCount{0};
    
#ifdef ENABLE_DEBUGGER
    std::shared_ptr<apc_debug::DebugInfoGenerator> m_debugInfoGen;
#endif
};

#endif // AST_TO_BYTECODE_CONVERTER_H

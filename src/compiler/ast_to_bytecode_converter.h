#ifndef AST_TO_BYTECODE_CONVERTER_H
#define AST_TO_BYTECODE_CONVERTER_H

#include "../ast/ast.h"
#include "../bytecode/bytecode_generator.h"
#include "../log/logger.h"
#include "ast_to_bytecode_visitor.h"
#include "collect_symbols_visitor.h"
#include "constant_folder.h"
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

        ASTToBytecodeVisitor bytVisitor(m_generator, m_sourceFile);
        bytVisitor.setStructDefinitions(symbolChecker.getStructs());
        bytVisitor.visit(ast);

#ifdef ENABLE_DEBUGGER
        m_debugInfoGen = bytVisitor.getDebugInfoGenerator();
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
    
#ifdef ENABLE_DEBUGGER
    std::shared_ptr<apc_debug::DebugInfoGenerator> getDebugInfoGenerator() const {
        return m_debugInfoGen;
    }
#endif

private:
    tbc::BytecodeGenerator m_generator;
    std::string m_sourceFile;
    nlohmann::ordered_json m_constructorParamsJson;
    nlohmann::ordered_json m_abiJson;
    nlohmann::ordered_json m_structJson;
    nlohmann::ordered_json m_allFunctionsJson;
    
#ifdef ENABLE_DEBUGGER
    std::shared_ptr<apc_debug::DebugInfoGenerator> m_debugInfoGen;
#endif
};

#endif // AST_TO_BYTECODE_CONVERTER_H

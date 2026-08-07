#ifndef PASS_CONTEXT_KEYS_H
#define PASS_CONTEXT_KEYS_H

#include <string>
#include <unordered_map>
#include <vector>

namespace apc_pipeline
{

using BytecodeOutput =
    std::pair<std::vector<std::string>, std::unordered_map<std::string, std::string>>;

namespace key
{
inline constexpr char kAbi[] = "abi";
inline constexpr char kAllowSubscopeAltstack[] = "allow_subscope_altstack";
inline constexpr char kAllFunctions[] = "all_functions";
inline constexpr char kAst[] = "ast";
inline constexpr char kArtifactFormat[] = "artifact_format";
inline constexpr char kBytecode[] = "bytcode";
inline constexpr char kBytecodeArtifact[] = "bytecode_artifact";
inline constexpr char kCodeFileName[] = "code_file_name";
inline constexpr char kConstructorParams[] = "constructorParams";
inline constexpr char kDebugInfo[] = "debug_info";
inline constexpr char kDebugOutputFile[] = "debug_output_file";
inline constexpr char kEnableDebug[] = "enable_debug";
inline constexpr char kLifetimeCleanupApplied[] = "lifetime_cleanup_applied";
inline constexpr char kLifetimeCleanupCount[] = "lifetime_cleanup_count";
inline constexpr char kSelfPlaceholderLengths[] = "self_placeholder_lengths";
inline constexpr char kSuppressDebugFile[] = "suppress_debug_file";
inline constexpr char kSourceCode[] = "source_code";
inline constexpr char kSourceFilePath[] = "source_file_path";
inline constexpr char kSourceMap[] = "source_map";
inline constexpr char kStructs[] = "structs";
inline constexpr char kTokens[] = "tokens";
inline constexpr char kUnlock[] = "unlock";
} // namespace key

} // namespace apc_pipeline

#endif // PASS_CONTEXT_KEYS_H

#ifndef BYTECODE_PIPELINE_H
#define BYTECODE_PIPELINE_H

#include <string>

#include "../pass/pass_context.h"

namespace apc_compiler
{

struct BytecodePipelineOptions
{
    bool allowSubscopeAltstack = false;
    bool enableDebug = false;
    bool exportResults = true;
    bool suppressDebugFile = false;
    std::string codeFileName;
    std::string debugOutputFile;
};

PassContext runBytecodePipeline(
    const std::string& sourceFile,
    const std::string& sourceCode,
    const BytecodePipelineOptions& options = {}
);

} // namespace apc_compiler

#endif // BYTECODE_PIPELINE_H

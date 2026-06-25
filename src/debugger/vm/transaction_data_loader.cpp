#include "transaction_data_loader.h"

#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "../../interpreter/transaction_context.h"
#include "../../util/string_utils.h"

namespace apc_debug
{
namespace
{

using apc::util::toLower;
using apc::util::trim;

bool parseUint32(const std::string& text, uint32_t& out)
{
    const std::string cleaned = trim(text);
    try {
        size_t consumed = 0;
        const unsigned long value = std::stoul(cleaned, &consumed, 0);
        if (consumed != cleaned.size() || value > UINT32_MAX) {
            return false;
        }
        out = static_cast<uint32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

std::string invalidValueMessage(
    const std::string& sourceLabel,
    const std::string& field,
    const TransactionDataLoadOptions& options
)
{
    if (options.chineseMessages) {
        return sourceLabel + " " + field + " 值无效";
    }
    return sourceLabel + " " + field + " is invalid";
}

std::string invalidHexMessage(
    const std::string& sourceLabel,
    const std::string& field,
    const std::exception& error,
    const TransactionDataLoadOptions& options
)
{
    if (options.chineseMessages) {
        return sourceLabel + " " + field + " 值无效: " + error.what();
    }
    return sourceLabel + " " + field + " is invalid: " + error.what();
}

std::string usually32BytesMessage(
    const std::string& sourceLabel,
    const std::string& field,
    const TransactionDataLoadOptions& options
)
{
    if (options.chineseMessages) {
        return sourceLabel + " " + field + " 通常为 32 字节";
    }
    return sourceLabel + " " + field + " is usually 32 bytes";
}

std::string unknownFieldMessage(
    const std::string& sourceLabel,
    const std::string& rawKey,
    const TransactionDataLoadOptions& options
)
{
    if (options.chineseMessages) {
        return sourceLabel + " 未知键 '" + rawKey + "'，已忽略";
    }
    return sourceLabel + " unknown field '" + rawKey + "' ignored";
}

std::string invalidLineMessage(
    int lineNumber,
    const TransactionDataLoadOptions& options
)
{
    if (options.chineseMessages) {
        return "交易上下文第 " + std::to_string(lineNumber) +
               " 行格式无效，已忽略";
    }
    return "transaction context line " + std::to_string(lineNumber) +
           " has invalid format and was ignored";
}

std::string jsonInvalidAssignmentMessage(
    const std::string& assignment,
    const TransactionDataLoadOptions& options
)
{
    if (options.chineseMessages) {
        return "JSON 交易上下文 BVM 派生字段格式无效，已忽略: " +
               assignment;
    }
    return "invalid JSON transaction BVM assignment ignored: " + assignment;
}

std::string jsonSourceLabel(const TransactionDataLoadOptions& options)
{
    return options.chineseMessages ? "JSON 交易上下文字段"
                                   : "JSON transaction field";
}

std::string lineSourceLabel(
    int lineNumber,
    const TransactionDataLoadOptions& options
)
{
    if (options.chineseMessages) {
        return "交易上下文第 " + std::to_string(lineNumber) + " 行";
    }
    return "transaction context line " + std::to_string(lineNumber);
}

} // namespace

bool applyTransactionDataAssignment(
    TransactionData& txData,
    std::vector<std::string>& warnings,
    std::string& errorMessage,
    const std::string& rawKey,
    const std::string& rawValue,
    const std::string& sourceLabel,
    const TransactionDataLoadOptions& options
)
{
    const std::string key = toLower(trim(rawKey));
    const std::string value = trim(rawValue);

    if (key == "version") {
        uint32_t parsed = 0;
        if (!parseUint32(value, parsed)) {
            errorMessage = invalidValueMessage(sourceLabel, "version", options);
            return false;
        }
        txData.version = parsed;
    } else if (key == "locktime") {
        uint32_t parsed = 0;
        if (!parseUint32(value, parsed)) {
            errorMessage = invalidValueMessage(sourceLabel, "locktime", options);
            return false;
        }
        txData.lockTime = parsed;
    } else if (key == "inputcount") {
        uint32_t parsed = 0;
        if (!parseUint32(value, parsed)) {
            errorMessage =
                invalidValueMessage(sourceLabel, "inputCount", options);
            return false;
        }
        txData.inputCount = parsed;
    } else if (key == "outputcount") {
        uint32_t parsed = 0;
        if (!parseUint32(value, parsed)) {
            errorMessage =
                invalidValueMessage(sourceLabel, "outputCount", options);
            return false;
        }
        txData.outputCount = parsed;
    } else if (key == "inputshash") {
        try {
            txData.inputsHash = StackElement::fromHexLiteral(value).data;
        } catch (const std::exception& e) {
            errorMessage =
                invalidHexMessage(sourceLabel, "inputsHash", e, options);
            return false;
        }
        if (!txData.inputsHash.empty() && txData.inputsHash.size() != 32) {
            warnings.push_back(
                usually32BytesMessage(sourceLabel, "inputsHash", options)
            );
        }
    } else if (key == "unlockinginput") {
        try {
            txData.unlockingInput = StackElement::fromHexLiteral(value).data;
        } catch (const std::exception& e) {
            errorMessage =
                invalidHexMessage(sourceLabel, "unlockingInput", e, options);
            return false;
        }
    } else if (key == "outputshash") {
        try {
            txData.outputsHash = StackElement::fromHexLiteral(value).data;
        } catch (const std::exception& e) {
            errorMessage =
                invalidHexMessage(sourceLabel, "outputsHash", e, options);
            return false;
        }
        if (!txData.outputsHash.empty() && txData.outputsHash.size() != 32) {
            warnings.push_back(
                usually32BytesMessage(sourceLabel, "outputsHash", options)
            );
        }
    } else if (key == "txid") {
        return true;
    } else if (!key.empty()) {
        warnings.push_back(unknownFieldMessage(sourceLabel, rawKey, options));
    }

    return true;
}

bool loadTransactionDataFromFile(
    const std::string& filename,
    TransactionData& txData,
    std::vector<std::string>& warnings,
    std::string& errorMessage,
    const TransactionDataLoadOptions& options
)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        errorMessage = options.chineseMessages
                           ? "无法打开交易上下文文件: " + filename
                           : "failed to open transaction context file: " +
                                 filename;
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();
    const std::string cleaned = trim(content);
    if (cleaned.empty()) {
        return true;
    }

    if (cleaned.front() == '{') {
        try {
            apc_interpreter::RuntimeAssignmentSets assignments =
                apc_interpreter::loadRuntimeAssignmentsFromTxFile(filename);
            warnings.insert(
                warnings.end(),
                assignments.warnings.begin(),
                assignments.warnings.end()
            );

            for (const auto& assignment : assignments.bvmAssignments) {
                const size_t eq = assignment.find('=');
                if (eq == std::string::npos || eq == 0) {
                    warnings.push_back(
                        jsonInvalidAssignmentMessage(assignment, options)
                    );
                    continue;
                }

                if (!applyTransactionDataAssignment(
                        txData,
                        warnings,
                        errorMessage,
                        assignment.substr(0, eq),
                        assignment.substr(eq + 1),
                        jsonSourceLabel(options),
                        options
                    )) {
                    return false;
                }
            }
            return true;
        } catch (const std::exception& e) {
            errorMessage = e.what();
            return false;
        }
    }

    std::istringstream input(content);
    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;

        const size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }

        line = trim(line);
        if (line.empty()) {
            continue;
        }

        size_t delimiter = line.find(':');
        if (delimiter == std::string::npos && options.acceptEqualsDelimiter) {
            delimiter = line.find('=');
        }

        if (delimiter == std::string::npos || delimiter == 0) {
            warnings.push_back(invalidLineMessage(lineNumber, options));
            continue;
        }

        if (!applyTransactionDataAssignment(
                txData,
                warnings,
                errorMessage,
                line.substr(0, delimiter),
                line.substr(delimiter + 1),
                lineSourceLabel(lineNumber, options),
                options
            )) {
            return false;
        }
    }

    return true;
}

} // namespace apc_debug

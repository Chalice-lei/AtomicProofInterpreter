#ifndef TRANSACTION_DATA_LOADER_H
#define TRANSACTION_DATA_LOADER_H

#include <string>
#include <vector>

#include "bvm_simulator.h"

namespace apc_debug
{

struct TransactionDataLoadOptions
{
    bool chineseMessages = true;
    bool acceptEqualsDelimiter = true;
};

bool loadTransactionDataFromFile(
    const std::string& filename,
    TransactionData& txData,
    std::vector<std::string>& warnings,
    std::string& errorMessage,
    const TransactionDataLoadOptions& options = {}
);

bool applyTransactionDataAssignment(
    TransactionData& txData,
    std::vector<std::string>& warnings,
    std::string& errorMessage,
    const std::string& rawKey,
    const std::string& rawValue,
    const std::string& sourceLabel,
    const TransactionDataLoadOptions& options = {}
);

} // namespace apc_debug

#endif // TRANSACTION_DATA_LOADER_H

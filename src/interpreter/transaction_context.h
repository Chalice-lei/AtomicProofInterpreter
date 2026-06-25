#ifndef TRANSACTION_CONTEXT_H
#define TRANSACTION_CONTEXT_H

#include <string>
#include <vector>

namespace apc_interpreter
{

struct RuntimeAssignmentSets
{
    std::vector<std::string> paramAssignments;
    std::vector<std::string> selfAssignments;
    std::vector<std::string> bvmAssignments;
    std::vector<std::string> warnings;
};

RuntimeAssignmentSets loadRuntimeAssignmentsFromTxFile(
    const std::string& filename
);

} // namespace apc_interpreter

#endif // TRANSACTION_CONTEXT_H

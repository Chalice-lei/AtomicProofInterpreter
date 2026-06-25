#ifndef RUNTIME_SELF_TEST_H
#define RUNTIME_SELF_TEST_H

#include <iosfwd>

namespace apc_interpreter
{

bool runRuntimeSelfTest(std::ostream& out, std::ostream& err);

} // namespace apc_interpreter

#endif // RUNTIME_SELF_TEST_H

#ifndef AST_SELF_TEST_H
#define AST_SELF_TEST_H

#include <iosfwd>

namespace apc_interpreter
{

bool runASTSelfTest(std::ostream& out, std::ostream& err);

} // namespace apc_interpreter

#endif // AST_SELF_TEST_H

#ifndef BYT_DEFS_H
#define BYT_DEFS_H
#include <limits>

#ifndef SIZE_MAX
#define SIZE_MAX std::numeric_limits<size_t>::max()
#endif

// TBC 命名空间宏

#define SPACE_TBC_START                                                        \
    namespace tbc                                                              \
    {

#define SPACE_TBC_END }

#endif
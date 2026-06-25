#ifndef TYPE_UTILS_H
#define TYPE_UTILS_H

#include <cstddef>
#include <optional>
#include <string>

namespace apc::util
{

struct FixedArrayType
{
    std::string elementType;
    size_t size = 0;
};

inline std::optional<FixedArrayType> parseFixedArrayType(
    const std::string& typeName
)
{
    const size_t bracket = typeName.find('[');
    if (bracket == std::string::npos || bracket == 0 ||
        typeName.empty() || typeName.back() != ']') {
        return std::nullopt;
    }

    const size_t close = typeName.find(']', bracket);
    if (close != typeName.size() - 1) {
        return std::nullopt;
    }

    const std::string elementType = typeName.substr(0, bracket);
    const std::string sizeText =
        typeName.substr(bracket + 1, close - bracket - 1);
    if (elementType.empty() || sizeText.empty()) {
        return std::nullopt;
    }

    try {
        return FixedArrayType{elementType, std::stoull(sizeText)};
    } catch (...) {
        return std::nullopt;
    }
}

inline bool isFixedArrayType(const std::string& typeName)
{
    return parseFixedArrayType(typeName).has_value();
}

} // namespace apc::util

#endif // TYPE_UTILS_H

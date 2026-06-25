#include "runtime_codec.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

#include "../util/string_utils.h"
#include "runtime_error.h"

namespace apc_interpreter::runtime_codec
{

std::string trim(std::string value)
{
    return apc::util::trim(std::move(value));
}

std::string toLower(std::string value)
{
    return apc::util::toLower(std::move(value));
}

std::vector<uint8_t> serializeScriptNum(int64_t value)
{
    if (value == 0) {
        return {};
    }

    std::vector<uint8_t> result;
    const bool negative = value < 0;
    uint64_t absolute = 0;
    if (value == std::numeric_limits<int64_t>::min()) {
        absolute = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) +
                   1ULL;
    } else {
        absolute = static_cast<uint64_t>(negative ? -value : value);
    }

    while (absolute != 0) {
        result.push_back(static_cast<uint8_t>(absolute & 0xff));
        absolute >>= 8;
    }

    if ((result.back() & 0x80) != 0) {
        result.push_back(negative ? 0x80 : 0x00);
    } else if (negative) {
        result.back() |= 0x80;
    }

    return result;
}

int64_t deserializeScriptNum(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty()) {
        return 0;
    }

    if (bytes.size() > 8) {
        throw RuntimeError(
            RuntimeErrorKind::TypeMismatch,
            "script number is wider than int64"
        );
    }

    uint64_t value = 0;
    for (size_t i = 0; i < bytes.size(); ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }

    const bool negative = (bytes.back() & 0x80) != 0;
    if (negative) {
        const uint64_t signMask =
            static_cast<uint64_t>(0x80) << (8 * (bytes.size() - 1));
        value &= ~signMask;
        if (value == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) +
                         1ULL) {
            return std::numeric_limits<int64_t>::min();
        }
        if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            throw RuntimeError(
                RuntimeErrorKind::TypeMismatch,
                "negative script number overflows int64"
            );
        }
        return -static_cast<int64_t>(value);
    }

    if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        throw RuntimeError(
            RuntimeErrorKind::TypeMismatch,
            "script number overflows int64"
        );
    }
    return static_cast<int64_t>(value);
}

std::vector<uint8_t> parseHex(std::string hex, const std::string& errorMessage)
{
    if (hex.size() >= 2 && hex[0] == '0' &&
        (hex[1] == 'x' || hex[1] == 'X')) {
        hex = hex.substr(2);
    }

    std::string clean;
    clean.reserve(hex.size());
    for (unsigned char c : hex) {
        if (!std::isspace(c)) {
            clean.push_back(static_cast<char>(c));
        }
    }

    if (clean.empty()) {
        return {};
    }

    for (unsigned char c : clean) {
        if (!std::isxdigit(c)) {
            throw RuntimeError(RuntimeErrorKind::TypeMismatch, errorMessage);
        }
    }

    if (clean.size() % 2 != 0) {
        clean.insert(clean.begin(), '0');
    }

    auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') {
            return static_cast<uint8_t>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<uint8_t>(c - 'a' + 10);
        }
        return static_cast<uint8_t>(c - 'A' + 10);
    };

    std::vector<uint8_t> bytes;
    bytes.reserve(clean.size() / 2);
    for (size_t i = 0; i < clean.size(); i += 2) {
        bytes.push_back(
            static_cast<uint8_t>((nibble(clean[i]) << 4) | nibble(clean[i + 1]))
        );
    }
    return bytes;
}

std::string bytesToHex(const std::vector<uint8_t>& bytes, bool withPrefix)
{
    std::ostringstream oss;
    if (withPrefix) {
        oss << "0x";
    }
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : bytes) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

} // namespace apc_interpreter::runtime_codec

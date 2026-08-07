#ifndef SCRIPT_CODEC_H
#define SCRIPT_CODEC_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "byt_defs.h"

SPACE_TBC_START

// Legacy reproduces the compiler's historical "length prefix + payload"
// representation. Canonical applies Bitcoin Script's minimal-push rules.
enum class PushEncodingPolicy
{
    Legacy,
    Canonical,
};

enum class PushForm
{
    Op0,
    SmallInteger,
    Direct,
    PushData1,
    PushData2,
    PushData4,
};

enum class ScriptCodecError
{
    None,
    EmptyInput,
    InvalidHex,
    OddHexLength,
    NotPush,
    TruncatedLength,
    TruncatedPayload,
    TrailingBytes,
    NonMinimalPush,
    PayloadTooLarge,
};

struct DecodedPush
{
    std::vector<uint8_t> payload;
    PushForm form{PushForm::Op0};
    size_t encodedSize{0};
};

struct PushDecodeResult
{
    std::optional<DecodedPush> value;
    ScriptCodecError error{ScriptCodecError::None};

    [[nodiscard]] bool ok() const noexcept
    {
        return value.has_value() && error == ScriptCodecError::None;
    }

    explicit operator bool() const noexcept
    {
        return ok();
    }
};

// The single codec for concrete Script pushes. It deliberately separates a
// compatibility encoder from the canonical encoder so migrating a caller can
// never change contract bytes implicitly.
class ScriptCodec final
{
public:
    [[nodiscard]] static std::optional<std::string>
    legacyPushPrefixHex(size_t payloadSize)
    {
        if (payloadSize < 76) {
            return byteToHex(static_cast<uint8_t>(payloadSize));
        }
        if (payloadSize <= std::numeric_limits<uint8_t>::max()) {
            return std::string("4c") +
                   byteToHex(static_cast<uint8_t>(payloadSize));
        }
        if (payloadSize <= std::numeric_limits<uint16_t>::max()) {
            return std::string("4d") + littleEndianHex(payloadSize, 2);
        }
        if (payloadSize <= std::numeric_limits<uint32_t>::max()) {
            return std::string("4e") + littleEndianHex(payloadSize, 4);
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<size_t>
    legacySerializedPushSize(size_t payloadSize)
    {
        if (payloadSize > std::numeric_limits<uint32_t>::max()) {
            return std::nullopt;
        }

        const size_t prefixSize = payloadSize < 76    ? 1
                                  : payloadSize <= 255 ? 2
                                  : payloadSize <= 65535 ? 3
                                                         : 5;
        return prefixSize + payloadSize;
    }

    [[nodiscard]] static std::optional<size_t> serializedPushSize(
        std::span<const uint8_t> payload,
        PushEncodingPolicy policy = PushEncodingPolicy::Canonical
    )
    {
        auto encoded = encodePush(payload, policy);
        if (!encoded.has_value()) {
            return std::nullopt;
        }
        return encoded->size() / 2;
    }

    [[nodiscard]] static std::optional<std::string> encodePush(
        std::span<const uint8_t> payload,
        PushEncodingPolicy policy = PushEncodingPolicy::Canonical
    )
    {
        if (payload.size() > std::numeric_limits<uint32_t>::max()) {
            return std::nullopt;
        }

        if (policy == PushEncodingPolicy::Canonical) {
            if (payload.empty()) {
                return std::string("00");
            }
            if (payload.size() == 1) {
                const uint8_t value = payload.front();
                if (value >= 0x01 && value <= 0x10) {
                    return byteToHex(static_cast<uint8_t>(0x50 + value));
                }
                if (value == 0x81) {
                    return std::string("4f");
                }
            }
        }

        auto prefix = legacyPushPrefixHex(payload.size());
        if (!prefix.has_value()) {
            return std::nullopt;
        }

        std::string result = std::move(prefix.value());
        result.reserve(result.size() + payload.size() * 2);
        for (uint8_t byte : payload) {
            result += byteToHex(byte);
        }
        return result;
    }

    [[nodiscard]] static std::optional<std::string> encodePush(
        const std::vector<uint8_t>& payload,
        PushEncodingPolicy policy = PushEncodingPolicy::Canonical
    )
    {
        return encodePush(std::span<const uint8_t>(payload), policy);
    }

    // Decode the first push from a script fragment. encodedSize identifies
    // the exact prefix consumed; trailing instructions are intentionally left
    // for the caller. Structural mode accepts a non-minimal encoding.
    [[nodiscard]] static PushDecodeResult decodePushPrefix(
        std::span<const uint8_t> encoding,
        bool requireCanonical = true
    )
    {
        if (encoding.empty()) {
            return failure(ScriptCodecError::EmptyInput);
        }

        const uint8_t opcode = encoding.front();
        size_t headerSize = 1;
        uint64_t payloadSize = 0;
        PushForm form = PushForm::Op0;
        std::vector<uint8_t> syntheticPayload;

        if (opcode == 0x00) {
            form = PushForm::Op0;
        } else if (opcode >= 0x01 && opcode <= 0x4b) {
            form = PushForm::Direct;
            payloadSize = opcode;
        } else if (opcode == 0x4c) {
            form = PushForm::PushData1;
            if (encoding.size() < 2) {
                return failure(ScriptCodecError::TruncatedLength);
            }
            headerSize = 2;
            payloadSize = encoding[1];
        } else if (opcode == 0x4d) {
            form = PushForm::PushData2;
            if (encoding.size() < 3) {
                return failure(ScriptCodecError::TruncatedLength);
            }
            headerSize = 3;
            payloadSize = static_cast<uint64_t>(encoding[1]) |
                          (static_cast<uint64_t>(encoding[2]) << 8);
        } else if (opcode == 0x4e) {
            form = PushForm::PushData4;
            if (encoding.size() < 5) {
                return failure(ScriptCodecError::TruncatedLength);
            }
            headerSize = 5;
            payloadSize = static_cast<uint64_t>(encoding[1]) |
                          (static_cast<uint64_t>(encoding[2]) << 8) |
                          (static_cast<uint64_t>(encoding[3]) << 16) |
                          (static_cast<uint64_t>(encoding[4]) << 24);
        } else if (opcode == 0x4f) {
            form = PushForm::SmallInteger;
            syntheticPayload.push_back(0x81);
        } else if (opcode >= 0x51 && opcode <= 0x60) {
            form = PushForm::SmallInteger;
            syntheticPayload.push_back(static_cast<uint8_t>(opcode - 0x50));
        } else {
            return failure(ScriptCodecError::NotPush);
        }

        size_t encodedSize = 1;
        if (syntheticPayload.empty()) {
            const uint64_t available = encoding.size() - headerSize;
            if (available < payloadSize) {
                return failure(ScriptCodecError::TruncatedPayload);
            }
            encodedSize = headerSize + static_cast<size_t>(payloadSize);
        }

        DecodedPush decoded;
        decoded.form = form;
        decoded.encodedSize = encodedSize;
        if (!syntheticPayload.empty()) {
            decoded.payload = std::move(syntheticPayload);
        } else if (payloadSize != 0) {
            decoded.payload.assign(
                encoding.begin() + static_cast<std::ptrdiff_t>(headerSize),
                encoding.begin() + static_cast<std::ptrdiff_t>(encodedSize)
            );
        }

        if (requireCanonical) {
            auto canonical = encodePush(
                std::span<const uint8_t>(decoded.payload),
                PushEncodingPolicy::Canonical
            );
            if (!canonical.has_value() ||
                canonical.value() != bytesToHex(encoding.first(encodedSize))) {
                return failure(ScriptCodecError::NonMinimalPush);
            }
        }

        return PushDecodeResult{std::move(decoded), ScriptCodecError::None};
    }

    // Decode exactly one complete push. Structural mode accepts a
    // non-minimal but well-formed push. Canonical mode additionally rejects
    // every encoding which is not byte-for-byte minimal.
    [[nodiscard]] static PushDecodeResult decodePush(
        std::span<const uint8_t> encoding,
        bool requireCanonical = true
    )
    {
        auto decoded = decodePushPrefix(encoding, requireCanonical);
        if (!decoded.ok()) {
            return decoded;
        }
        if (decoded.value->encodedSize != encoding.size()) {
            return failure(ScriptCodecError::TrailingBytes);
        }
        return decoded;
    }

    [[nodiscard]] static PushDecodeResult decodePushHex(
        std::string_view encoding,
        bool requireCanonical = true
    )
    {
        ScriptCodecError parseError = ScriptCodecError::None;
        auto bytes = hexToBytes(encoding, &parseError);
        if (!bytes.has_value()) {
            return failure(parseError);
        }
        return decodePush(std::span<const uint8_t>(bytes.value()),
                          requireCanonical);
    }

    [[nodiscard]] static std::optional<std::vector<uint8_t>> hexToBytes(
        std::string_view hex,
        ScriptCodecError* error = nullptr
    )
    {
        if (hex.size() >= 2 && hex[0] == '0' &&
            (hex[1] == 'x' || hex[1] == 'X')) {
            hex.remove_prefix(2);
        }
        if (hex.empty()) {
            setError(error, ScriptCodecError::EmptyInput);
            return std::nullopt;
        }
        if ((hex.size() % 2) != 0) {
            setError(error, ScriptCodecError::OddHexLength);
            return std::nullopt;
        }

        std::vector<uint8_t> bytes;
        bytes.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2) {
            const int high = hexDigit(hex[i]);
            const int low = hexDigit(hex[i + 1]);
            if (high < 0 || low < 0) {
                setError(error, ScriptCodecError::InvalidHex);
                return std::nullopt;
            }
            bytes.push_back(static_cast<uint8_t>((high << 4) | low));
        }

        setError(error, ScriptCodecError::None);
        return bytes;
    }

    [[nodiscard]] static std::string bytesToHex(
        std::span<const uint8_t> bytes
    )
    {
        std::string result;
        result.reserve(bytes.size() * 2);
        for (uint8_t byte : bytes) {
            result += byteToHex(byte);
        }
        return result;
    }

    [[nodiscard]] static const char* errorName(ScriptCodecError error) noexcept
    {
        switch (error) {
            case ScriptCodecError::None:
                return "none";
            case ScriptCodecError::EmptyInput:
                return "empty input";
            case ScriptCodecError::InvalidHex:
                return "invalid hex";
            case ScriptCodecError::OddHexLength:
                return "odd hex length";
            case ScriptCodecError::NotPush:
                return "not a push instruction";
            case ScriptCodecError::TruncatedLength:
                return "truncated push length";
            case ScriptCodecError::TruncatedPayload:
                return "truncated push payload";
            case ScriptCodecError::TrailingBytes:
                return "trailing bytes";
            case ScriptCodecError::NonMinimalPush:
                return "non-minimal push";
            case ScriptCodecError::PayloadTooLarge:
                return "payload too large";
        }
        return "unknown codec error";
    }

private:
    [[nodiscard]] static PushDecodeResult failure(ScriptCodecError error)
    {
        return PushDecodeResult{std::nullopt, error};
    }

    static void setError(ScriptCodecError* target, ScriptCodecError error)
    {
        if (target) {
            *target = error;
        }
    }

    [[nodiscard]] static int hexDigit(char value) noexcept
    {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return 10 + value - 'a';
        }
        if (value >= 'A' && value <= 'F') {
            return 10 + value - 'A';
        }
        return -1;
    }

    [[nodiscard]] static std::string byteToHex(uint8_t byte)
    {
        static constexpr char digits[] = "0123456789abcdef";
        return {digits[byte >> 4], digits[byte & 0x0f]};
    }

    [[nodiscard]] static std::string
    littleEndianHex(size_t value, size_t byteCount)
    {
        std::string result;
        result.reserve(byteCount * 2);
        for (size_t i = 0; i < byteCount; ++i) {
            result += byteToHex(
                static_cast<uint8_t>((value >> (8 * i)) & 0xff)
            );
        }
        return result;
    }
};

SPACE_TBC_END

#endif // SCRIPT_CODEC_H

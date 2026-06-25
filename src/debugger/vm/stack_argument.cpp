#include "stack_argument.h"

#include <cstdint>
#include <string>

#include "../../bytecode/bytecode_helper_fun.h"
#include "../../interpreter/runtime_codec.h"

namespace apc_debug
{
namespace
{

bool isAddressType(const std::string& typeName)
{
    return typeName == "address" || typeName == "Address";
}

StackArgumentParseResult defaultResult(StackArgumentStatus status)
{
    StackArgumentParseResult result;
    result.value = defaultStackArgumentValue("");
    result.status = status;
    return result;
}

StackArgumentParseResult parseAddressValue(const std::string& input)
{
    const std::string pubkeyHashHex = tbc::decodeP2PKHAddress(input);
    if (!pubkeyHashHex.empty() && pubkeyHashHex.size() == 40) {
        StackArgumentParseResult result;
        result.value = StackElement::fromHexLiteral(
            "0x" + tbc::bytEncodeLengthOpcode(20) + pubkeyHashHex
        );
        result.convertedAddress = true;
        return result;
    }
    return defaultResult(StackArgumentStatus::InvalidAddress);
}

StackArgumentParseResult parseStackArgumentValueImpl(
    const std::string& rawInput,
    const std::string& typeName,
    bool defaultInvalidHex
)
{
    const std::string input = apc_interpreter::runtime_codec::trim(rawInput);
    if (input.empty()) {
        return defaultResult(StackArgumentStatus::DefaultEmpty);
    }

    if (input.size() >= 2 && input.front() == '"' && input.back() == '"') {
        const std::string unquoted = input.substr(1, input.size() - 2);
        if (isAddressType(typeName)) {
            return parseAddressValue(unquoted);
        }

        StackArgumentParseResult result;
        result.value = StackElement::fromBytesString(unquoted);
        return result;
    }

    if (input.size() >= 2 &&
        (input.substr(0, 2) == "0x" || input.substr(0, 2) == "0X")) {
        try {
            StackArgumentParseResult result;
            result.value = StackElement::fromHexLiteral(input);
            return result;
        } catch (...) {
            if (!defaultInvalidHex) {
                throw;
            }
            return defaultResult(StackArgumentStatus::InvalidHex);
        }
    }

    if (isAddressType(typeName)) {
        return parseAddressValue(input);
    }

    try {
        StackArgumentParseResult result;
        result.value = StackElement(std::stoll(input));
        return result;
    } catch (...) {
        return defaultResult(StackArgumentStatus::InvalidNumber);
    }
}

} // namespace

StackElement defaultStackArgumentValue(const std::string& typeName)
{
    (void)typeName;
    return StackElement(std::vector<uint8_t>{0x00});
}

StackArgumentParseResult parseStackArgumentValueDetailed(
    const std::string& rawInput,
    const std::string& typeName
)
{
    return parseStackArgumentValueImpl(rawInput, typeName, true);
}

StackElement parseStackArgumentValue(
    const std::string& rawInput,
    const std::string& typeName
)
{
    return parseStackArgumentValueImpl(rawInput, typeName, false).value;
}

} // namespace apc_debug

#include "builtin_runtime.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_set>

#include "../crypto/hash_utils.h"
#include "runtime_codec.h"
#include "runtime_error.h"

namespace apc_interpreter
{
namespace
{

void expectArgCount(
    const std::string& name,
    const std::vector<RuntimeValue>& args,
    size_t expected,
    SourceLocation location
)
{
    if (args.size() != expected) {
        throw RuntimeError(
            RuntimeErrorKind::BuiltinError,
            name + "(...) expects " + std::to_string(expected) +
                " argument(s), got " + std::to_string(args.size()),
            location
        );
    }
}

RuntimeValue verifyTruthy(
    const std::string& name,
    const RuntimeValue& value,
    SourceLocation location
)
{
    if (!value.truthy()) {
        throw RuntimeError(
            RuntimeErrorKind::BuiltinError,
            name + "(...) verification failed",
            location
        );
    }
    return RuntimeValue::voidValue();
}

RuntimeValue verifyEqual(
    const std::string& name,
    const RuntimeValue& lhs,
    const RuntimeValue& rhs,
    SourceLocation location
)
{
    if (lhs != rhs) {
        throw RuntimeError(
            RuntimeErrorKind::BuiltinError,
            name + "(...) verification failed: left=" +
                lhs.toDisplayString() + ", right=" + rhs.toDisplayString(),
            location
        );
    }
    return RuntimeValue::voidValue();
}

RuntimeValue fromDigest(
    std::vector<uint8_t> bytes,
    const std::string& declaredType
)
{
    return RuntimeValue::fromBytes(std::move(bytes), declaredType);
}

RuntimeValue numToBin(
    int64_t number,
    int64_t rawSize,
    SourceLocation location
)
{
    if (rawSize < 0) {
        throw RuntimeError(
            RuntimeErrorKind::BuiltinError,
            "NumToBin(...) size cannot be negative",
            location
        );
    }

    const size_t size = static_cast<size_t>(rawSize);
    std::vector<uint8_t> bytes = runtime_codec::serializeScriptNum(number);
    if (bytes.empty()) {
        return RuntimeValue::fromBytes(
            std::vector<uint8_t>(size, 0x00),
            "bytes"
        );
    }

    const bool negative = (bytes.back() & 0x80) != 0;
    if (negative) {
        bytes.back() &= 0x7f;
    }
    if (bytes.size() > size) {
        throw RuntimeError(
            RuntimeErrorKind::BuiltinError,
            "NumToBin(...) size is too small for the number",
            location
        );
    }

    bytes.resize(size, 0x00);
    if (negative && !bytes.empty()) {
        bytes.back() |= 0x80;
    }
    return RuntimeValue::fromBytes(std::move(bytes), "bytes");
}

RuntimeValue sliceBytes(
    const RuntimeValue& value,
    int64_t rawStart,
    int64_t rawLength
)
{
    std::vector<uint8_t> bytes = value.toScriptBytes();
    size_t start = rawStart < 0 ? 0 : static_cast<size_t>(rawStart);
    if (start > bytes.size()) {
        start = bytes.size();
    }

    size_t end = bytes.size();
    if (rawLength >= 0) {
        const size_t length = static_cast<size_t>(rawLength);
        end = std::min(bytes.size(), start + length);
    }

    return RuntimeValue::fromBytes(
        std::vector<uint8_t>(bytes.begin() + start, bytes.begin() + end),
        value.declaredType().empty() ? "bytes" : value.declaredType()
    );
}

std::vector<uint8_t> littleEndianU64(uint64_t value)
{
    std::vector<uint8_t> out(8);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
    }
    return out;
}

} // namespace

bool BuiltinRuntime::isBuiltinFunction(const std::string& name)
{
    static const std::unordered_set<std::string> kNames = {
        "Push",
        "Clone",
        "Ripemd160",
        "Sha1",
        "Sha256",
        "Hash160",
        "Hash256",
        "Cat",
        "Concat",
        "Slice",
        "Size",
        "NumToBin",
        "BinToNum",
        "PartialHash",
        "Equal",
        "EqualVerify",
        "Require",
        "Verify",
        "NumEqual",
        "NumEqualVerify",
        "NumNotEqual",
        "LessThan",
        "GreaterThan",
        "LessThanOrEqual",
        "GreaterThanOrEqual",
        "Add",
        "Sub",
        "Mul",
        "Div",
        "Mod",
        "Inc",
        "Dec",
        "Neg",
        "Abs",
        "Not",
        "ZeroNotEqual",
        "CheckSig",
        "CheckSigVerify",
        "MultiSig",
        "MultiSigVerify",
        "Delete",
        "Keep",
        "Move",
        "SetAlt",
        "SetMain",
    };
    return kNames.find(name) != kNames.end();
}

RuntimeValue BuiltinRuntime::callFunction(
    const std::string& name,
    const std::vector<RuntimeValue>& args,
    SourceLocation location
)
{
    if (name == "Push" || name == "Clone") {
        expectArgCount(name, args, 1, location);
        return args.front();
    }

    if (name == "Ripemd160") {
        expectArgCount(name, args, 1, location);
        return fromDigest(
            apc_crypto::ripemd160Digest(args.front().toScriptBytes()),
            "ripemd160"
        );
    }
    if (name == "Sha1") {
        expectArgCount(name, args, 1, location);
        return fromDigest(apc_crypto::sha1Digest(args.front().toScriptBytes()), "sha1");
    }
    if (name == "Sha256") {
        expectArgCount(name, args, 1, location);
        return fromDigest(
            apc_crypto::sha256Digest(args.front().toScriptBytes()),
            "sha256"
        );
    }
    if (name == "Hash160") {
        expectArgCount(name, args, 1, location);
        return fromDigest(
            apc_crypto::hash160Digest(args.front().toScriptBytes()),
            "hash160"
        );
    }
    if (name == "Hash256") {
        expectArgCount(name, args, 1, location);
        return fromDigest(
            apc_crypto::hash256Digest(args.front().toScriptBytes()),
            "hash256"
        );
    }
    if (name == "Cat" || name == "Concat") {
        expectArgCount(name, args, 2, location);
        std::vector<uint8_t> bytes = args[0].toScriptBytes();
        std::vector<uint8_t> rhs = args[1].toScriptBytes();
        bytes.insert(bytes.end(), rhs.begin(), rhs.end());
        return RuntimeValue::fromBytes(std::move(bytes), "bytes");
    }
    if (name == "Size") {
        expectArgCount(name, args, 1, location);
        return RuntimeValue::fromInt(
            static_cast<int64_t>(args.front().toScriptBytes().size()),
            "int"
        );
    }
    if (name == "Slice") {
        expectArgCount(name, args, 3, location);
        return sliceBytes(
            args[0],
            args[1].toScriptNum(),
            args[2].toScriptNum()
        );
    }
    if (name == "BinToNum") {
        expectArgCount(name, args, 1, location);
        return RuntimeValue::fromInt(args.front().toScriptNum(), "int");
    }
    if (name == "NumToBin") {
        expectArgCount(name, args, 2, location);
        return numToBin(
            args[0].toScriptNum(),
            args[1].toScriptNum(),
            location
        );
    }
    if (name == "PartialHash") {
        expectArgCount(name, args, 3, location);
        const std::vector<uint8_t> vch = args[0].toScriptBytes();
        const std::vector<uint8_t> vchPartHash = args[1].toScriptBytes();
        const uint64_t totalSize = static_cast<uint64_t>(args[2].toScriptNum());

        if (vchPartHash.empty()) {
            if (totalSize != vch.size()) {
                throw RuntimeError(
                    RuntimeErrorKind::BuiltinError,
                    "PartialHash(...) size mismatch for full hash",
                    location
                );
            }
            return fromDigest(apc_crypto::sha256Digest(vch), "sha256");
        }

        if (vchPartHash.size() != 32 || totalSize < vch.size()) {
            throw RuntimeError(
                RuntimeErrorKind::BuiltinError,
                "PartialHash(...) expects empty or 32-byte partial hash",
                location
            );
        }

        std::vector<uint8_t> data;
        std::vector<uint8_t> partHashSize =
            littleEndianU64(totalSize - static_cast<uint64_t>(vch.size()));
        data.reserve(vchPartHash.size() + partHashSize.size() + vch.size());
        data.insert(data.end(), vchPartHash.begin(), vchPartHash.end());
        data.insert(data.end(), partHashSize.begin(), partHashSize.end());
        data.insert(data.end(), vch.begin(), vch.end());
        return fromDigest(apc_crypto::sha256Digest(data), "sha256");
    }

    if (name == "Equal") {
        expectArgCount(name, args, 2, location);
        return RuntimeValue::fromBool(args[0] == args[1]);
    }
    if (name == "EqualVerify" || name == "Require") {
        expectArgCount(name, args, 2, location);
        return verifyEqual(name, args[0], args[1], location);
    }
    if (name == "Verify") {
        expectArgCount(name, args, 1, location);
        return verifyTruthy(name, args[0], location);
    }

    if (name == "NumEqual") {
        expectArgCount(name, args, 2, location);
        return RuntimeValue::fromBool(args[0].toScriptNum() == args[1].toScriptNum());
    }
    if (name == "NumEqualVerify") {
        expectArgCount(name, args, 2, location);
        return verifyTruthy(
            name,
            RuntimeValue::fromBool(args[0].toScriptNum() == args[1].toScriptNum()),
            location
        );
    }
    if (name == "NumNotEqual") {
        expectArgCount(name, args, 2, location);
        return RuntimeValue::fromBool(args[0].toScriptNum() != args[1].toScriptNum());
    }
    if (name == "LessThan") {
        expectArgCount(name, args, 2, location);
        return RuntimeValue::fromBool(args[0].toScriptNum() < args[1].toScriptNum());
    }
    if (name == "GreaterThan") {
        expectArgCount(name, args, 2, location);
        return RuntimeValue::fromBool(args[0].toScriptNum() > args[1].toScriptNum());
    }
    if (name == "LessThanOrEqual") {
        expectArgCount(name, args, 2, location);
        return RuntimeValue::fromBool(args[0].toScriptNum() <= args[1].toScriptNum());
    }
    if (name == "GreaterThanOrEqual") {
        expectArgCount(name, args, 2, location);
        return RuntimeValue::fromBool(args[0].toScriptNum() >= args[1].toScriptNum());
    }

    if (name == "Add") {
        expectArgCount(name, args, 2, location);
        return RuntimeValue::fromInt(args[0].toScriptNum() + args[1].toScriptNum());
    }
    if (name == "Sub") {
        expectArgCount(name, args, 2, location);
        return RuntimeValue::fromInt(args[0].toScriptNum() - args[1].toScriptNum());
    }
    if (name == "Mul") {
        expectArgCount(name, args, 2, location);
        return RuntimeValue::fromInt(args[0].toScriptNum() * args[1].toScriptNum());
    }
    if (name == "Div" || name == "Mod") {
        expectArgCount(name, args, 2, location);
        const int64_t divisor = args[1].toScriptNum();
        if (divisor == 0) {
            throw RuntimeError(
                RuntimeErrorKind::BuiltinError,
                name + "(...) division by zero",
                location
            );
        }
        if (name == "Div") {
            return RuntimeValue::fromInt(args[0].toScriptNum() / divisor);
        }
        return RuntimeValue::fromInt(args[0].toScriptNum() % divisor);
    }
    if (name == "Inc") {
        expectArgCount(name, args, 1, location);
        return RuntimeValue::fromInt(args[0].toScriptNum() + 1);
    }
    if (name == "Dec") {
        expectArgCount(name, args, 1, location);
        return RuntimeValue::fromInt(args[0].toScriptNum() - 1);
    }
    if (name == "Neg") {
        expectArgCount(name, args, 1, location);
        return RuntimeValue::fromInt(-args[0].toScriptNum());
    }
    if (name == "Abs") {
        expectArgCount(name, args, 1, location);
        return RuntimeValue::fromInt(std::llabs(args[0].toScriptNum()));
    }
    if (name == "Not") {
        expectArgCount(name, args, 1, location);
        return RuntimeValue::fromBool(!args[0].truthy());
    }
    if (name == "ZeroNotEqual") {
        expectArgCount(name, args, 1, location);
        return RuntimeValue::fromBool(args[0].toScriptNum() != 0);
    }

    if (name == "CheckSig" || name == "MultiSig") {
        expectArgCount(name, args, 2, location);
        throw RuntimeError(
            RuntimeErrorKind::BuiltinError,
            name +
                "(...) requires an interpreter signature policy; AST mode "
                "uses BVM.checkSigResult/BVM.multiSigResult and bytecode mode "
                "uses BVMSimulator::setCheckSigCallback"
        );
    }
    if (name == "CheckSigVerify" || name == "MultiSigVerify") {
        expectArgCount(name, args, 2, location);
        throw RuntimeError(
            RuntimeErrorKind::BuiltinError,
            name +
                "(...) requires an interpreter signature policy; AST mode "
                "uses BVM.checkSigResult/BVM.multiSigResult and bytecode mode "
                "uses BVMSimulator::setCheckSigCallback"
        );
    }
    if (name == "Delete") {
        expectArgCount(name, args, 1, location);
        return RuntimeValue::voidValue();
    }
    if (name == "Keep" || name == "Move" ||
        name == "SetAlt" || name == "SetMain") {
        expectArgCount(name, args, 1, location);
        return args.front();
    }

    throw RuntimeError(
        RuntimeErrorKind::BuiltinError,
        "unsupported builtin function '" + name + "'",
        location
    );
}

} // namespace apc_interpreter

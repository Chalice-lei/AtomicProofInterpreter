#include "transaction_context.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#include "../crypto/hash_utils.h"
#include "../util/string_utils.h"
#include "runtime_codec.h"

namespace apc_interpreter
{
namespace
{

using nlohmann::json;

using runtime_codec::bytesToHex;
using runtime_codec::serializeScriptNum;
using runtime_codec::toLower;
using runtime_codec::trim;
using apc::util::startsWith;

std::string normalizeName(const std::string& value)
{
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch) != 0) {
            normalized.push_back(
                static_cast<char>(std::tolower(ch))
            );
        }
    }
    return normalized;
}

bool isJsonScalar(const json& value)
{
    return value.is_null() || value.is_string() || value.is_boolean() ||
           value.is_number();
}

std::vector<uint8_t> littleEndianU32(uint32_t value)
{
    return {
        static_cast<uint8_t>(value & 0xff),
        static_cast<uint8_t>((value >> 8) & 0xff),
        static_cast<uint8_t>((value >> 16) & 0xff),
        static_cast<uint8_t>((value >> 24) & 0xff),
    };
}

uint32_t jsonScalarToU32(
    const json& value,
    const std::string& path
)
{
    if (value.is_number_unsigned()) {
        const uint64_t number = value.get<uint64_t>();
        if (number > UINT32_MAX) {
            throw std::runtime_error(
                "JSON transaction context path '" + path +
                "' is too large for uint32"
            );
        }
        return static_cast<uint32_t>(number);
    }
    if (value.is_number_integer()) {
        const int64_t number = value.get<int64_t>();
        if (number < 0 || number > static_cast<int64_t>(UINT32_MAX)) {
            throw std::runtime_error(
                "JSON transaction context path '" + path +
                "' is outside uint32 range"
            );
        }
        return static_cast<uint32_t>(number);
    }
    if (value.is_string()) {
        const std::string text = trim(value.get<std::string>());
        size_t consumed = 0;
        unsigned long number = std::stoul(text, &consumed, 0);
        if (consumed != text.size() || number > UINT32_MAX) {
            throw std::runtime_error(
                "JSON transaction context path '" + path +
                "' is not a valid uint32"
            );
        }
        return static_cast<uint32_t>(number);
    }

    throw std::runtime_error(
        "JSON transaction context path '" + path +
        "' cannot be converted to uint32"
    );
}

std::vector<uint8_t> parseHexBytes(std::string value)
{
    return runtime_codec::parseHex(
        std::move(value),
        "invalid hex byte in transaction context"
    );
}

void addBvmAssignment(
    RuntimeAssignmentSets& result,
    const std::string& field,
    const std::string& value
)
{
    result.bvmAssignments.push_back(field + "=" + value);
}

bool addLegacyTransactionKey(
    RuntimeAssignmentSets& result,
    const std::string& lowerKey,
    const std::string& value
)
{
    if (lowerKey == "version") {
        addBvmAssignment(result, "version", value);
        return true;
    }
    if (lowerKey == "locktime") {
        addBvmAssignment(result, "locktime", value);
        return true;
    }
    if (lowerKey == "inputcount") {
        addBvmAssignment(result, "inputCount", value);
        return true;
    }
    if (lowerKey == "outputcount") {
        addBvmAssignment(result, "outputCount", value);
        return true;
    }
    if (lowerKey == "inputshash") {
        addBvmAssignment(result, "inputsHash", value);
        return true;
    }
    if (lowerKey == "unlockinginput") {
        addBvmAssignment(result, "unlockingInput", value);
        return true;
    }
    if (lowerKey == "outputshash") {
        addBvmAssignment(result, "outputsHash", value);
        return true;
    }
    return false;
}

std::string jsonScalarToRuntimeText(
    const json& value,
    const std::string& path
)
{
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<int64_t>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<uint64_t>());
    }
    if (value.is_number_float()) {
        throw std::runtime_error(
            "JSON transaction context path '" + path +
            "' uses a floating-point number; use an integer or string"
        );
    }
    if (value.is_null()) {
        return "";
    }
    throw std::runtime_error(
        "JSON transaction context path '" + path +
        "' is not a scalar value"
    );
}

std::vector<uint8_t> jsonScalarToBytes(
    const json& value,
    const std::string& path
)
{
    if (value.is_null()) {
        return {};
    }
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (text.size() >= 2 && text[0] == '0' &&
            (text[1] == 'x' || text[1] == 'X')) {
            return parseHexBytes(text);
        }
        return std::vector<uint8_t>(text.begin(), text.end());
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? std::vector<uint8_t>{0x01}
                                 : std::vector<uint8_t>{};
    }
    if (value.is_number_integer()) {
        return serializeScriptNum(value.get<int64_t>());
    }
    if (value.is_number_unsigned()) {
        const uint64_t number = value.get<uint64_t>();
        if (number > static_cast<uint64_t>(INT64_MAX)) {
            throw std::runtime_error(
                "JSON transaction context path '" + path +
                "' integer is too large for script-num conversion"
            );
        }
        return serializeScriptNum(static_cast<int64_t>(number));
    }
    throw std::runtime_error(
        "JSON transaction context path '" + path +
        "' cannot be converted to bytes"
    );
}

std::vector<uint8_t> fixedBytesFromJsonScalar(
    const json& value,
    size_t width,
    const std::string& path
)
{
    if (value.is_number_integer() || value.is_number_unsigned()) {
        if (width == 4) {
            return littleEndianU32(jsonScalarToU32(value, path));
        }
    }

    std::vector<uint8_t> bytes = jsonScalarToBytes(value, path);
    if (bytes.size() == width) {
        return bytes;
    }
    if (bytes.size() < width) {
        bytes.resize(width, 0x00);
        return bytes;
    }

    throw std::runtime_error(
        "JSON transaction context path '" + path + "' expected " +
        std::to_string(width) + " byte(s), got " +
        std::to_string(bytes.size())
    );
}

const json* findObjectField(
    const json& object,
    std::initializer_list<const char*> names
)
{
    if (!object.is_object()) {
        return nullptr;
    }

    for (const char* name : names) {
        auto it = object.find(name);
        if (it != object.end()) {
            return &(*it);
        }
    }

    for (const auto& [candidate, value] : object.items()) {
        const std::string normalizedCandidate = toLower(candidate);
        std::string compactCandidate;
        compactCandidate.reserve(normalizedCandidate.size());
        for (char ch : normalizedCandidate) {
            if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
                compactCandidate.push_back(ch);
            }
        }

        for (const char* name : names) {
            const std::string normalizedName = toLower(name);
            std::string compactName;
            compactName.reserve(normalizedName.size());
            for (char ch : normalizedName) {
                if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
                    compactName.push_back(ch);
                }
            }
            if (compactCandidate == compactName) {
                return &value;
            }
        }
    }

    return nullptr;
}

bool hasBvmAssignment(
    const RuntimeAssignmentSets& result,
    const std::string& fieldName
)
{
    auto normalizeField = [](const std::string& field) {
        std::string normalized;
        for (unsigned char ch : field) {
            if (std::isalnum(ch) != 0) {
                normalized.push_back(
                    static_cast<char>(std::tolower(ch))
                );
            }
        }
        return normalized;
    };

    const std::string expected = normalizeField(fieldName);
    for (const std::string& assignment : result.bvmAssignments) {
        const size_t eq = assignment.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        if (normalizeField(assignment.substr(0, eq)) == expected) {
            return true;
        }
    }
    return false;
}

void addBvmAssignmentIfMissing(
    RuntimeAssignmentSets& result,
    const std::string& field,
    const std::string& value
)
{
    if (!hasBvmAssignment(result, field)) {
        addBvmAssignment(result, field, value);
    }
}

const json* findLockingScriptValue(const json& output)
{
    const json* script = findObjectField(
        output,
        {"lockingScript", "locking_script", "lockScript", "script", "scriptPubKey"}
    );
    if (!script) {
        return nullptr;
    }

    if (script->is_object()) {
        if (const json* nested = findObjectField(
                *script,
                {"hex", "raw", "data", "script", "suffixData", "suffix_data"}
            )) {
            return nested;
        }
    }

    return script;
}

std::vector<uint8_t> scriptBytesFromOutput(const json& output)
{
    const json* script = findLockingScriptValue(output);
    if (!script || !isJsonScalar(*script)) {
        return {};
    }
    return jsonScalarToBytes(*script, "lockingScript");
}

std::vector<uint8_t> valueBytesFromOutput(const json& output)
{
    const json* value =
        findObjectField(output, {"value", "amount", "satoshis"});
    if (!value || !isJsonScalar(*value)) {
        return {};
    }
    return jsonScalarToBytes(*value, "value");
}

std::vector<uint8_t> deriveOutputsHash(const json& outputs)
{
    std::vector<uint8_t> data;
    if (!outputs.is_array()) {
        return data;
    }

    for (const auto& output : outputs) {
        if (!output.is_object()) {
            continue;
        }

        const std::vector<uint8_t> script = scriptBytesFromOutput(output);
        if (script.empty()) {
            continue;
        }

        const std::vector<uint8_t> value = valueBytesFromOutput(output);
        const std::vector<uint8_t> scriptHash = apc_crypto::sha256Digest(script);
        data.insert(data.end(), value.begin(), value.end());
        data.insert(data.end(), scriptHash.begin(), scriptHash.end());
    }

    if (data.empty()) {
        return {};
    }
    return apc_crypto::sha256Digest(data);
}

void addDerivedOutputAssignments(
    RuntimeAssignmentSets& result,
    const std::string& txName,
    const json& outputs
)
{
    if (!outputs.is_array()) {
        return;
    }

    for (size_t i = 0; i < outputs.size(); ++i) {
        const json& output = outputs[i];
        if (!output.is_object()) {
            continue;
        }

        const std::string outputPath =
            txName + ".outputs[" + std::to_string(i) + "]";

        if (const json* value =
                findObjectField(output, {"value", "amount", "satoshis"});
            value && isJsonScalar(*value)) {
            result.paramAssignments.push_back(
                outputPath + ".value=" + jsonScalarToRuntimeText(*value, outputPath)
            );
        }

        const json* scriptNode = findObjectField(
            output,
            {"lockingScript", "locking_script", "lockScript", "script", "scriptPubKey"}
        );
        if (!scriptNode) {
            continue;
        }

        const std::vector<uint8_t> script = scriptBytesFromOutput(output);
        const bool hasScriptBytes = !script.empty();
        if (hasScriptBytes) {
            result.paramAssignments.push_back(
                outputPath + ".lockingScript.suffixData=" + bytesToHex(script)
            );
            result.paramAssignments.push_back(
                outputPath + ".lockingScript.size=" +
                bytesToHex(littleEndianU32(static_cast<uint32_t>(script.size())))
            );
        }

        bool hasExplicitPartialHash = false;
        if (scriptNode->is_object()) {
            if (const json* partial = findObjectField(
                    *scriptNode,
                    {"partialHash", "partial_hash"}
                );
                partial && isJsonScalar(*partial)) {
                hasExplicitPartialHash = true;
                result.paramAssignments.push_back(
                    outputPath + ".lockingScript.partialHash=" +
                    jsonScalarToRuntimeText(*partial, outputPath)
                );
            }
        }

        if (!hasExplicitPartialHash && hasScriptBytes) {
            result.paramAssignments.push_back(
                outputPath + ".lockingScript.partialHash=0x"
            );
        }
    }
}

std::vector<uint8_t> inputDataFromInput(const json& input)
{
    if (!input.is_object()) {
        return {};
    }
    if (const json* data = findObjectField(input, {"data", "raw", "hex"});
        data && isJsonScalar(*data)) {
        return jsonScalarToBytes(*data, "input.data");
    }

    std::vector<uint8_t> out;
    if (const json* txid = findObjectField(
            input,
            {"txid", "txId", "prevTxid", "prev_txid", "previousTxid"}
        );
        txid && isJsonScalar(*txid)) {
        const std::vector<uint8_t> txidBytes =
            fixedBytesFromJsonScalar(*txid, 32, "txid");
        out.insert(out.end(), txidBytes.begin(), txidBytes.end());
    }
    if (const json* vout = findObjectField(input, {"vout", "index"});
        vout && isJsonScalar(*vout)) {
        const std::vector<uint8_t> voutBytes =
            fixedBytesFromJsonScalar(*vout, 4, "vout");
        out.insert(out.end(), voutBytes.begin(), voutBytes.end());
    }
    if (const json* sequence = findObjectField(input, {"sequence", "seq"});
        sequence && isJsonScalar(*sequence)) {
        const std::vector<uint8_t> sequenceBytes =
            fixedBytesFromJsonScalar(*sequence, 4, "sequence");
        out.insert(out.end(), sequenceBytes.begin(), sequenceBytes.end());
    }
    return out;
}

std::vector<uint8_t> unlockingInputDataFromInput(
    const json& input,
    const std::vector<uint8_t>& fallbackTxid
)
{
    if (!input.is_object()) {
        return {};
    }
    if (const json* data = findObjectField(
            input,
            {"unlockingInput", "unlocking_input", "data", "raw", "hex"}
        );
        data && isJsonScalar(*data)) {
        return jsonScalarToBytes(*data, "input.unlockingInput");
    }

    std::vector<uint8_t> out;
    if (const json* txid = findObjectField(
            input,
            {"txid", "txId", "prevTxid", "prev_txid", "previousTxid"}
        );
        txid && isJsonScalar(*txid)) {
        const std::vector<uint8_t> txidBytes =
            fixedBytesFromJsonScalar(*txid, 32, "txid");
        out.insert(out.end(), txidBytes.begin(), txidBytes.end());
    } else if (fallbackTxid.size() == 32) {
        out.insert(out.end(), fallbackTxid.begin(), fallbackTxid.end());
    } else {
        return {};
    }

    const json* vout = findObjectField(input, {"vout", "index", "outputIndex"});
    const json* sequence = findObjectField(input, {"sequence", "seq"});
    if (!vout || !isJsonScalar(*vout) || !sequence ||
        !isJsonScalar(*sequence)) {
        return {};
    }

    const std::vector<uint8_t> voutBytes =
        fixedBytesFromJsonScalar(*vout, 4, "vout");
    const std::vector<uint8_t> sequenceBytes =
        fixedBytesFromJsonScalar(*sequence, 4, "sequence");
    out.insert(out.end(), voutBytes.begin(), voutBytes.end());
    out.insert(out.end(), sequenceBytes.begin(), sequenceBytes.end());
    return out;
}

void addDerivedInputAssignments(
    RuntimeAssignmentSets& result,
    const std::string& txName,
    const json& inputs
)
{
    if (!inputs.is_array()) {
        return;
    }

    for (size_t i = 0; i < inputs.size(); ++i) {
        const json& input = inputs[i];
        if (!input.is_object()) {
            continue;
        }

        const std::vector<uint8_t> inputData = inputDataFromInput(input);
        if (inputData.empty()) {
            continue;
        }

        result.paramAssignments.push_back(
            txName + ".inputs[" + std::to_string(i) + "].data=" +
            bytesToHex(inputData)
        );
    }
}

std::vector<uint8_t> deriveInputsHash(const json& inputs)
{
    if (!inputs.is_array()) {
        return {};
    }

    std::vector<uint8_t> data;
    for (const auto& input : inputs) {
        const std::vector<uint8_t> inputData = inputDataFromInput(input);
        data.insert(data.end(), inputData.begin(), inputData.end());
    }

    if (data.empty()) {
        return {};
    }
    return apc_crypto::sha256Digest(data);
}

void addScalarParamIfPresent(
    RuntimeAssignmentSets& result,
    const std::string& path,
    const json& object,
    std::initializer_list<const char*> names
)
{
    if (const json* value = findObjectField(object, names);
        value && isJsonScalar(*value)) {
        result.paramAssignments.push_back(
            path + "=" + jsonScalarToRuntimeText(*value, path)
        );
    }
}

std::vector<uint8_t> objectScalarBytes(
    const json& object,
    std::initializer_list<const char*> names,
    const std::string& path
)
{
    if (const json* value = findObjectField(object, names);
        value && isJsonScalar(*value)) {
        return jsonScalarToBytes(*value, path);
    }
    return {};
}

std::vector<uint8_t> objectFixedBytes(
    const json& object,
    std::initializer_list<const char*> names,
    size_t width,
    const std::string& path
)
{
    if (const json* value = findObjectField(object, names);
        value && isJsonScalar(*value)) {
        return fixedBytesFromJsonScalar(*value, width, path);
    }
    return {};
}

std::vector<uint8_t> deriveUnlockingScriptHash(const json& tx)
{
    std::vector<uint8_t> explicitHash = objectFixedBytes(
        tx,
        {"unlockingScriptHash", "unlocking_script_hash", "scriptHash"},
        32,
        "unlockingScriptHash"
    );
    if (!explicitHash.empty()) {
        return explicitHash;
    }

    const std::vector<uint8_t> unlockingScript = objectScalarBytes(
        tx,
        {"unlockingScript", "unlocking_script", "scriptSig"},
        "unlockingScript"
    );
    if (unlockingScript.empty()) {
        return {};
    }

    return apc_crypto::sha256Digest(unlockingScript);
}

std::vector<uint8_t> deriveTransactionInputsHash(const json& tx)
{
    if (const json* inputs = findObjectField(tx, {"inputs", "vin"});
        inputs && inputs->is_array()) {
        const std::vector<uint8_t> derived = deriveInputsHash(*inputs);
        if (!derived.empty()) {
            return derived;
        }
    }

    return objectFixedBytes(
        tx,
        {"inputsHash", "inputs_hash", "vinHash"},
        32,
        "inputsHash"
    );
}

std::vector<uint8_t> deriveTransactionOutputsHash(const json& tx)
{
    if (const json* outputs = findObjectField(tx, {"outputs", "vout"});
        outputs && outputs->is_array()) {
        const std::vector<uint8_t> derived = deriveOutputsHash(*outputs);
        if (!derived.empty()) {
            return derived;
        }
    }

    return objectFixedBytes(
        tx,
        {"outputsHash", "outputs_hash", "voutHash"},
        32,
        "outputsHash"
    );
}

std::vector<uint8_t> deriveTransactionPreimage(const json& tx)
{
    const std::vector<uint8_t> vlio = objectScalarBytes(
        tx,
        {"vlio", "VLIO", "prefix", "header"},
        "vlio"
    );
    const std::vector<uint8_t> inputsHash = deriveTransactionInputsHash(tx);
    const std::vector<uint8_t> unlockingScriptHash =
        deriveUnlockingScriptHash(tx);
    const std::vector<uint8_t> outputsHash = deriveTransactionOutputsHash(tx);

    if (vlio.empty() || inputsHash.empty() || unlockingScriptHash.empty() ||
        outputsHash.empty()) {
        return {};
    }

    std::vector<uint8_t> preimage;
    preimage.reserve(
        vlio.size() + inputsHash.size() + unlockingScriptHash.size() +
        outputsHash.size()
    );
    preimage.insert(preimage.end(), vlio.begin(), vlio.end());
    preimage.insert(preimage.end(), inputsHash.begin(), inputsHash.end());
    preimage.insert(
        preimage.end(),
        unlockingScriptHash.begin(),
        unlockingScriptHash.end()
    );
    preimage.insert(preimage.end(), outputsHash.begin(), outputsHash.end());
    return preimage;
}

std::vector<uint8_t> deriveTransactionId(const json& tx)
{
    const std::vector<uint8_t> preimage = deriveTransactionPreimage(tx);
    if (!preimage.empty()) {
        return apc_crypto::hash256Digest(preimage);
    }

    return objectFixedBytes(
        tx,
        {"txid", "txId", "TXID", "transactionId", "transaction_id"},
        32,
        "txid"
    );
}

void addDerivedTransactionIdAssignments(
    RuntimeAssignmentSets& result,
    const std::string& txName,
    const json& tx
)
{
    const std::vector<uint8_t> preimage = deriveTransactionPreimage(tx);
    if (!preimage.empty()) {
        result.paramAssignments.push_back(
            txName + ".txData=" + bytesToHex(preimage)
        );
        const std::vector<uint8_t> txid =
            apc_crypto::hash256Digest(preimage);
        result.paramAssignments.push_back(
            txName + ".txid=" + bytesToHex(txid)
        );

        if (normalizeName(txName) == "currenttx") {
            addBvmAssignmentIfMissing(result, "txid", bytesToHex(txid));
        }
        return;
    }

    const std::vector<uint8_t> explicitTxid = deriveTransactionId(tx);
    if (explicitTxid.empty()) {
        return;
    }

    result.paramAssignments.push_back(
        txName + ".txid=" + bytesToHex(explicitTxid)
    );
    if (normalizeName(txName) == "currenttx") {
        addBvmAssignmentIfMissing(result, "txid", bytesToHex(explicitTxid));
    }
}

size_t findUnlockingInputIndex(const json& root, const json* currentTx)
{
    const std::initializer_list<const char*> names = {
        "unlockingInputIndex",
        "unlocking_input_index",
        "inputIndex",
        "input_index",
        "vinIndex",
        "spendingInputIndex",
    };

    if (currentTx) {
        if (const json* value = findObjectField(*currentTx, names);
            value && isJsonScalar(*value)) {
            return static_cast<size_t>(
                jsonScalarToU32(*value, "unlockingInputIndex")
            );
        }
    }

    if (const json* value = findObjectField(root, names);
        value && isJsonScalar(*value)) {
        return static_cast<size_t>(
            jsonScalarToU32(*value, "unlockingInputIndex")
        );
    }

    return 0;
}

std::vector<uint8_t> directUnlockingInputFromObject(const json& object)
{
    return objectScalarBytes(
        object,
        {"unlockingInput", "unlocking_input", "unlockingInputData"},
        "unlockingInput"
    );
}

void addDerivedUnlockingInputAssignment(
    RuntimeAssignmentSets& result,
    const json& root,
    const json* currentTx,
    const json* preTx
)
{
    if (hasBvmAssignment(result, "unlockingInput")) {
        return;
    }

    if (currentTx) {
        const std::vector<uint8_t> direct =
            directUnlockingInputFromObject(*currentTx);
        if (!direct.empty()) {
            addBvmAssignmentIfMissing(
                result,
                "unlockingInput",
                bytesToHex(direct)
            );
            return;
        }
    }

    const json* inputs = nullptr;
    if (currentTx) {
        inputs = findObjectField(*currentTx, {"inputs", "vin"});
    }
    if (!inputs || !inputs->is_array()) {
        inputs = findObjectField(root, {"inputs", "vin"});
    }
    if (!inputs || !inputs->is_array() || inputs->empty()) {
        return;
    }

    const size_t inputIndex = findUnlockingInputIndex(root, currentTx);
    if (inputIndex >= inputs->size()) {
        result.warnings.push_back(
            "JSON 交易上下文 unlockingInputIndex 超出 inputs 范围，已忽略"
        );
        return;
    }

    const std::vector<uint8_t> fallbackTxid =
        preTx ? deriveTransactionId(*preTx) : std::vector<uint8_t>{};
    const std::vector<uint8_t> unlockingInput =
        unlockingInputDataFromInput((*inputs)[inputIndex], fallbackTxid);
    if (unlockingInput.empty()) {
        return;
    }

    addBvmAssignmentIfMissing(
        result,
        "unlockingInput",
        bytesToHex(unlockingInput)
    );
}

void addDerivedPreTxMetadata(
    RuntimeAssignmentSets& result,
    const std::string& txName,
    const json& tx
)
{
    addScalarParamIfPresent(
        result,
        txName + ".vlio",
        tx,
        {"vlio", "VLIO", "prefix", "header"}
    );
    const std::vector<uint8_t> explicitUnlockingScriptHash = objectFixedBytes(
        tx,
        {"unlockingScriptHash", "unlocking_script_hash", "scriptHash"},
        32,
        txName + ".unlockingScriptHash"
    );
    if (!explicitUnlockingScriptHash.empty()) {
        result.paramAssignments.push_back(
            txName + ".unlockingScriptHash=" +
            bytesToHex(explicitUnlockingScriptHash)
        );
        return;
    }

    const std::vector<uint8_t> script = objectScalarBytes(
        tx,
        {"unlockingScript", "unlocking_script", "scriptSig"},
        "unlockingScript"
    );
    if (!script.empty()) {
        result.paramAssignments.push_back(
            txName + ".unlockingScriptHash=" +
            bytesToHex(apc_crypto::sha256Digest(script))
        );
    }
}

void addDerivedTransactionAssignments(
    RuntimeAssignmentSets& result,
    const std::string& txName,
    const json& tx
)
{
    if (!tx.is_object()) {
        return;
    }

    const json* inputs = findObjectField(tx, {"inputs", "vin"});
    if (inputs && inputs->is_array()) {
        addBvmAssignmentIfMissing(
            result,
            "inputCount",
            std::to_string(inputs->size())
        );

        const std::vector<uint8_t> inputsHash = deriveInputsHash(*inputs);
        if (!inputsHash.empty()) {
            addBvmAssignmentIfMissing(
                result,
                "inputsHash",
                bytesToHex(inputsHash)
            );
        }

        addDerivedInputAssignments(result, txName, *inputs);
    }

    const json* outputs = findObjectField(tx, {"outputs", "vout"});
    if (outputs && outputs->is_array()) {
        addBvmAssignmentIfMissing(
            result,
            "outputCount",
            std::to_string(outputs->size())
        );

        addDerivedOutputAssignments(result, txName, *outputs);

        const std::vector<uint8_t> outputsHash = deriveOutputsHash(*outputs);
        if (!outputsHash.empty()) {
            addBvmAssignmentIfMissing(
                result,
                "outputsHash",
                bytesToHex(outputsHash)
            );
        }
    }

    addDerivedPreTxMetadata(result, txName, tx);
    addDerivedTransactionIdAssignments(result, txName, tx);
}

void flattenJsonAssignments(
    const json& value,
    const std::string& prefix,
    std::vector<std::string>& outAssignments
)
{
    if (value.is_null()) {
        return;
    }

    if (value.is_object()) {
        for (const auto& [fieldName, fieldValue] : value.items()) {
            const std::string childPrefix =
                prefix.empty() ? fieldName : prefix + "." + fieldName;
            flattenJsonAssignments(fieldValue, childPrefix, outAssignments);
        }
        return;
    }

    if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i) {
            const std::string childPrefix =
                prefix + "[" + std::to_string(i) + "]";
            flattenJsonAssignments(value[i], childPrefix, outAssignments);
        }
        return;
    }

    if (prefix.empty()) {
        throw std::runtime_error(
            "JSON transaction context cannot flatten a scalar without a path"
        );
    }

    outAssignments.push_back(
        prefix + "=" + jsonScalarToRuntimeText(value, prefix)
    );
}

void flattenJsonParameters(
    const json& params,
    RuntimeAssignmentSets& result
)
{
    if (!params.is_object()) {
        throw std::runtime_error(
            "JSON transaction context field 'params' must be an object"
        );
    }

    for (const auto& [paramName, paramValue] : params.items()) {
        flattenJsonAssignments(
            paramValue,
            paramName,
            result.paramAssignments
        );
    }
}

void addJsonTopLevelField(
    RuntimeAssignmentSets& result,
    const std::string& key,
    const json& value
)
{
    const std::string lowerKey = toLower(key);

    if (lowerKey == "bvm") {
        flattenJsonAssignments(value, "", result.bvmAssignments);
        return;
    }
    if (lowerKey == "self") {
        flattenJsonAssignments(value, "", result.selfAssignments);
        return;
    }
    if (lowerKey == "params" || lowerKey == "parameters" ||
        lowerKey == "param") {
        flattenJsonParameters(value, result);
        return;
    }

    if (isJsonScalar(value)) {
        const std::string runtimeValue =
            jsonScalarToRuntimeText(value, key);
        if (addLegacyTransactionKey(result, lowerKey, runtimeValue)) {
            return;
        }

        if (key.find('.') != std::string::npos ||
            key.find('[') != std::string::npos) {
            result.paramAssignments.push_back(key + "=" + runtimeValue);
            return;
        }

        result.warnings.push_back(
            "JSON 交易上下文未知标量键 '" + key + "'，已忽略"
        );
        return;
    }

    flattenJsonAssignments(value, key, result.paramAssignments);
}

RuntimeAssignmentSets parseJsonTransactionContext(
    const std::string& content,
    const std::string& filename
)
{
    RuntimeAssignmentSets result;
    json root;
    try {
        root = json::parse(content);
    } catch (const json::parse_error& e) {
        throw std::runtime_error(
            "无法解析 JSON 交易上下文文件 '" + filename + "': " + e.what()
        );
    }

    if (!root.is_object()) {
        throw std::runtime_error(
            "JSON 交易上下文文件必须是对象: " + filename
        );
    }

    for (const auto& [key, value] : root.items()) {
        addJsonTopLevelField(result, key, value);
    }

    const json* current = findObjectField(
        root,
        {"currenttx", "currentTx", "ctx", "current"}
    );
    const json* pretx = findObjectField(
        root,
        {"pretx", "preTx", "previousTx", "previous"}
    );

    if (current) {
        addDerivedTransactionAssignments(result, "currenttx", *current);
    }
    if (pretx) {
        addDerivedTransactionAssignments(result, "pretx", *pretx);
    }

    if (findObjectField(root, {"outputs", "vout"}) ||
        findObjectField(root, {"inputs", "vin"})) {
        addDerivedTransactionAssignments(result, "currenttx", root);
        if (!current) {
            current = &root;
        }
    }

    addDerivedUnlockingInputAssignment(result, root, current, pretx);

    return result;
}

RuntimeAssignmentSets parseTextTransactionContext(
    const std::string& content
)
{
    RuntimeAssignmentSets result;
    std::istringstream input(content);

    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;

        const size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }

        line = trim(line);
        if (line.empty()) {
            continue;
        }

        size_t delimiter = line.find(':');
        if (delimiter == std::string::npos) {
            delimiter = line.find('=');
        }

        if (delimiter == std::string::npos || delimiter == 0) {
            result.warnings.push_back(
                "交易上下文第 " + std::to_string(lineNumber) +
                " 行格式无效，已忽略"
            );
            continue;
        }

        const std::string key = trim(line.substr(0, delimiter));
        const std::string value = trim(line.substr(delimiter + 1));
        const std::string lowerKey = toLower(key);

        if (addLegacyTransactionKey(result, lowerKey, value)) {
            continue;
        }

        if (startsWith(lowerKey, "bvm.")) {
            result.bvmAssignments.push_back(key.substr(4) + "=" + value);
            continue;
        }
        if (startsWith(lowerKey, "self.")) {
            result.selfAssignments.push_back(key.substr(5) + "=" + value);
            continue;
        }
        if (startsWith(lowerKey, "param.")) {
            result.paramAssignments.push_back(key.substr(6) + "=" + value);
            continue;
        }

        if (key.find('.') != std::string::npos ||
            key.find('[') != std::string::npos) {
            result.paramAssignments.push_back(key + "=" + value);
            continue;
        }

        result.warnings.push_back(
            "交易上下文第 " + std::to_string(lineNumber) +
            " 行未知键 '" + key + "'，已忽略"
        );
    }

    return result;
}

} // namespace

RuntimeAssignmentSets loadRuntimeAssignmentsFromTxFile(
    const std::string& filename
)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("无法打开交易上下文文件: " + filename);
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();
    const std::string trimmed = trim(content);
    if (trimmed.empty()) {
        return {};
    }

    if (trimmed.front() == '{') {
        return parseJsonTransactionContext(content, filename);
    }

    return parseTextTransactionContext(content);
}

} // namespace apc_interpreter

#include "stack_trace.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace apc_debug
{
namespace
{

json sourceLocationToJson(const SourceLocation& loc)
{
    json locJson;
    locJson["file"] = loc.filename;
    locJson["line"] = loc.line;
    locJson["column"] = loc.column;
    locJson["endLine"] = loc.endLine;
    locJson["endColumn"] = loc.endColumn;
    return locJson;
}

std::vector<std::string> splitLines(const std::string& sourceCode)
{
    std::vector<std::string> lines;
    std::istringstream input(sourceCode);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    if (!sourceCode.empty() && sourceCode.back() == '\n') {
        lines.push_back("");
    }
    return lines;
}

bool isPrintableAscii(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty()) {
        return false;
    }
    return std::all_of(bytes.begin(), bytes.end(), [](uint8_t c) {
        return c >= 0x20 && c <= 0x7e;
    });
}

std::string toAsciiString(const std::vector<uint8_t>& bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

json stackElementToJson(
    const StackElement& element,
    size_t index,
    size_t stackSize
)
{
    json elemJson;
    elemJson["index"] = index;
    elemJson["depth"] = stackSize == 0 ? 0 : stackSize - index - 1;
    elemJson["hex"] = element.toHexString(true);
    elemJson["byteLength"] = element.data.size();

    if (auto intValue = element.toInt()) {
        elemJson["int"] = *intValue;
        elemJson["intString"] = std::to_string(*intValue);
    }

    if (isPrintableAscii(element.data)) {
        elemJson["ascii"] = toAsciiString(element.data);
    }

    return elemJson;
}

json stackToJson(const std::vector<StackElement>& stack)
{
    json stackJson = json::array();
    for (size_t i = 0; i < stack.size(); ++i) {
        stackJson.push_back(stackElementToJson(stack[i], i, stack.size()));
    }
    return stackJson;
}

std::string elementKey(const StackElement& element)
{
    return element.toHexString(true);
}

std::vector<StackElement> difference(
    const std::vector<StackElement>& left,
    const std::vector<StackElement>& right
)
{
    std::map<std::string, size_t> remaining;
    for (const auto& element : right) {
        remaining[elementKey(element)]++;
    }

    std::vector<StackElement> result;
    for (const auto& element : left) {
        const std::string key = elementKey(element);
        auto it = remaining.find(key);
        if (it != remaining.end() && it->second > 0) {
            it->second--;
        } else {
            result.push_back(element);
        }
    }
    return result;
}

bool stacksEqual(
    const std::vector<StackElement>& left,
    const std::vector<StackElement>& right
)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].data != right[i].data) {
            return false;
        }
    }
    return true;
}

bool sameMultiset(
    const std::vector<StackElement>& left,
    const std::vector<StackElement>& right
)
{
    std::map<std::string, size_t> leftCounts;
    std::map<std::string, size_t> rightCounts;
    for (const auto& element : left) {
        leftCounts[elementKey(element)]++;
    }
    for (const auto& element : right) {
        rightCounts[elementKey(element)]++;
    }
    return leftCounts == rightCounts;
}

json moveToJson(
    const std::string& from,
    const std::string& to,
    const StackElement& element
)
{
    json moveJson;
    moveJson["from"] = from;
    moveJson["to"] = to;
    moveJson["element"] = stackElementToJson(element, 0, 1);
    return moveJson;
}

std::vector<StackElement> removeMovedElements(
    std::vector<StackElement>& popped,
    std::vector<StackElement>& pushed,
    const std::string& from,
    const std::string& to,
    json& moves
)
{
    std::vector<bool> pushedUsed(pushed.size(), false);
    std::vector<StackElement> remainingPopped;

    for (const auto& poppedElement : popped) {
        const std::string key = elementKey(poppedElement);
        bool matched = false;
        for (size_t i = 0; i < pushed.size(); ++i) {
            if (!pushedUsed[i] && elementKey(pushed[i]) == key) {
                pushedUsed[i] = true;
                moves.push_back(moveToJson(from, to, poppedElement));
                matched = true;
                break;
            }
        }

        if (!matched) {
            remainingPopped.push_back(poppedElement);
        }
    }

    std::vector<StackElement> remainingPushed;
    for (size_t i = 0; i < pushed.size(); ++i) {
        if (!pushedUsed[i]) {
            remainingPushed.push_back(pushed[i]);
        }
    }
    pushed = std::move(remainingPushed);
    return remainingPopped;
}

json stackEffectToJson(
    const std::vector<StackElement>& before,
    const std::vector<StackElement>& after,
    const std::vector<StackElement>& pushed,
    const std::vector<StackElement>& popped
)
{
    json effect;
    effect["pushed"] = stackToJson(pushed);
    effect["popped"] = stackToJson(popped);
    effect["sizeBefore"] = before.size();
    effect["sizeAfter"] = after.size();
    effect["reordered"] =
        !stacksEqual(before, after) && sameMultiset(before, after);
    return effect;
}

json effectsToJson(const StackTraceStep& step)
{
    std::vector<StackElement> poppedMain =
        difference(step.mainStackBefore, step.mainStackAfter);
    std::vector<StackElement> pushedMain =
        difference(step.mainStackAfter, step.mainStackBefore);
    std::vector<StackElement> poppedAlt =
        difference(step.altStackBefore, step.altStackAfter);
    std::vector<StackElement> pushedAlt =
        difference(step.altStackAfter, step.altStackBefore);

    json moves = json::array();
    poppedMain =
        removeMovedElements(poppedMain, pushedAlt, "main", "alt", moves);
    poppedAlt =
        removeMovedElements(poppedAlt, pushedMain, "alt", "main", moves);

    json effects;
    effects["mainStack"] = stackEffectToJson(
        step.mainStackBefore,
        step.mainStackAfter,
        pushedMain,
        poppedMain
    );
    effects["altStack"] = stackEffectToJson(
        step.altStackBefore,
        step.altStackAfter,
        pushedAlt,
        poppedAlt
    );
    effects["moves"] = moves;
    return effects;
}

struct ElementLife
{
    size_t id = 0;
    std::string hex;
    size_t originStep = 0;
    std::string originStack;
    size_t lastStep = 0;
    std::string lastStack;
    bool consumed = false;
    size_t consumedAt = 0;
    json events = json::array();
};

struct StackSlot
{
    size_t id = 0;
    std::string stack;
    size_t index = 0;
};

std::string elementIdString(size_t id)
{
    return "e" + std::to_string(id);
}

bool containsId(const std::vector<size_t>& ids, size_t id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

std::vector<size_t> idsNotIn(
    const std::vector<size_t>& left,
    const std::vector<size_t>& right
)
{
    std::vector<size_t> out;
    for (const size_t id : left) {
        if (!containsId(right, id)) {
            out.push_back(id);
        }
    }
    return out;
}

class LifecycleAnnotator
{
public:
    json annotate(const std::vector<StackTraceStep>& steps, json& stepsJson)
    {
        for (size_t i = 0; i < steps.size(); ++i) {
            annotateStep(steps[i], stepsJson[i]);
        }
        return lifecycleJson();
    }

private:
    size_t m_nextId = 1;
    std::map<size_t, ElementLife> m_lives;
    std::vector<size_t> m_mainIds;
    std::vector<size_t> m_altIds;

    size_t createId(
        const StackElement& element,
        const StackTraceStep& step,
        const std::string& stack,
        const std::string& eventType
    )
    {
        const size_t id = m_nextId++;
        ElementLife life;
        life.id = id;
        life.hex = elementKey(element);
        life.originStep = step.stepIndex;
        life.originStack = stack;
        life.lastStep = step.stepIndex;
        life.lastStack = stack;
        m_lives[id] = std::move(life);
        addEvent(id, step, eventType, stack, "", "");
        return id;
    }

    void addEvent(
        size_t id,
        const StackTraceStep& step,
        const std::string& type,
        const std::string& stack,
        const std::string& from,
        const std::string& to
    )
    {
        json event;
        event["type"] = type;
        event["step"] = step.stepIndex;
        event["pc"] = step.pc;
        event["opcode"] = step.opcode;
        event["sourceFile"] = step.location.filename;
        event["sourceLine"] = step.location.line;
        if (!stack.empty()) {
            event["stack"] = stack;
        }
        if (!from.empty()) {
            event["from"] = from;
        }
        if (!to.empty()) {
            event["to"] = to;
        }
        m_lives[id].events.push_back(event);
    }

    void annotateStep(const StackTraceStep& step, json& stepJson)
    {
        syncBeforeStack(m_mainIds, step.mainStackBefore, step, "main");
        syncBeforeStack(m_altIds, step.altStackBefore, step, "alt");

        annotateStack(
            stepJson["mainStackBefore"],
            m_mainIds
        );
        annotateStack(stepJson["mainStack"]["before"], m_mainIds);
        annotateStack(
            stepJson["altStackBefore"],
            m_altIds
        );
        annotateStack(stepJson["altStack"]["before"], m_altIds);

        const std::vector<size_t> beforeMain = m_mainIds;
        const std::vector<size_t> beforeAlt = m_altIds;
        std::set<size_t> usedBeforeIds;
        std::vector<size_t> afterMain = matchAfterStack(
            step.mainStackAfter,
            beforeMain,
            step.mainStackBefore,
            beforeAlt,
            step.altStackBefore,
            usedBeforeIds,
            step,
            "main"
        );
        std::vector<size_t> afterAlt = matchAfterStack(
            step.altStackAfter,
            beforeAlt,
            step.altStackBefore,
            beforeMain,
            step.mainStackBefore,
            usedBeforeIds,
            step,
            "alt"
        );

        updateLives(step, beforeMain, beforeAlt, afterMain, afterAlt);

        annotateStack(stepJson["mainStackAfter"], afterMain);
        annotateStack(stepJson["mainStack"]["after"], afterMain);
        annotateStack(stepJson["altStackAfter"], afterAlt);
        annotateStack(stepJson["altStack"]["after"], afterAlt);

        annotateEffects(stepJson["effects"], beforeMain, beforeAlt, afterMain, afterAlt);

        m_mainIds = std::move(afterMain);
        m_altIds = std::move(afterAlt);
    }

    void syncBeforeStack(
        std::vector<size_t>& ids,
        const std::vector<StackElement>& stack,
        const StackTraceStep& step,
        const std::string& stackName
    )
    {
        if (ids.size() == stack.size()) {
            bool matches = true;
            for (size_t i = 0; i < stack.size(); ++i) {
                if (m_lives[ids[i]].hex != elementKey(stack[i])) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                return;
            }
        }

        std::vector<size_t> synced;
        std::set<size_t> used;
        for (size_t i = 0; i < stack.size(); ++i) {
            const std::string key = elementKey(stack[i]);
            size_t matchedId = 0;
            if (i < ids.size() && used.count(ids[i]) == 0 &&
                m_lives[ids[i]].hex == key) {
                matchedId = ids[i];
            } else {
                for (const size_t id : ids) {
                    if (used.count(id) == 0 && m_lives[id].hex == key) {
                        matchedId = id;
                        break;
                    }
                }
            }

            if (matchedId == 0) {
                matchedId = createId(stack[i], step, stackName, "initial");
            }
            used.insert(matchedId);
            synced.push_back(matchedId);
        }
        ids = std::move(synced);
    }

    std::vector<StackSlot> slotsFor(
        const std::vector<size_t>& sameIds,
        const std::string& sameStack,
        const std::vector<size_t>& otherIds,
        const std::string& otherStack
    ) const
    {
        std::vector<StackSlot> slots;
        for (size_t i = 0; i < sameIds.size(); ++i) {
            slots.push_back({sameIds[i], sameStack, i});
        }
        for (size_t i = 0; i < otherIds.size(); ++i) {
            slots.push_back({otherIds[i], otherStack, i});
        }
        return slots;
    }

    size_t findSlot(
        const std::vector<StackSlot>& slots,
        const std::set<size_t>& used,
        const std::string& key,
        const std::string& stackName,
        size_t preferredIndex,
        int pass
    ) const
    {
        for (const auto& slot : slots) {
            if (used.count(slot.id) != 0 || m_lives.at(slot.id).hex != key) {
                continue;
            }
            if (pass == 0 && slot.stack == stackName &&
                slot.index == preferredIndex) {
                return slot.id;
            }
            if (pass == 1 && slot.stack == stackName) {
                return slot.id;
            }
            if (pass == 2 && slot.stack != stackName) {
                return slot.id;
            }
        }
        return 0;
    }

    std::vector<size_t> matchAfterStack(
        const std::vector<StackElement>& after,
        const std::vector<size_t>& sameBeforeIds,
        const std::vector<StackElement>& /*sameBefore*/,
        const std::vector<size_t>& otherBeforeIds,
        const std::vector<StackElement>& /*otherBefore*/,
        std::set<size_t>& usedBeforeIds,
        const StackTraceStep& step,
        const std::string& stackName
    )
    {
        const std::string otherStack = stackName == "main" ? "alt" : "main";
        const std::vector<StackSlot> slots =
            slotsFor(sameBeforeIds, stackName, otherBeforeIds, otherStack);
        std::vector<size_t> afterIds;
        for (size_t i = 0; i < after.size(); ++i) {
            const std::string key = elementKey(after[i]);
            size_t id = 0;
            for (int pass = 0; pass < 3 && id == 0; ++pass) {
                id = findSlot(slots, usedBeforeIds, key, stackName, i, pass);
            }
            if (id == 0) {
                id = createId(after[i], step, stackName, "push");
            } else {
                usedBeforeIds.insert(id);
            }
            afterIds.push_back(id);
        }
        return afterIds;
    }

    std::map<size_t, std::string> stackMap(
        const std::vector<size_t>& mainIds,
        const std::vector<size_t>& altIds
    ) const
    {
        std::map<size_t, std::string> map;
        for (const size_t id : mainIds) {
            map[id] = "main";
        }
        for (const size_t id : altIds) {
            map[id] = "alt";
        }
        return map;
    }

    void updateLives(
        const StackTraceStep& step,
        const std::vector<size_t>& beforeMain,
        const std::vector<size_t>& beforeAlt,
        const std::vector<size_t>& afterMain,
        const std::vector<size_t>& afterAlt
    )
    {
        const std::map<size_t, std::string> before =
            stackMap(beforeMain, beforeAlt);
        const std::map<size_t, std::string> after =
            stackMap(afterMain, afterAlt);

        for (const auto& [id, afterStack] : after) {
            auto& life = m_lives[id];
            auto beforeIt = before.find(id);
            if (beforeIt != before.end() && beforeIt->second != afterStack) {
                addEvent(id, step, "move", "", beforeIt->second, afterStack);
            }
            life.lastStep = step.stepIndex;
            life.lastStack = afterStack;
        }

        for (const auto& [id, beforeStack] : before) {
            if (after.find(id) == after.end()) {
                auto& life = m_lives[id];
                life.consumed = true;
                life.consumedAt = step.stepIndex;
                life.lastStep = step.stepIndex;
                life.lastStack = beforeStack;
                addEvent(id, step, "pop", beforeStack, "", "");
            }
        }
    }

    void addElementFields(json& elementJson, size_t id) const
    {
        const auto& life = m_lives.at(id);
        elementJson["elementId"] = elementIdString(id);
        elementJson["originStep"] = life.originStep;
        elementJson["originStack"] = life.originStack;
    }

    void annotateStack(json& stackJson, const std::vector<size_t>& ids) const
    {
        if (!stackJson.is_array()) {
            return;
        }
        const size_t limit = std::min(stackJson.size(), ids.size());
        for (size_t i = 0; i < limit; ++i) {
            addElementFields(stackJson[i], ids[i]);
        }
    }

    void annotateEffectArray(json& array, const std::vector<size_t>& ids) const
    {
        if (!array.is_array()) {
            return;
        }
        std::set<size_t> used;
        for (auto& element : array) {
            const std::string key = element.value("hex", "");
            for (const size_t id : ids) {
                if (used.count(id) == 0 && m_lives.at(id).hex == key) {
                    addElementFields(element, id);
                    used.insert(id);
                    break;
                }
            }
        }
    }

    void annotateMoveArray(json& moves, const std::vector<size_t>& ids) const
    {
        if (!moves.is_array()) {
            return;
        }
        std::set<size_t> used;
        for (auto& move : moves) {
            if (!move.contains("element")) {
                continue;
            }
            const std::string key = move["element"].value("hex", "");
            for (const size_t id : ids) {
                if (used.count(id) == 0 && m_lives.at(id).hex == key) {
                    addElementFields(move["element"], id);
                    move["elementId"] = elementIdString(id);
                    used.insert(id);
                    break;
                }
            }
        }
    }

    void annotateEffects(
        json& effects,
        const std::vector<size_t>& beforeMain,
        const std::vector<size_t>& beforeAlt,
        const std::vector<size_t>& afterMain,
        const std::vector<size_t>& afterAlt
    ) const
    {
        const std::vector<size_t> pushedMain = idsNotIn(afterMain, beforeMain);
        const std::vector<size_t> poppedMain = idsNotIn(beforeMain, afterMain);
        const std::vector<size_t> pushedAlt = idsNotIn(afterAlt, beforeAlt);
        const std::vector<size_t> poppedAlt = idsNotIn(beforeAlt, afterAlt);

        annotateEffectArray(effects["mainStack"]["pushed"], pushedMain);
        annotateEffectArray(effects["mainStack"]["popped"], poppedMain);
        annotateEffectArray(effects["altStack"]["pushed"], pushedAlt);
        annotateEffectArray(effects["altStack"]["popped"], poppedAlt);

        const std::map<size_t, std::string> beforeMap =
            stackMap(beforeMain, beforeAlt);
        const std::map<size_t, std::string> afterMap =
            stackMap(afterMain, afterAlt);
        std::vector<size_t> moved;
        for (const auto& [id, beforeStack] : beforeMap) {
            auto afterIt = afterMap.find(id);
            if (afterIt != afterMap.end() && afterIt->second != beforeStack) {
                moved.push_back(id);
            }
        }
        annotateMoveArray(effects["moves"], moved);
    }

    json lifecycleJson() const
    {
        json lifecycle;
        lifecycle["idFormat"] = "e<number>";
        lifecycle["matching"] =
            "synthetic: prefer same stack/index, same stack/value, then cross-stack/value";
        json elements = json::array();
        for (const auto& [id, life] : m_lives) {
            json entry;
            entry["elementId"] = elementIdString(id);
            entry["hex"] = life.hex;
            entry["originStep"] = life.originStep;
            entry["originStack"] = life.originStack;
            entry["lastStep"] = life.lastStep;
            entry["lastStack"] = life.lastStack;
            entry["consumed"] = life.consumed;
            if (life.consumed) {
                entry["consumedAt"] = life.consumedAt;
            }
            entry["events"] = life.events;
            elements.push_back(entry);
        }
        lifecycle["elements"] = elements;
        return lifecycle;
    }
};

json stepToJson(const StackTraceStep& step)
{
    json stepJson;
    stepJson["step"] = step.stepIndex;
    stepJson["pc"] = step.pc;
    stepJson["instruction"] = step.instruction;
    stepJson["opcode"] = step.opcode;
    stepJson["operand"] = step.operand;
    stepJson["source"] = sourceLocationToJson(step.location);
    stepJson["sourceFile"] = step.location.filename;
    stepJson["sourceLine"] = step.location.line;
    stepJson["functionName"] = step.functionName;
    stepJson["mainStackBefore"] = stackToJson(step.mainStackBefore);
    stepJson["mainStackAfter"] = stackToJson(step.mainStackAfter);
    stepJson["altStackBefore"] = stackToJson(step.altStackBefore);
    stepJson["altStackAfter"] = stackToJson(step.altStackAfter);
    stepJson["mainStack"] = {
        {"before", stackToJson(step.mainStackBefore)},
        {"after", stackToJson(step.mainStackAfter)}
    };
    stepJson["altStack"] = {
        {"before", stackToJson(step.altStackBefore)},
        {"after", stackToJson(step.altStackAfter)}
    };
    stepJson["effects"] = effectsToJson(step);

    if (!step.errorMessage.empty()) {
        stepJson["error"] = step.errorMessage;
    }

    return stepJson;
}

json debugInfoSummaryToJson(const std::shared_ptr<DebugInfo>& debugInfo)
{
    json summary;
    if (!debugInfo) {
        return summary;
    }

    summary["sourceFile"] = debugInfo->sourceFilename;
    summary["contractName"] = debugInfo->contractName;

    json lineToPCJson = json::object();
    for (const auto& [line, pcs] : debugInfo->lineToPCs) {
        lineToPCJson[std::to_string(line)] = pcs;
    }
    summary["lineToPC"] = lineToPCJson;

    json functionsJson = json::array();
    for (const auto& [name, func] : debugInfo->functions) {
        json funcJson;
        funcJson["name"] = func.name;
        funcJson["startPC"] = func.startPC;
        funcJson["endPC"] = func.endPC;
        funcJson["isPublic"] = func.isPublic;
        funcJson["location"] = sourceLocationToJson(func.location);
        functionsJson.push_back(funcJson);
    }
    summary["functions"] = functionsJson;

    return summary;
}

} // namespace

void StackTraceRecorder::clear()
{
    m_steps.clear();
}

void StackTraceRecorder::record(StackTraceStep step)
{
    step.stepIndex = m_steps.size();
    m_steps.push_back(std::move(step));
}

std::string StackTraceRecorder::toJson(
    const std::vector<std::string>& bytecode,
    const std::shared_ptr<DebugInfo>& debugInfo,
    const std::string& sourceCode
) const
{
    json trace;
    trace["version"] = "1.0";
    trace["format"] = "apc-stack-trace";
    trace["stackOrder"] = "bottom-to-top";
    trace["top"] = "last element in each stack array";

    trace["debugInfo"] = debugInfoSummaryToJson(debugInfo);

    json sourceJson;
    sourceJson["file"] = debugInfo ? debugInfo->sourceFilename : "";
    sourceJson["lines"] = splitLines(sourceCode);
    trace["source"] = sourceJson;

    trace["bytecode"] = bytecode;

    json stepsJson = json::array();
    for (const auto& step : m_steps) {
        stepsJson.push_back(stepToJson(step));
    }
    LifecycleAnnotator lifecycleAnnotator;
    trace["lifecycle"] = lifecycleAnnotator.annotate(m_steps, stepsJson);
    trace["steps"] = stepsJson;

    return trace.dump(2);
}

bool StackTraceRecorder::save(
    const std::string& filename,
    const std::vector<std::string>& bytecode,
    const std::shared_ptr<DebugInfo>& debugInfo,
    const std::string& sourceCode
) const
{
    try {
        std::ofstream out(filename);
        if (!out.is_open()) {
            return false;
        }
        out << toJson(bytecode, debugInfo, sourceCode);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace apc_debug

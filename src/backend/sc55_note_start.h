#pragma once
#include "sc55_note_dispatch.h"
#include "sc55_voice_lifecycle.h"

namespace sc55
{
// Controller-derived words shared at5ff5..6049. These are not oscillator
// phase/depth state: paired voices retain their own patch-derived modulation.
struct VoiceControllerState
{
    uint16_t pitchOffset = 0, envelopeOffset = 0, levelBias = 0;
    std::array<uint16_t,2> rateModifiers{}, levelDepths{}, envelopeDepths{}, pitchDepths{};

    void apply(LevelInputs& level,SecondEnvelopeOutputInputs& envelope,
               PitchModulationInputs& pitch,ModulationBlock& first,ModulationBlock& second) const noexcept
    {
        const auto signedWord = [](uint16_t v) { return int16_t(v < 32768 ? int(v) : int(v)-65536); };
        pitch.offset = pitchOffset; envelope.offset = envelopeOffset; level.bias = signedWord(levelBias);
        first.rateModifier = signedWord(rateModifiers[0]); second.rateModifier = signedWord(rateModifiers[1]);
        level.mod1_b = signedWord(levelDepths[0]); level.mod2_b = signedWord(levelDepths[1]);
        for (unsigned i = 0; i < 2; ++i)
        { envelope.sources[i].second = envelopeDepths[i]; pitch.sources[i].second = pitchDepths[i]; }
    }
};

inline void CopyPairedVoiceControllers(VoiceControllerState& destination,
    const VoiceControllerState& source) noexcept
{
    destination = source;
}

struct VoiceControllerInputs
{
    uint8_t keyValue = 0; // selected per-part/per-original-key RAM byte
    // Order: pitch offset, envelope offset, level bias, first rate/pitch/env/level,
    // second rate/pitch/env/level. No assumption about GS defaults/producers.
    std::array<uint8_t,11> sensitivity{};
    std::array<std::array<uint16_t,11>,5> contributions{};
};

// 2666..2689: expand a controller byte into the eleven contribution words.
// Compact sensitivities omit the unused fourth byte of the firmware row.
// Negative halves truncate magnitude before restoring sign (not signed >>).
inline std::array<uint16_t,11> PrepareControllerContribution(uint8_t value,
    std::span<const uint8_t,11> sensitivity) noexcept
{
    std::array<uint16_t,11> result{};
    for (unsigned i = 0; i < result.size(); ++i)
    {
        const bool centered = i <= 3 || i == 7;
        const int parameter = centered ? int(sensitivity[i])-64 : sensitivity[i];
        const unsigned magnitude = unsigned(parameter < 0 ? -parameter : parameter);
        const unsigned shift = i == 0 ? 0 : centered ? 1 : 2;
        const auto product = uint16_t((magnitude*value)>>shift);
        result[i] = parameter < 0 ? uint16_t(0u-product) : product;
    }
    return result;
}

// Mutable input tables owned by the synthesis thread. Defaults are zeroed
// storage, not a complete GS reset image. Configuration/CC producers populate
// the sensitivities and contribution rows independently of per-key pressure.
struct PartControllerState
{
    struct Part
    {
        std::array<uint8_t,128> keyPressure{};
        std::array<uint8_t,11> sensitivity{};
        std::array<std::array<uint8_t,11>,5> sourceSensitivity{};
        std::array<uint8_t,2> assignedControllers{}; // part+26/+27; configuration, not GS defaults
        std::array<std::array<uint16_t,11>,5> contributions{};
    };
    std::array<Part,16> parts{};

    // 2390..23f7 and2401/2441/2481: full fourteen-bit bend contribution.
    // Bend depths use reversed polarity; centered sensitivities use normal
    // polarity. Preserve the intermediate byte truncation before coefficient
    // multiplication, rather than combining these into one floating scale.
    bool receivePitchBend(const MidiDecoder::Event& event,
                          std::span<const PartMidiReceive,16> routing) noexcept
    {
        if (event.kind != MidiDecoder::Kind::message || (event.status&0xf0) != 0xe0
            || event.dataSize != 2 || event.first > 127 || event.second > 127) return false;
        const int bend = std::max(1,int(event.first)+(int(event.second)<<7))-8192;
        const unsigned magnitude = unsigned(bend < 0 ? -bend : bend)*4;
        for (unsigned n = 16; n > 0; --n)
        {
            const auto part = n-1;
            if (routing[part].channel != (event.status&15) || !(routing[part].flags&0x4000)) continue;
            for (unsigned i = 0; i < 11; ++i)
            {
                const bool centered = i <= 3 || i == 7;
                const int parameter = int(parts[part].sourceSensitivity[1][i])-(centered ? 64 : 0);
                const unsigned absolute = unsigned(parameter < 0 ? -parameter : parameter);
                const auto intermediate = uint16_t((absolute*magnitude)>>8);
                const unsigned coefficient = i == 0 ? 0xfe16 : centered ? 0x7f00 : 0x3f81;
                const auto value = uint16_t((uint32_t(intermediate)*coefficient)>>16);
                const bool negative = centered ? ((parameter < 0) != (bend < 0)) : bend >= 0;
                parts[part].contributions[1][i] = negative ? uint16_t(0u-value) : value;
            }
        }
        return true;
    }

    // Only the contribution-row side of Bn; volume/pedals/RPN/channel-mode
    // handling remains the caller's responsibility, even when this returns true.
    bool receiveControlContributions(const MidiDecoder::Event& event,
                                    std::span<const PartMidiReceive,16> routing) noexcept
    {
        if (event.kind != MidiDecoder::Kind::message || (event.status&0xf0) != 0xb0
            || event.dataSize != 2 || event.first > 127 || event.second > 127) return false;
        const auto enabled = [&](unsigned part) {
            return routing[part].channel == (event.status&15) && (routing[part].flags&0x800);
        };
        const auto update = [&](unsigned part,unsigned source) {
            parts[part].contributions[source] = PrepareControllerContribution(
                event.second,parts[part].sourceSensitivity[source]);
        };
        if (event.first == 1)
            for (unsigned n = 16; n > 0; --n)
                if (enabled(n-1) && (routing[n-1].flags&2)) update(n-1,0);
        if (event.first >= 121) return true; // 22e9 excludes channel-mode CC121..127
        for (unsigned n = 16; n > 0; --n)
            if (enabled(n-1) && parts[n-1].assignedControllers[0] == event.first) update(n-1,3);
        unsigned assignment = 1;
        for (unsigned n = 16; n > 0; --n)
        {
            const auto part = n-1;
            if (!enabled(part)) continue;
            if (parts[part].assignedControllers[assignment] == event.first) update(part,3+assignment);
            // v1.21's 2646 branches to2623, not265e: after the first
            // enabled mismatch, lower parts resume the FIRST assignment loop.
            else if (assignment == 1) assignment = 0;
        }
        return true;
    }

    // 2346..2378: channel pressure uses receive bit13, not the CC/note gates.
    bool receiveChannelPressure(const MidiDecoder::Event& event,
                               std::span<const PartMidiReceive,16> routing) noexcept
    {
        if (event.kind != MidiDecoder::Kind::message || (event.status&0xf0) != 0xd0
            || event.dataSize != 1 || event.first > 127) return false;
        for (unsigned n = 16; n > 0; --n)
        {
            const auto part = n-1;
            if (routing[part].channel == (event.status&15) && (routing[part].flags&0x2000))
                parts[part].contributions[2] = PrepareControllerContribution(
                    event.first,parts[part].sourceSensitivity[2]);
        }
        return true;
    }

    // 2218..223b: polyphonic key pressure, channel equality and receive bit10.
    // This is not note routing: it does not apply note-on/off mode gates.
    bool receivePolyPressure(const MidiDecoder::Event& event,
                            std::span<const PartMidiReceive,16> routing) noexcept
    {
        if (event.kind != MidiDecoder::Kind::message || (event.status&0xf0) != 0xa0
            || event.dataSize != 2 || event.first > 127 || event.second > 127) return false;
        for (unsigned n = 16; n > 0; --n)
        {
            const auto part = n-1;
            if (routing[part].channel == (event.status&15) && (routing[part].flags&0x400))
                parts[part].keyPressure[event.first] = event.second;
        }
        return true;
    }

    std::optional<VoiceControllerInputs> inputs(unsigned part,unsigned originalKey) const noexcept
    {
        if (part >= 16 || originalKey >= 128) return std::nullopt;
        const auto& state = parts[part];
        return VoiceControllerInputs{state.keyPressure[originalKey],state.sensitivity,state.contributions};
    }
};

// Complete5c46..5ff4 arithmetic after the caller selects the part and key byte.
// Preserve byte subtraction, signed-magnitude truncation, five wrapping word
// additions, then saturation and high-product-word scaling, in that order.
inline VoiceControllerState PrepareVoiceControllers(const VoiceControllerInputs& input) noexcept
{
    std::array<uint16_t,11> values{};
    for (unsigned i = 0; i < values.size(); ++i)
    {
        const bool centered = i <= 3 || i == 7;
        const bool negativeInput = centered && input.sensitivity[i] < 64;
        const unsigned magnitude = centered ? (negativeInput ? 64-input.sensitivity[i] : input.sensitivity[i]-64)
                                             : input.sensitivity[i];
        const unsigned shift = i == 0 ? 0 : centered ? 1 : 2;
        const auto product = uint16_t((magnitude*input.keyValue)>>shift);
        uint16_t sum = negativeInput ? uint16_t(0u-product) : product;
        for (const auto& contribution : input.contributions) sum = uint16_t(sum+contribution[i]);
        const bool negative = (sum&32768) != 0;
        const uint16_t absolute = negative ? uint16_t(0u-sum) : sum;
        const bool rate = i == 3 || i == 7;
        const bool level = i == 2 || i == 6 || i == 10;
        const bool envelope = i == 5 || i == 9;
        const unsigned cap = i == 0 ? 0x0be8 : i <= 2 || rate ? 0x0fa0 : 0x0fc0;
        const unsigned leftShift = i <= 1 ? 3 : level ? 4 : 1;
        const unsigned coefficient = i == 0 ? 0xfbf8 : i == 1 ? 0xc49c : i == 2 ? 0x820d
            : rate ? 0xa7c7 : level ? 0x8105 : envelope ? 0xc30d : 0xbe7a;
        const auto result = uint16_t((uint32_t(uint16_t(std::min(unsigned(absolute),cap)<<leftShift))*coefficient)>>16);
        values[i] = negative ? uint16_t(0u-result) : result;
    }
    return {values[0],values[1],values[2],{values[3],values[7]},
        {values[6],values[10]},{values[5],values[9]},{values[4],values[8]}};
}

// 5b11..5b73/5bb4: choose the next periodic update, independently of the
// startup-task dispatcher. The update delegates own the per-voice visited byte.
struct VoiceUpdateSelection
{
    std::array<uint8_t,2> slots{255,255};
    uint8_t count = 0, resume = 255;
};

inline std::optional<VoiceUpdateSelection> SelectNextVoiceUpdate(uint8_t start,
    std::span<const uint16_t,24> stages,std::span<uint8_t,24> visited,
    const VoiceLinks& links) noexcept
{
    if (start >= 24 && start != 255) return std::nullopt;
    // 255 resumes after slot0: finish this pass and clear all visited bytes.
    for (unsigned n = start == 255 ? 0 : unsigned(start)+1; n > 0; --n)
    {
        const auto slot = uint8_t(n-1);
        if (stages[slot] >= 18 || visited[slot] != 0) continue;
        VoiceUpdateSelection result{{slot,255},1,uint8_t(slot == 0 ? 255 : slot-1)};
        if (links.first[slot] != 255)
        {
            if (links.first[slot] >= 24) return std::nullopt;
            result.slots = {links.first[slot],slot}; result.count = 2;
        }
        else if (links.second[slot] != 255)
        {
            if (links.second[slot] >= 24) return std::nullopt;
            result.slots[1] = links.second[slot]; result.count = 2;
        }
        return result;
    }
    for (auto& value : visited) value = 0;
    return VoiceUpdateSelection{};
}

// Controller phase of5b73..5b89 / 5bb4..5bdb. The first selected voice's
// installed part/original key is authoritative even if the partner differs.
// Do not touch oscillator/envelope state, visited flags or scheduling here.
inline bool RefreshSelectedVoiceControllers(const VoiceUpdateSelection& selection,
    const VoiceInstallationState& installed,const PartControllerState& parts,
    std::span<VoiceControllerState,24> controllers) noexcept
{
    if (selection.count > 2) return false;
    if (!selection.count) return true;
    for (unsigned i = 0; i < selection.count; ++i)
        if (selection.slots[i] >= 24) return false;
    const auto first = selection.slots[0];
    const auto& voice = installed.voices[first].input;
    const auto input = parts.inputs(voice.part,voice.originalKey);
    if (!input) return false;
    const auto result = PrepareVoiceControllers(*input);
    controllers[first] = result;
    if (selection.count == 2) CopyPairedVoiceControllers(controllers[selection.slots[1]],result);
    return true;
}

// Serialized 5b11..5c1d pass orchestration. Delegates operate on stable voice
// owners and must not reenter; their internal DSP state need not be copied.
// A late failure may have advanced DSP/PCM, so poison the pass until the owner
// explicitly resets it rather than replaying partially executed work.
class PeriodicVoiceUpdatePass
{
public:
    enum class Result { invalidInput, updated, complete };
    enum class UpdateResult { invalidInput, proceed, skipRemaining };
    void reset() noexcept { cursor_ = 23; visited_.fill(0); failed_ = complete_ = false; }
    uint8_t cursor() const noexcept { return cursor_; }
    const std::array<uint8_t,24>& visited() const noexcept { return visited_; }

    template<class Controllers,class First,class PairedFirst,class Update,class Write>
    Result step(std::span<const uint16_t,24> stages,const VoiceLinks& links,
        Controllers&& controllers,First&& first,PairedFirst&& pairedFirst,
        Update&& update,Write&& write)
    {
        if (failed_) return Result::invalidInput;
        if (complete_) return Result::complete;
        const auto selected = SelectNextVoiceUpdate(cursor_,stages,visited_,links);
        const auto fail = [&] { failed_ = true; return Result::invalidInput; };
        if (!selected) return fail();
        if (!selected->count) { complete_ = true; return Result::complete; }
        if (!controllers(*selected) || !first(selected->slots[0])) return fail();
        if (selected->count == 2 && !pairedFirst(selected->slots[1],selected->slots[0])) return fail();
        for (unsigned i = 0; i < selected->count; ++i)
        {
            const auto slot = selected->slots[i];
            visited_[slot] = 255; // 3190: happens before the voice-stage gate
            const auto outcome = update(slot);
            if (outcome == UpdateResult::invalidInput) return fail();
            // 33e7..33f1 discards the 3188 return address and resumes the
            // scan at5b5c: no remaining paired DSP or final PCM writes.
            if (outcome == UpdateResult::skipRemaining)
            {
                cursor_ = selected->resume;
                return Result::updated;
            }
        }
        // Both updates precede either write for a pair, including self pairs.
        for (unsigned i = 0; i < selected->count; ++i)
            if (!write(selected->slots[i])) return fail();
        cursor_ = selected->resume;
        return Result::updated;
    }
private:
    std::array<uint8_t,24> visited_{};
    uint8_t cursor_ = 23;
    bool failed_ = false, complete_ = false;
};

struct VoicePreparationContext
{
    uint8_t slot, part, partial, flags;
    uint16_t tone, sample;
    // 5639's +30 pointer represented as a selected per-key control table.
    // These RAM tables are not wave/sample ROM and their semantics are not
    // inferred from their addresses. The caller owns their current contents.
    enum class KeyTable { none, first, second } keyTable;
    uint8_t keyIndex;
};

// 5639..56b4 metadata expansion. IDs replace firmware bank/pointer copies;
// the immutable sound-data generation must outlive all consumers. Activity is
// set only by flag7; a normal non-restarted preparation preserves its old value.
inline std::optional<VoicePreparationContext> PrepareVoiceContext(unsigned slot,
    const InstalledVoice& installed,uint8_t& activity) noexcept
{
    const auto& input = installed.input;
    if (slot >= 24 || input.part >= 16 || input.partial >= 2 || (input.sample&0x8000)) return std::nullopt;
    using Table = VoicePreparationContext::KeyTable;
    const auto table = input.sampleMode&128 ? Table::none : input.sampleMode == 0 ? Table::first : Table::second;
    VoicePreparationContext result{uint8_t(slot),input.part,input.partial,installed.flags,
        input.tone,input.sample,table,input.sampleKey};
    if (installed.flags&128) activity = 255;
    return result;
}

struct VoiceTaskDispatch
{
    enum class Kind { idle, prepare, finishStop } kind = Kind::idle;
    std::array<uint8_t,2> slots{255,255};
    uint8_t count = 0;
};

// 54bf..5542/55de: descending pending-task selection and linked-voice ordering.
// Preparation itself (5639 and following DSP setup) is the caller's next phase.
// A task4 completes the stop-stage transition at54f0..5517. Reject corrupt
// task/link values before mutation instead of indexing an H8 jump table.
inline std::optional<VoiceTaskDispatch> DispatchNextVoiceTask(
    std::array<VoiceStopState,24>& voices,const VoiceLinks& links,
    std::span<uint8_t,24> activity) noexcept
{
    for (unsigned n = 24; n > 0; --n)
    {
        const auto slot = uint8_t(n-1);
        const auto task = voices[slot].fieldCAF4;
        if (!task) continue;
        if (task != 2 && task != 4) return std::nullopt;
        VoiceTaskDispatch result{task == 2 ? VoiceTaskDispatch::Kind::prepare
                                          : VoiceTaskDispatch::Kind::finishStop,{slot,255},1};
        if (task == 2)
        {
            const auto first = links.first[slot], second = links.second[slot];
            if (first != 255)
            {
                if (first >= 24) return std::nullopt;
                result.slots = {first,slot}; result.count = 2;
            }
            else if (second != 255)
            {
                if (second >= 24) return std::nullopt;
                result.slots = {slot,second}; result.count = 2;
            }
        }
        for (unsigned i = 0; i < result.count; ++i) voices[result.slots[i]].fieldCAF4 = 0;
        if (task == 4)
        {
            activity[slot] = 0;
            voices[slot].stages.fill(voices[slot].stages[0] == 0x12 ? 0x0e : 0x10);
        }
        return result;
    }
    return VoiceTaskDispatch{};
}

// Compose the restart gate at125d/12e5 with53e6 and113a. Partial sample
// preparation and slot-input staging are separate. Device callbacks belong to
// the serialized synthesis thread and must not throw or mutate these owners.
// This ramps the old voice; it neither waits for silence nor keys the new one on.
template<class Read,class Write>
bool RestartAndInstallVoice(unsigned slot,VoiceInstallationInput input,uint8_t& flags,
    VoiceAllocator& allocator,VoiceInstallationState& installation,VoiceStopState& lifecycle,
    Read&& read,Write&& write)
{
    if (slot >= 24) return false;
    const bool restart = (flags&128) != 0;
    input.restarted = input.restarted || restart;
    auto checkedAllocator = allocator;
    auto checkedInstallation = installation;
    auto checkedFlags = flags;
    // Validate even the negative-sample return path before irreversible PCM I/O.
    if (!checkedInstallation.install(slot,input,checkedFlags,checkedAllocator)) return false;
    if (restart) (void)StopPreparedVoice(slot,lifecycle,read,write);
    allocator = checkedAllocator;
    installation = checkedInstallation;
    flags = checkedFlags;
    if (!(input.sample&0x8000)) lifecycle.fieldCAF4 = installation.voices[slot].taskState;
    return true;
}
}

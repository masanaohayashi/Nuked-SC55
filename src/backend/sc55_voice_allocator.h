#pragma once
#include "sc55_voice_links.h"
#include <algorithm>
#include <optional>
#include <span>

namespace sc55
{
// State touched by v1.21 1cbd..1d54, including its two detach subroutines.
// Free-list and group-link primitives; full note allocation/stealing are pending.
struct VoiceAllocator
{
    VoiceLinks pcmLinks;
    VoiceGroupLinks groups;
    std::array<uint8_t,24> voicePart{}, voiceGroup{}, status{}, fieldA360{}, fieldA3E0{}, freeNext{};
    std::array<uint8_t,24> groupNext{}, groupPrevious{}, groupStatus{}, groupValue{};
    std::array<uint8_t,24> groupFieldA288{}, groupFieldA2D0{}, groupFieldA300{};
    std::array<uint8_t,16> partHead{}, partTail{}, partMinimum{}, partVoiceCount{};
    std::array<uint8_t,16> partFlags{}, partPrevious{};
    std::array<uint8_t,24> activity{};
    uint8_t shortage = 0;
    uint8_t freeCount = 0, freeGroupHead = 0xff, freeHead = 0xff, freeTail = 0xff;

    // 1e04..1e1a publishes A3E0 into AC2A, descending physical-slot order.
    // It overwrites (including zero), does not OR requests or clear the source.
    // Invoke at the note-management publication boundary, NOT every control
    // tick: consumed requests would otherwise be published again. Store must
    // be bounded, nonthrowing and nonreentrant on the serialized voice owner.
    template<class Store>
    void publishReleaseRequests(Store&& store) const
    {
        for (unsigned slot = 24; slot-- > 0;)
            store(uint8_t(slot),fieldA3E0[slot]);
    }

    struct MonoReuse
    {
        uint8_t flags; // A1B1
        std::array<uint8_t,2> voices; // A3D6/A3D7, tail then distinct head
    };

    // 0b1a..0b64, existing-group path only (A1CE part bit already clear).
    // Preserve the caller's second slot if head==tail, matching the firmware.
    // Merely selects slots/flags; does not mutate or prepare PCM voices.
    std::optional<MonoReuse> prepareMonoReuse(unsigned part,MonoReuse input) const noexcept
    {
        if (part >= 16 || partHead[part] >= 24) return std::nullopt;
        const auto group = partHead[part], tail = groups.tail[group], head = groups.head[group];
        if (tail >= 24) return std::nullopt;
        input.flags = uint8_t((input.flags&0x1f)|0x40);
        input.voices[0] = tail;
        if (status[tail] == 0x94) input.flags |= 0x80;
        if (head != tail)
        {
            if (head < 128 && head >= 24) return std::nullopt;
            input.voices[1] = head;
            if (head < 128 && status[head] == 0x94) input.flags |= 0x80;
        }
        return input;
    }

    // 0aab..0aec: release the part's first mono group, or defer for hold.
    // Unlike normal note-off, this uses explicit head/tail, not tail/previous,
    // and does not inspect retained keys or the old group status. A missing
    // tail skips status/hold handling but can still release a distinct head.
    bool releaseMonoGroup(unsigned part) noexcept
    {
        if (part >= 16 || partHead[part] >= 24) return false;
        const auto group = partHead[part], tail = groups.tail[group], head = groups.head[group];
        auto updated = *this;
        if (tail < 128)
        {
            if (tail >= 24) return false;
            updated.groupStatus[group] = 2;
            if (partFlags[part]&1)
            {
                updated.groupFieldA288[group] |= 1;
                *this = updated;
                return true;
            }
            updated.fieldA360[tail] = 1; updated.fieldA3E0[tail] = 255;
        }
        if (head != tail)
        {
            if (head >= 24) return false;
            updated.fieldA360[head] = 1; updated.fieldA3E0[head] = 255;
        }
        *this = updated;
        return true;
    }

    // 155a..1597 + 1633..1687. Select only the first eligible group.
    // Selector0 bypasses A2D0 matching. Retained keys are the caller-owned
    // A090 part row; the first high-bit byte terminates the scan. Their MIDI
    // producer is separate. true includes deferred/retained releases; false
    // means no match, nullopt corrupt routing/links (no partial mutation).
    std::optional<bool> requestNoteRelease(unsigned part,uint8_t note,uint8_t selector,
        std::span<const uint8_t,16> retainedKeys) noexcept
    {
        if (part >= 16) return std::nullopt;
        unsigned visited = 0;
        for (auto group = partHead[part]; group < 128; group = groupNext[group])
        {
            if (group >= 24 || ++visited > 24) return std::nullopt;
            if (groupStatus[group] != 0 || groupValue[group] != note
                || (selector != 0 && groupFieldA2D0[group] != selector)
                || (groupFieldA300[group]&1) == 0) continue;
            auto updated = *this;
            updated.groupStatus[group] = 2;
            if (partFlags[part]&1) updated.groupFieldA288[group] |= 1;
            else
            {
                bool retained = false;
                for (const auto key : retainedKeys)
                {
                    if (key&128) break;
                    if (key == note) { retained = true; break; }
                }
                if (!retained)
                {
                    const auto tail = groups.tail[group];
                    if (tail >= 24) return std::nullopt;
                    const auto previous = groups.previous[tail];
                    if (previous < 128 && previous >= 24) return std::nullopt;
                    updated.fieldA360[tail] = 1; updated.fieldA3E0[tail] = 255;
                    if (previous < 128)
                    { updated.fieldA360[previous] = 1; updated.fieldA3E0[previous] = 255; }
                }
            }
            *this = updated;
            return true;
        }
        return false;
    }

    // 16cd..16d1 / 16d2..1736. Hold-off visits every deferred group, even
    // though its status already changed to2 at note-off. Retained keys can
    // still suppress individual requests. An already-clear part is a no-op.
    // Invalid links roll back the complete operation, including flag clears.
    bool setPartHold(unsigned part,bool enabled,std::span<const uint8_t,16> retainedKeys) noexcept
    {
        if (part >= 16) return false;
        if (enabled) { partFlags[part] |= 1; return true; }
        if ((partFlags[part]&1) == 0) return true;
        auto updated = *this;
        updated.partFlags[part] &= 0xfe;
        unsigned visited = 0;
        for (auto group = partHead[part]; group < 128; group = groupNext[group])
        {
            if (group >= 24 || ++visited > 24) return false;
            const bool deferred = (groupFieldA288[group]&1) != 0;
            updated.groupFieldA288[group] &= 0xfe;
            if (!deferred) continue;
            bool retained = false;
            for (const auto key : retainedKeys)
            {
                if (key&128) break;
                if (key == groupValue[group]) { retained = true; break; }
            }
            if (retained) continue;
            const auto tail = groups.tail[group];
            if (tail >= 24) return false;
            const auto previous = groups.previous[tail];
            if (previous < 128 && previous >= 24) return false;
            updated.fieldA360[tail] = 1; updated.fieldA3E0[tail] = 255;
            if (previous < 128)
            { updated.fieldA360[previous] = 1; updated.fieldA3E0[previous] = 255; }
        }
        *this = updated;
        return true;
    }

    // 04:05f1..063e. Append unreleased group keys to the caller-owned A090
    // row, preserving existing entries. Duplicate keys do not consume slots.
    // A high-bit byte is the insertion sentinel; filling slot15 ends the
    // whole traversal. No allocation or ROM access on the voice-owner thread.
    bool captureRetainedKeys(unsigned part,std::span<uint8_t,16> retainedKeys) const noexcept
    {
        if (part >= 16) return false;
        std::array<uint8_t,16> updated;
        std::copy(retainedKeys.begin(),retainedKeys.end(),updated.begin());
        unsigned visited = 0;
        for (auto group = partHead[part]; group < 128; group = groupNext[group])
        {
            if (group >= 24 || ++visited > 24) return false;
            if (groupStatus[group] != 0) continue;
            unsigned index = 0;
            for (; index < 16; ++index)
            {
                if (updated[index]&128) { updated[index] = groupValue[group]; break; }
                if (updated[index] == groupValue[group]) break;
            }
            if (index == 15) break;
        }
        std::copy(updated.begin(),updated.end(),retainedKeys.begin());
        return true;
    }

    // 04:0640..06b5. Release retained status2 groups unless hold deferred
    // them, then clear the row. A full, nonmatching row ends ALL traversal
    // (0679), unlike a sentinel which skips only the current group.
    // Invalid links roll back both requests and the caller-owned row.
    bool releaseRetainedKeys(unsigned part,std::span<uint8_t,16> retainedKeys) noexcept
    {
        if (part >= 16) return false;
        auto updated = *this;
        unsigned visited = 0;
        for (auto group = partHead[part]; group < 128; group = groupNext[group])
        {
            if (group >= 24 || ++visited > 24) return false;
            if (groupStatus[group] != 2) continue;
            unsigned index = 0;
            bool matched = false;
            for (; index < 16; ++index)
            {
                if (retainedKeys[index]&128) break;
                if (retainedKeys[index] == groupValue[group]) { matched = true; break; }
            }
            if (index == 16) break;
            if (!matched || (groupFieldA288[group]&1)) continue;
            const auto tail = groups.tail[group];
            if (tail >= 24) return false;
            const auto previous = groups.previous[tail];
            if (previous < 128 && previous >= 24) return false;
            updated.fieldA360[tail] = 1; updated.fieldA3E0[tail] = 255;
            if (previous < 128)
            { updated.fieldA360[previous] = 1; updated.fieldA3E0[previous] = 255; }
        }
        *this = updated;
        std::fill(retainedKeys.begin(),retainedKeys.end(),255);
        return true;
    }

    // Allocator tables initialized by 04:04b9..0569. Other subsystem fields
    // cleared by that ROM region are outside this object. Preserve fields the
    // firmware does not touch, including PCM links initialized earlier.
    bool initializeTables(unsigned voices = 24, unsigned noteGroups = 24) noexcept
    {
        if (voices < 1 || voices > 24 || noteGroups < 1 || noteGroups > 24) return false;
        groups.next.fill(255); groups.previous.fill(255);
        fieldA360.fill(0); freeNext.fill(255);
        freeHead = uint8_t(voices-1); freeTail = 0; freeCount = uint8_t(voices);
        for (unsigned i = 0; i < voices; ++i)
        { freeNext[i] = i == 0 ? 255 : uint8_t(i-1); status[i] = 0x94; }
        freeGroupHead = 0;
        for (unsigned i = 0; i < noteGroups; ++i)
        {
            groupNext[i] = i+1 == noteGroups ? 255 : uint8_t(i+1);
            groupStatus[i] = 0x94; groupPrevious[i] = groupValue[i] = 255;
            groups.head[i] = groups.tail[i] = 255;
        }
        partVoiceCount.fill(0); partHead.fill(255); partTail.fill(255); partMinimum.fill(127);
        for (unsigned i = 0; i < 16; ++i)
        { partFlags[i] &= 0xfe; partPrevious[i] = i == 0 ? 255 : uint8_t(i-1); }
        return true;
    }

    struct GroupRequest { uint8_t part, fieldA3D1, value, fieldA1BF, voiceCount; };
    struct GroupAllocation { uint8_t group; std::array<uint8_t,3> voices{255,255,255}; };

    enum class CandidatePass { nonzeroStatusOtherValue, otherValue, sameValue };
    struct Candidate { uint8_t voice = 255, activity = 255; };
    // 1961/19af/19f5: scan the part's groups, then tail and predecessor.
    // Strictly smaller activity wins, so ties retain the first encountered slot.
    // nullopt denotes corrupt input; Candidate{255,255} denotes no candidate.
    std::optional<Candidate> selectCandidate(unsigned part, uint8_t value,
        CandidatePass pass, const std::array<uint8_t,24>& activity) const noexcept
    {
        if (part >= 16) return std::nullopt;
        Candidate best;
        unsigned visited = 0;
        for (auto group = partHead[part]; group < 128; group = groupNext[group])
        {
            if (group >= 24 || ++visited > 24) return std::nullopt;
            if (pass == CandidatePass::nonzeroStatusOtherValue && groupStatus[group] == 0) continue;
            const bool same = groupValue[group] == value;
            if (same != (pass == CandidatePass::sameValue)) continue;
            const auto tail = groups.tail[group];
            if (tail >= 24) return std::nullopt;
            if (activity[tail] < best.activity) best = {tail,activity[tail]};
            const auto previous = groups.previous[tail];
            if (previous < 128)
            {
                if (previous >= 24) return std::nullopt;
                if (activity[previous] < best.activity) best = {previous,activity[previous]};
            }
        }
        return best;
    }

    // 1bab..1c3f, after caller has made room. Does not start the PCM voices.
    // The result slots follow firmware order (first acquired goes in last slot).
    std::optional<GroupAllocation> createGroup(GroupRequest request) noexcept
    {
        if (request.part >= 16 || request.voiceCount < 1 || request.voiceCount > 2 || freeGroupHead >= 24)
            return std::nullopt;
        auto updated = *this;
        const auto group = updated.freeGroupHead, part = request.part;
        const auto tail = updated.partTail[part];
        if (tail < 128 && tail >= 24) return std::nullopt;
        updated.freeGroupHead = updated.groupNext[group];
        if (tail >= 128) updated.partHead[part] = group;
        else updated.groupNext[tail] = group;
        updated.partTail[part] = group;
        updated.groupPrevious[group] = tail;
        updated.groupNext[group] = 255;
        updated.groupFieldA2D0[group] = request.fieldA3D1;
        updated.groupValue[group] = request.value;
        updated.groupFieldA300[group] = request.fieldA1BF;
        updated.groupStatus[group] = updated.groupFieldA288[group] = 0;
        // Firmware branches on N from byte subtraction, not signed-less-than.
        if (updated.partMinimum[part] < 128
            && (uint8_t(request.value-updated.partMinimum[part]) & 128))
            updated.partMinimum[part] = request.value;
        GroupAllocation result{group};
        for (unsigned remaining = request.voiceCount; remaining > 0; --remaining)
        {
            const auto voice = updated.takeFreeVoice();
            if (!voice || !updated.attachUnchecked(*voice,group,part)) return std::nullopt;
            if (remaining != request.voiceCount && result.voices[remaining] == *voice) return std::nullopt;
            result.voices[remaining-1] = *voice;
            ++updated.partVoiceCount[part];
        }
        *this = updated;
        return result;
    }

    // 1ca5..1cbc. Caller normally ensures capacity before invoking firmware.
    // Does not mark active or clear stale links: those are separate operations.
    std::optional<uint8_t> takeFreeVoice() noexcept
    {
        if (freeHead >= 24) return std::nullopt;
        const auto voice = freeHead, next = freeNext[voice];
        if (next < 128 && next >= 24) return std::nullopt;
        freeHead = next;
        if (next >= 128) freeTail = next;
        --freeCount;
        return voice;
    }

    // 1c40..1ca4. Firmware retains at most the previous tail and new voice
    // as group endpoints. This is not generic linked-list append.
    bool attachVoice(unsigned voice, unsigned group, unsigned part) noexcept
    {
        if (voice >= 24 || group >= 24 || part >= 16) return false;
        auto updated = *this;
        if (!updated.attachUnchecked(voice,group,part)) return false;
        *this = updated;
        return true;
    }

    // Caller owns this state on the synthesis thread. Bounded, no allocation.
    // Invalid references/cycles fail without partially updating any table.
    bool returnVoice(unsigned voice) noexcept
    {
        if (voice >= 24) return false;
        if (status[voice] & 0x80) return true; // already free
        auto updated = *this;
        if (!updated.returnActiveVoice(voice,voiceGroup[voice],voicePart[voice],false)) return false;
        *this = updated;
        return true;
    }

    // Bookkeeping only: 1a7d..1b0f or 1b24..1baa, AFTER the caller has
    // stopped the PCM voice via 53e6 and set CAF4=4. Unlike returnVoice,
    // this does not skip free status and takes caller-supplied group/part.
    bool reclaimStoppedVoice(unsigned voice, unsigned group, unsigned part, bool prepend) noexcept
    {
        if (voice >= 24) return false;
        auto updated = *this;
        if (!updated.returnActiveVoice(voice,group,part,prepend)) return false;
        updated.activity[voice] = 0;
        --updated.shortage;
        *this = updated;
        return true;
    }

private:
    bool attachUnchecked(unsigned voice, unsigned group, unsigned part) noexcept
    {
        const auto oldTail = groups.tail[group];
        if (oldTail >= 128)
        {
            groups.head[group] = uint8_t(voice);
            auto partner = pcmLinks.first[voice];
            if (partner < 128)
            {
                if (partner >= 24) return false;
                pcmLinks.second[partner] = 0xff;
            }
            // Read after the previous write, preserving alias/self-link order.
            partner = pcmLinks.second[voice];
            if (partner < 128)
            {
                if (partner >= 24) return false;
                pcmLinks.first[partner] = 0xff;
            }
            groups.previous[voice] = pcmLinks.first[voice] = 0xff;
        }
        else
        {
            if (oldTail >= 24) return false;
            groups.head[group] = oldTail;
            groups.next[oldTail] = pcmLinks.second[oldTail] = uint8_t(voice);
            groups.previous[voice] = pcmLinks.first[voice] = oldTail;
            groups.previous[oldTail] = pcmLinks.first[oldTail] = 0xff;
        }
        groups.tail[group] = uint8_t(voice);
        groups.next[voice] = pcmLinks.second[voice] = 0xff;
        voicePart[voice] = uint8_t(part); voiceGroup[voice] = uint8_t(group);
        return true;
    }

    bool returnActiveVoice(unsigned voice, unsigned group, unsigned part, bool prepend) noexcept
    {
        if (group >= 24 || part >= 16 || (!prepend && freeTail < 128 && freeTail >= 24)) return false;
        if (!groups.detach(voice,group,pcmLinks)) return false;
        fieldA360[voice] = fieldA3E0[voice] = 0;
        if (prepend)
        {
            if (freeHead < 128 && freeHead >= 24) return false;
            if (freeHead >= 128) freeTail = uint8_t(voice);
            freeNext[voice] = freeHead;
            freeHead = uint8_t(voice);
        }
        else if (freeTail >= 128)
        {
            freeHead = uint8_t(voice);
            freeNext[voice] = freeTail;
        }
        else
        {
            freeNext[freeTail] = uint8_t(voice);
            freeNext[voice] = 0xff;
        }
        if (!prepend) freeTail = uint8_t(voice);
        status[voice] = 0x94;
        ++freeCount;
        if (groups.head[group] >= 128)
        {
            // 1e59 removes the empty group from the part's group chain.
            const auto next = groupNext[group], previous = groupPrevious[group];
            if ((next < 128 && next >= 24) || (previous < 128 && previous >= 24)) return false;
            if (previous >= 128) partHead[part] = next;
            else groupNext[previous] = next;
            if (next >= 128) partTail[part] = previous;
            else groupPrevious[next] = previous;
            groupNext[group] = freeGroupHead;
            freeGroupHead = group;
            groupStatus[group] = 0x94;
            if (partMinimum[part] < 128 && partMinimum[part] == groupValue[group])
            {
                uint8_t minimum = 127;
                unsigned visited = 0;
                for (auto item = partHead[part]; item < 128; item = groupNext[item])
                {
                    if (item >= 24 || ++visited > 24) return false;
                    minimum = std::min(minimum,groupValue[item]);
                }
                partMinimum[part] = minimum;
            }
        }
        --partVoiceCount[part];
        return true;
    }
};
}

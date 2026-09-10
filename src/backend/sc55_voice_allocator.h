#pragma once
#include "sc55_voice_set.h"
#include "sc55_voice_links.h"
#include <algorithm>
#include <optional>
#include <span>

namespace sc55
{
// Owns note groups and physical voice allocation. A note group survives release
// until its last voice is returned; capacity and pedal decisions share this owner.
template<unsigned Capacity>
struct BasicVoiceAllocator
{
    static_assert(Capacity > 0 && Capacity <= 128);
    struct NoteGroup
    {
        uint8_t next=0, previous=0, status=0, key=0;
        uint8_t retirementFlags=0; // bit0: hold-deferred; bit2: repeated-note mark
        uint8_t noteClass=0; // melodic80/high81, otherwise rhythm exclusion class
        uint8_t releaseFlags=0; // bit0 permits Note Off; preserve remaining firmware flags

        bool acceptsNoteOff(uint8_t selector) const noexcept
        { return status==0 && (releaseFlags&1) && (selector==0 || noteClass==selector); }
        bool held() const noexcept { return (retirementFlags&1)!=0; }
        void deferRelease() noexcept { retirementFlags|=1; }
        bool releaseHold() noexcept
        {
            const bool wasHeld=held(); retirementFlags&=0xfe; return wasHeld;
        }
        // Returns the previous mark: a second encounter retires the group.
        bool markRepeated() noexcept
        {
            const bool wasMarked=(retirementFlags&4)!=0; retirementFlags|=4; return wasMarked;
        }
    };
    struct VoiceAllocation
    {
        uint8_t part=0, noteGroup=0, status=0;
        uint8_t releaseRequested=0, releaseCommand=0, nextFree=0;
        void requestRelease() noexcept { releaseRequested=1; releaseCommand=255; }
        void clearRelease() noexcept { releaseRequested=releaseCommand=0; }
        bool free() const noexcept { return (status&128)!=0; }
    };
    BasicVoiceLinks<Capacity> pcmLinks;
    BasicVoiceGroupLinks<Capacity> groups;
    std::array<VoiceAllocation,Capacity> allocations{};
    std::array<NoteGroup,Capacity> noteGroups{};
    std::array<uint8_t,16> partHead{}, partTail{}, partMinimum{}, partVoiceCount{};
    std::array<uint8_t,16> partFlags{}, partPrevious{};
    std::array<uint8_t,Capacity> activity{};
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
        for (unsigned slot = Capacity; slot-- > 0;)
            store(uint8_t(slot),allocations[slot].releaseCommand);
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
        if (part >= 16 || partHead[part] >= Capacity) return std::nullopt;
        return prepareGroupReuse(partHead[part],input);
    }

    // CC84 searches newest-to-oldest and accepts only melodic groups. A
    // released-but-linked group is eligible too; preparation resets its status.
    std::optional<uint8_t> findSourceGroup(unsigned part,uint8_t source) const noexcept
    {
        if(part>=16 || source>=128) return std::nullopt;
        unsigned visited=0;
        for(auto group=partTail[part];group<128;group=noteGroups[group].previous) {
            if(group>=Capacity || ++visited>Capacity) return std::nullopt;
            if(noteGroups[group].key==source && noteGroups[group].noteClass==0x80) return group;
        }
        return uint8_t(255);
    }

    std::optional<MonoReuse> prepareGroupReuse(unsigned group,MonoReuse input) const noexcept
    {
        if(group>=Capacity) return std::nullopt;
        const auto tail = groups.tail[group], head = groups.head[group];
        if (tail >= Capacity) return std::nullopt;
        input.flags = uint8_t((input.flags&0x1f)|0x40);
        input.voices[0] = tail;
        if (allocations[tail].status == 0x94) input.flags |= 0x80;
        if (head != tail)
        {
            if (head < 128 && head >= Capacity) return std::nullopt;
            input.voices[1] = head;
            if (head < 128 && allocations[head].status == 0x94) input.flags |= 0x80;
        }
        return input;
    }

    // 0aab..0aec: release the part's first mono group, or defer for hold.
    // Unlike normal note-off, this uses explicit head/tail, not tail/previous,
    // and does not inspect retained keys or the old group status. A missing
    // tail skips status/hold handling but can still release a distinct head.
    bool releaseMonoGroup(unsigned part) noexcept
    {
        if (part >= 16 || partHead[part] >= Capacity) return false;
        const auto group = partHead[part], tail = groups.tail[group], head = groups.head[group];
        auto updated = *this;
        if (tail < 128)
        {
            if (tail >= Capacity) return false;
            updated.noteGroups[group].status = 2;
            if (partFlags[part]&1)
            {
                updated.noteGroups[group].deferRelease();
                *this = updated;
                return true;
            }
            updated.allocations[tail].requestRelease();
        }
        if (head != tail)
        {
            if (head >= Capacity) return false;
            updated.allocations[head].requestRelease();
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
        for (auto group = partHead[part]; group < 128; group = noteGroups[group].next)
        {
            if (group >= Capacity || ++visited > Capacity) return std::nullopt;
            if (noteGroups[group].key != note || !noteGroups[group].acceptsNoteOff(selector)) continue;
            auto updated = *this;
            updated.noteGroups[group].status = 2;
            if (partFlags[part]&1) updated.noteGroups[group].deferRelease();
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
                    if (tail >= Capacity) return std::nullopt;
                    const auto previous = groups.previous[tail];
                    if (previous < 128 && previous >= Capacity) return std::nullopt;
                    updated.allocations[tail].requestRelease();
                    if (previous < 128)
                    { updated.allocations[previous].requestRelease(); }
                }
            }
            *this = updated;
            return true;
        }
        return false;
    }

    // 15af..1632. CC123 uses selector80/mask0. Rhythm also tests A300bit0;
    // melodic mode does not. Release tails and pedal retention remain alive.
    bool requestGroupReleases(unsigned part,bool rhythm,uint8_t selector,uint8_t mask,
        std::span<const uint8_t,16> retainedKeys) noexcept
    {
        if (part >= 16) return false;
        auto updated = *this;
        unsigned visited = 0;
        for (auto group = partHead[part]; group < 128; group = noteGroups[group].next)
        {
            if (group >= Capacity || ++visited > Capacity) return false;
            if (noteGroups[group].status != 0 || (rhythm ? !(noteGroups[group].releaseFlags&1)
                : (noteGroups[group].noteClass&mask) != (selector&mask))) continue;
            updated.noteGroups[group].status = 2;
            if (partFlags[part]&1) { updated.noteGroups[group].deferRelease(); continue; }
            bool retained = false;
            for (auto key : retainedKeys) {
                if (key&128) break;
                if (key == noteGroups[group].key) { retained = true; break; }
            }
            if (retained) continue;
            const auto tail = groups.tail[group];
            if (tail >= Capacity) return false;
            const auto previous = groups.previous[tail];
            if (previous < 128 && previous >= Capacity) return false;
            updated.allocations[tail].requestRelease();
            if (previous < 128) { updated.allocations[previous].requestRelease(); }
        }
        *this = updated;
        return true;
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
        for (auto group = partHead[part]; group < 128; group = noteGroups[group].next)
        {
            if (group >= Capacity || ++visited > Capacity) return false;
            const bool deferred = updated.noteGroups[group].releaseHold();
            if (!deferred) continue;
            bool retained = false;
            for (const auto key : retainedKeys)
            {
                if (key&128) break;
                if (key == noteGroups[group].key) { retained = true; break; }
            }
            if (retained) continue;
            const auto tail = groups.tail[group];
            if (tail >= Capacity) return false;
            const auto previous = groups.previous[tail];
            if (previous < 128 && previous >= Capacity) return false;
            updated.allocations[tail].requestRelease();
            if (previous < 128)
            { updated.allocations[previous].requestRelease(); }
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
        for (auto group = partHead[part]; group < 128; group = noteGroups[group].next)
        {
            if (group >= Capacity || ++visited > Capacity) return false;
            if (noteGroups[group].status != 0) continue;
            unsigned index = 0;
            for (; index < 16; ++index)
            {
                if (updated[index]&128) { updated[index] = noteGroups[group].key; break; }
                if (updated[index] == noteGroups[group].key) break;
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
        for (auto group = partHead[part]; group < 128; group = noteGroups[group].next)
        {
            if (group >= Capacity || ++visited > Capacity) return false;
            if (noteGroups[group].status != 2) continue;
            unsigned index = 0;
            bool matched = false;
            for (; index < 16; ++index)
            {
                if (retainedKeys[index]&128) break;
                if (retainedKeys[index] == noteGroups[group].key) { matched = true; break; }
            }
            if (index == 16) break;
            if (!matched || noteGroups[group].held()) continue;
            const auto tail = groups.tail[group];
            if (tail >= Capacity) return false;
            const auto previous = groups.previous[tail];
            if (previous < 128 && previous >= Capacity) return false;
            updated.allocations[tail].requestRelease();
            if (previous < 128)
            { updated.allocations[previous].requestRelease(); }
        }
        *this = updated;
        std::fill(retainedKeys.begin(),retainedKeys.end(),255);
        return true;
    }

    // Allocator tables initialized by 04:04b9..0569. Other subsystem fields
    // cleared by that ROM region are outside this object. Preserve fields the
    // firmware does not touch, including PCM links initialized earlier.
    unsigned voiceLimit = 24;
    bool initializeTables(unsigned voices = 24, unsigned groupCount = 24) noexcept
    {
        if (voices < 1 || voices > Capacity || groupCount < 1 || groupCount > Capacity) return false;
        voiceLimit = voices;
        for (unsigned i=voices;i<Capacity;++i) allocations[i].status=0x94;
        groups.next.fill(255); groups.previous.fill(255);
        for(auto& voice:allocations) { voice.releaseRequested=0; voice.nextFree=255; }
        freeHead = uint8_t(voices-1); freeTail = 0; freeCount = uint8_t(voices);
        for (unsigned i = 0; i < voices; ++i)
        { allocations[i].nextFree = i == 0 ? 255 : uint8_t(i-1); allocations[i].status = 0x94; }
        freeGroupHead = 0;
        for (unsigned i = 0; i < groupCount; ++i)
        {
            noteGroups[i].next = i+1 == groupCount ? 255 : uint8_t(i+1);
            noteGroups[i].status = 0x94; noteGroups[i].previous = noteGroups[i].key = 255;
            groups.head[i] = groups.tail[i] = 255;
        }
        partVoiceCount.fill(0); partHead.fill(255); partTail.fill(255); partMinimum.fill(127);
        for (unsigned i = 0; i < 16; ++i)
        { partFlags[i] &= 0xfe; partPrevious[i] = i == 0 ? 255 : uint8_t(i-1); }
        return true;
    }

    struct GroupRequest { uint8_t part, noteClass, value, releaseFlags, voiceCount; };
    struct GroupAllocation { uint8_t group; std::array<uint8_t,3> voices{255,255,255}; };

    enum class CandidatePass { nonzeroStatusOtherValue, otherValue, sameValue };
    struct Candidate { uint8_t voice = 255, activity = 255; };
    // 1961/19af/19f5: scan the part's groups, then tail and predecessor.
    // Strictly smaller activity wins, so ties retain the first encountered slot.
    // nullopt denotes corrupt input; Candidate{255,255} denotes no candidate.
    std::optional<Candidate> selectCandidate(unsigned part, uint8_t value,
        CandidatePass pass, const std::array<uint8_t,Capacity>& activity) const noexcept
    {
        if (part >= 16) return std::nullopt;
        Candidate best;
        unsigned visited = 0;
        for (auto group = partHead[part]; group < 128; group = noteGroups[group].next)
        {
            if (group >= Capacity || ++visited > Capacity) return std::nullopt;
            if (pass == CandidatePass::nonzeroStatusOtherValue && noteGroups[group].status == 0) continue;
            const bool same = noteGroups[group].key == value;
            if (same != (pass == CandidatePass::sameValue)) continue;
            const auto tail = groups.tail[group];
            if (tail >= Capacity) return std::nullopt;
            if (activity[tail] < best.activity) best = {tail,activity[tail]};
            const auto previous = groups.previous[tail];
            if (previous < 128)
            {
                if (previous >= Capacity) return std::nullopt;
                if (activity[previous] < best.activity) best = {previous,activity[previous]};
            }
        }
        return best;
    }

    // 1bab..1c3f, after caller has made room. Does not start the PCM voices.
    // The result slots follow firmware order (first acquired goes in last slot).
    std::optional<GroupAllocation> createGroup(GroupRequest request) noexcept
    {
        if (request.part >= 16 || request.voiceCount < 1 || request.voiceCount > 2 || freeGroupHead >= Capacity)
            return std::nullopt;
        auto updated = *this;
        const auto group = updated.freeGroupHead, part = request.part;
        const auto tail = updated.partTail[part];
        if (tail < 128 && tail >= Capacity) return std::nullopt;
        updated.freeGroupHead = updated.noteGroups[group].next;
        if (tail >= 128) updated.partHead[part] = group;
        else updated.noteGroups[tail].next = group;
        updated.partTail[part] = group;
        updated.noteGroups[group].previous = tail;
        updated.noteGroups[group].next = 255;
        updated.noteGroups[group].noteClass = request.noteClass;
        updated.noteGroups[group].key = request.value;
        updated.noteGroups[group].releaseFlags = request.releaseFlags;
        updated.noteGroups[group].status = updated.noteGroups[group].retirementFlags = 0;
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
        if (freeHead >= Capacity) return std::nullopt;
        const auto voice = freeHead, next = allocations[voice].nextFree;
        if (next < 128 && next >= Capacity) return std::nullopt;
        freeHead = next;
        if (next >= 128) freeTail = next;
        --freeCount;
        return voice;
    }

    // 1c40..1ca4. Firmware retains at most the previous tail and new voice
    // as group endpoints. This is not generic linked-list append.
    bool attachVoice(unsigned voice, unsigned group, unsigned part) noexcept
    {
        if (voice >= Capacity || group >= Capacity || part >= 16) return false;
        auto updated = *this;
        if (!updated.attachUnchecked(voice,group,part)) return false;
        *this = updated;
        return true;
    }

    // Caller owns this state on the synthesis thread. Bounded, no allocation.
    // Invalid references/cycles fail without partially updating any table.
    bool returnVoice(unsigned voice) noexcept
    {
        if (voice >= Capacity) return false;
        if (allocations[voice].free()) return true; // already free
        auto updated = *this;
        if (!updated.returnActiveVoice(voice,allocations[voice].noteGroup,allocations[voice].part,false)) return false;
        *this = updated;
        return true;
    }

    // Bookkeeping only: 1a7d..1b0f or 1b24..1baa, AFTER the caller has
    // stopped the PCM voice via 53e6 and set CAF4=4. Unlike returnVoice,
    // this does not skip free status and takes caller-supplied group/part.
    bool reclaimStoppedVoice(unsigned voice, unsigned group, unsigned part, bool prepend) noexcept
    {
        if (voice >= Capacity) return false;
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
                if (partner >= Capacity) return false;
                pcmLinks.second[partner] = 0xff;
            }
            // Read after the previous write, preserving alias/self-link order.
            partner = pcmLinks.second[voice];
            if (partner < 128)
            {
                if (partner >= Capacity) return false;
                pcmLinks.first[partner] = 0xff;
            }
            groups.previous[voice] = pcmLinks.first[voice] = 0xff;
        }
        else
        {
            if (oldTail >= Capacity) return false;
            groups.head[group] = oldTail;
            groups.next[oldTail] = pcmLinks.second[oldTail] = uint8_t(voice);
            groups.previous[voice] = pcmLinks.first[voice] = oldTail;
            groups.previous[oldTail] = pcmLinks.first[oldTail] = 0xff;
        }
        groups.tail[group] = uint8_t(voice);
        groups.next[voice] = pcmLinks.second[voice] = 0xff;
        allocations[voice].part = uint8_t(part); allocations[voice].noteGroup = uint8_t(group);
        return true;
    }

    bool returnActiveVoice(unsigned voice, unsigned group, unsigned part, bool prepend) noexcept
    {
        if (group >= Capacity || part >= 16 || (!prepend && freeTail < 128 && freeTail >= Capacity)) return false;
        if (!groups.detach(voice,group,pcmLinks)) return false;
        allocations[voice].clearRelease();
        if (prepend)
        {
            if (freeHead < 128 && freeHead >= Capacity) return false;
            if (freeHead >= 128) freeTail = uint8_t(voice);
            allocations[voice].nextFree = freeHead;
            freeHead = uint8_t(voice);
        }
        else if (freeTail >= 128)
        {
            freeHead = uint8_t(voice);
            allocations[voice].nextFree = freeTail;
        }
        else
        {
            allocations[freeTail].nextFree = uint8_t(voice);
            allocations[voice].nextFree = 0xff;
        }
        if (!prepend) freeTail = uint8_t(voice);
        allocations[voice].status = 0x94;
        ++freeCount;
        if (groups.head[group] >= 128)
        {
            // 1e59 removes the empty group from the part's group chain.
            const auto next = noteGroups[group].next, previous = noteGroups[group].previous;
            if ((next < 128 && next >= Capacity) || (previous < 128 && previous >= Capacity)) return false;
            if (previous >= 128) partHead[part] = next;
            else noteGroups[previous].next = next;
            if (next >= 128) partTail[part] = previous;
            else noteGroups[next].previous = previous;
            noteGroups[group].next = freeGroupHead;
            freeGroupHead = group;
            noteGroups[group].status = 0x94;
            if (partMinimum[part] < 128 && partMinimum[part] == noteGroups[group].key)
            {
                uint8_t minimum = 127;
                unsigned visited = 0;
                for (auto item = partHead[part]; item < 128; item = noteGroups[item].next)
                {
                    if (item >= Capacity || ++visited > Capacity) return false;
                    minimum = std::min(minimum,noteGroups[item].key);
                }
                partMinimum[part] = minimum;
            }
        }
        --partVoiceCount[part];
        return true;
    }
};
using VoiceAllocator = BasicVoiceAllocator<voiceCapacity>;
}

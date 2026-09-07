#pragma once
#include "sc55_voice_setup.h"
#include <algorithm>
#include <array>
#include <vector>

namespace sc55
{
// Owned sound data only. No CPU, ROM address, file path or firmware lifetime.
// load() is setup-thread only; queries are allocation-free. Returned pointers
// remain valid until the bank is loaded again or destroyed.
class SampleBank
{
public:
    struct Group { uint16_t id; std::array<uint8_t,60> data; };
    struct Sample { uint16_t id; std::array<uint8_t,16> data; };

    // Data-only format v1: eight-byte magic, big-endian 16-bit counts,
    // followed by (16-bit ID, raw record) groups, then samples. No padding.
    std::vector<uint8_t> encode() const
    {
        if (groups_.empty()) return {};
        std::vector<uint8_t> bytes{'S','C','5','5','S','B','0','1'};
        const auto word = [&](size_t value) {
            bytes.push_back(uint8_t(value >> 8)); bytes.push_back(uint8_t(value));
        };
        word(groups_.size()); word(samples_.size());
        const auto records = [&](const auto& values) {
            for (const auto& value : values)
            {
                word(value.id);
                bytes.insert(bytes.end(),value.data.begin(),value.data.end());
            }
        };
        records(groups_); records(samples_);
        return bytes;
    }

    bool loadEncoded(std::span<const uint8_t> bytes)
    {
        constexpr std::array<uint8_t,8> magic{'S','C','5','5','S','B','0','1'};
        if (bytes.size() < 12 || !std::equal(magic.begin(),magic.end(),bytes.begin())) return false;
        const auto word = [&](size_t at) { return size_t((bytes[at] << 8) | bytes[at+1]); };
        const size_t groupCount = word(8), sampleCount = word(10);
        // Bound both allocation and arithmetic before reading record contents.
        if (groupCount == 0 || sampleCount > 32768
            || bytes.size() != 12 + groupCount*62 + sampleCount*18) return false;
        std::vector<Group> groups(groupCount);
        std::vector<Sample> samples(sampleCount);
        size_t at = 12;
        const auto records = [&](auto& values) {
            for (auto& value : values)
            {
                value.id = uint16_t(word(at)); at += 2;
                std::copy_n(bytes.begin()+at,value.data.size(),value.data.begin());
                at += value.data.size();
            }
        };
        records(groups); records(samples);
        return load(std::move(groups),std::move(samples));
    }

    bool load(std::vector<Group> groups, std::vector<Sample> samples)
    {
        const auto sortUnique = [](auto& records) {
            std::sort(records.begin(),records.end(),[](const auto& a,const auto& b){return a.id < b.id;});
            return std::adjacent_find(records.begin(),records.end(),
                [](const auto& a,const auto& b){return a.id == b.id;}) == records.end();
        };
        if (groups.empty() || !sortUnique(groups) || !sortUnique(samples)) return false;
        for (const auto& sample : samples) if (sample.id & 0x8000) return false;
        for (const auto& group : groups)
        {
            if (group.id == 0xffff) return false;
            for (unsigned key = 0; key < 128; ++key)
            {
                const auto id = SelectSampleZone(group.data,uint8_t(key));
                if (!id || (!(*id & 0x8000) && !find(samples,*id))) return false;
            }
        }
        // Commit only after validation; failure preserves the previous bank.
        groups_ = std::move(groups);
        samples_ = std::move(samples);
        return true;
    }

    std::optional<uint16_t> select(uint16_t groupId,uint8_t key) const noexcept
    {
        const auto* group = find(groups_,groupId);
        return group ? SelectSampleZone(group->data,key) : std::nullopt;
    }
    const Sample* sample(uint16_t id) const noexcept { return find(samples_,id); }
    size_t groupCount() const noexcept { return groups_.size(); }
    size_t sampleCount() const noexcept { return samples_.size(); }
private:
    template<class T> static const T* find(const std::vector<T>& records,uint16_t id) noexcept
    {
        const auto it = std::lower_bound(records.begin(),records.end(),id,
            [](const T& value,uint16_t key){return value.id < key;});
        return it != records.end() && it->id == id ? &*it : nullptr;
    }
    std::vector<Group> groups_;
    std::vector<Sample> samples_;
};
}

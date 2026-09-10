#pragma once
#include "sc55_rhythm_presets.h"
#include <algorithm>

namespace sc55
{
// Audio-owned, mutable drum maps. ROM presets are immutable input; protocol
// records and decoded note lookup cannot be edited independently by callers.
// No voice allocation, PCM writes, timer work or heap allocation occurs here.
class RhythmSettings
{
public:
    using WriteResult=RhythmPresetTable::WriteResult;
    using Record=RhythmPresetTable::Record;
    struct KeyOutput { uint8_t level,pan,reverb,chorus; };

    void reset(const Record& preset) noexcept
    {
        records_.fill(preset);
        maps_.fill(RhythmPresetTable::decode(preset));
    }
    void selectPreset(unsigned map,const Record& preset) noexcept
    {
        records_[map]=preset;
        maps_[map]=RhythmPresetTable::decode(preset);
    }
    WriteResult write(std::span<const uint8_t> payload) noexcept
    {
        if(payload.size()<3) return WriteResult::invalidLength;
        if(payload[0]==0x49) {
            const auto result=RhythmPresetTable::writeBulk(records_,payload);
            if(result.status==WriteResult::applied)
                maps_[result.map]=RhythmPresetTable::decode(records_[result.map]);
            return result.status;
        }
        if(payload[0]!=0x41 || (payload[1]>>4)>=maps_.size()) return WriteResult::unsupported;
        const unsigned map=payload[1]>>4;
        const auto result=RhythmPresetTable::write(records_[map],payload[1]&15,payload[2],payload.subspan(3));
        if(result==WriteResult::applied) maps_[map]=RhythmPresetTable::decode(records_[map]);
        return result;
    }
    bool writeNrpn(unsigned map,unsigned program,uint8_t msb,uint8_t key,uint8_t value,
        const RhythmPresetTable& presets) noexcept
    {
        if(map>=maps_.size() || key>=128) return false;
        if(msb==0x18) {
            const auto applied=presets.writeRelativePitch(records_[map],program,key,value);
            maps_[map].pitches[key]=records_[map][0x180+key];
            return applied;
        }
        const auto offset=RhythmPresetTable::nrpnOutputOffset(msb,key);
        if(!offset) return false;
        records_[map][*offset]=value;
        return true;
    }
    // Read-only views for note admission and the optional protocol diagnostics.
    // Internal callers supply a validated map (0/1) and MIDI key (0..127).
    const RhythmKeyMap& map(unsigned index) const noexcept { return maps_[index]; }
    const auto& records() const noexcept { return records_; }
    KeyOutput output(unsigned map,unsigned key) const noexcept
    {
        const auto& record=records_[map];
        return {record[0x100+key],record[0x280+key],record[0x380+key],record[0x300+key]};
    }
    std::array<char,12> name(unsigned map) const noexcept
    {
        std::array<char,12> result;
        std::copy_n(records_[map].begin()+0x480,result.size(),result.begin());
        return result;
    }
private:
    std::array<Record,2> records_{};
    std::array<RhythmKeyMap,2> maps_{};
};
}

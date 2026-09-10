#pragma once
#include "sc55_synth_state.h"

namespace sc55
{
// Resolved by the panel owner before crossing to the audio owner. No button
// identifiers: part is a front-panel part number (0..15), not a GS index.
// Relative sound edits intentionally apply to the audio owner's latest value,
// so a preceding MIDI controller change is not overwritten by a stale UI copy.
struct SynthCommand
{
    enum class Kind { focus, adjust, toggleMute, solo, standby,
                      receiveExclusive, receiveReset, ignoreChecksum, receiveProgramChanges };
    Kind kind=Kind::focus;
    uint8_t part=0;
    bool all=false;
    PartParameter parameter=PartParameter::program;
    int delta=0;
    bool enabled=false;
    bool cancelBitmap=false;
};
}

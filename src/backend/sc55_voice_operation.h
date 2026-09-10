#pragma once
#include <cstdint>

namespace sc55
{
// Pending work on a physical voice, not an H8 kernel task or allocation state.
// Preserve the encodings solely for differential firmware diagnostics.
enum class VoiceOperation : uint8_t { none=0, prepare=2, finishStop=4 };
}

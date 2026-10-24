#pragma once
// The SC-55's reverb and chorus. Both live in one 16,384-word delay memory
// (512 ms at 32 kHz) that every voice frame steps through by one word: a value
// written at position W is read back at position R after (R - W) frames. A
// reverb or delay type is a set of such positions and gains.
//
//   reverb send -> low-pass -> 4 diffusing all-passes -> two tank halves,
//   each an all-pass, a damped feedback from the other half (scaled by the
//   spread envelope) and a looped all-pass -> 4 output taps per side
//   chorus send -> low-pass -> memory -> two read heads swept by an
//   oscillator, interpolating between neighbouring words
//
// Returns go to the voice buses as (dry, send) pairs: reverb left/right, then
// the chorus four times (see MixVoiceAndEffectBuses).

#include "sc55_chorus_oscillator.h"
#include "sc55_chorus_setup.h"
#include "sc55_effect_parameter.h"
#include "sc55_envelope_ramp.h"
#include "sc55_reverb_setup.h"
#include <array>
#include <cstdint>

namespace sc55
{
class EffectUnit
{
public:
    static constexpr unsigned memoryWords = 0x4000;

    // A gain pair as the controller encodes it: signed eighths of four (the
    // high byte feeds back, the low byte passes on); bits 4-5 enable feedback.
    struct Gain
    {
        float feedback = 0, feedbackHalf = 0, forward = 0;
        bool recirculates = false;
        static Gain decode(uint16_t word) noexcept
        {
            const float high = float(int8_t(word >> 8)) * (1.0f / 32.0f);
            return {high, high * 0.5f, float(int8_t(word & 255)) * (1.0f / 32.0f), (word & 0x30) != 0};
        }
    };

    void configureReverb(const ReverbSetup& setup) noexcept
    {
        for (unsigned i = 0; i < 5; ++i) layout_.diffuser[i] = setup.diffusionTaps[i];
        layout_.leftTankEnd = setup.diffusionTaps[5];
        layout_.leftOutput = {setup.diffusionTaps[6], setup.diffusionTaps[10], setup.tailTaps[2], setup.tailTaps[6]};
        layout_.rightOutput = {setup.diffusionTaps[7], setup.diffusionTaps[11], setup.tailTaps[3], setup.tailTaps[7]};
        layout_.leftLoop = setup.diffusionTaps[8];
        layout_.leftLoopEnd = setup.diffusionTaps[9];
        layout_.rightTankStart = setup.tailTaps[0];
        layout_.rightTankEnd = setup.tailTaps[1];
        layout_.rightLoop = setup.tailTaps[4];
        layout_.rightLoopEnd = setup.tailTaps[5];
        layout_.rightDampingTap = setup.tailTaps[8];
        for (unsigned i = 0; i < 2; ++i) {
            gains_.diffusion[i] = Gain::decode(setup.diffusion[i]);
            gains_.damping[i] = Gain::decode(setup.damping[i]);
            gains_.reverbReturn[i] = Gain::decode(setup.output);
        }
        gains_.tank = Gain::decode(setup.comb);
        spread_.command = setup.spreadCommand;
        // Sound already in the memory and the spread level carry on.
    }

    void configureChorus(const ChorusSetup& setup) noexcept
    {
        chorus_ = {setup.position, setup.begin, setup.end, 0, setup.increment, false, true, false};
        layout_.chorusInput = 0x3800;
        for (unsigned i = 0; i < 4; ++i) gains_.chorusReturn[i] = Gain::decode(setup.returns[i]);
    }

    void update(EffectParameter parameter, uint16_t value) noexcept
    {
        using P = EffectParameter;
        const auto gain = Gain::decode(value);
        switch (parameter) {
        case P::reverbInput: gains_.reverbInput = gain; break;
        case P::reverbOutput: gains_.reverbReturn.fill(gain); break;
        case P::reverbSpread: spread_.command = value; break;
        case P::chorusInput: gains_.chorusInput = gain; break;
        case P::chorusLevel:
            gains_.chorusReturn[0] = gain;
            gains_.chorusReturn[3] = Gain::decode(value & 0xff00);
            break;
        case P::chorusFeedback: gains_.chorusReturn[1] = gain; break;
        case P::chorusSend: gains_.chorusReturn[0] = gain; break;
        }
    }

    void setChorusMix(const std::array<uint8_t, 3>& mix) noexcept
    {
        update(EffectParameter::chorusLevel, uint16_t((mix[0] << 8) | mix[2]));
        update(EffectParameter::chorusFeedback, mix[1]);
        gains_.chorusReturn[2] = {};
    }

    // Reverb off: no input, no returns, the network collapsed onto a few
    // adjacent words so the memory empties. The tail already there plays out.
    void beginReverbDrain() noexcept
    {
        spread_.command = 0xb6;
        gains_.reverbInput = {}; gains_.reverbReturn = {};
        gains_.diffusion = {}; gains_.tank = {}; gains_.damping = {};
        for (unsigned i = 0; i < 5; ++i) layout_.diffuser[i] = uint16_t(i);
        layout_.leftTankEnd = 5;
        layout_.leftLoop = 6; layout_.leftLoopEnd = 7;
        layout_.rightTankStart = 0x2000; layout_.rightTankEnd = 0x2001;
        layout_.rightLoop = 0x2002; layout_.rightLoopEnd = 0x2003;
    }

    // One frame. `reverbSend` and `chorusSend` are the previous frame's buses.
    void process(EnvelopeClock clock, bool active, float reverbSend, float chorusSend, float dry[6], float sends[6]) noexcept
    {
        // Chorus read heads blend each word with the next by the oscillator's
        // phase, in 64ths; the two heads move in opposite directions.
        const unsigned phase = chorus_.phase;
        const unsigned forward = chorus_.descending ? phase : 0x4000 - phase;
        const unsigned backward = chorus_.descending ? 0x4000 - phase : phase;
        const float leftBlend = float(int8_t(forward >> 8)) * (1.0f / 32.0f);
        const float rightBlend = float(int8_t(backward >> 8)) * (1.0f / 32.0f);
        spread_.advance(EnvelopeRamp::Stage::secondGain, clock, active);
        const float spread = float(int8_t(spread_.level >> 8)) * (1.0f / 32.0f);

        processReverbAndChorus(clock.phase, spread, leftBlend, rightBlend, reverbSend, chorusSend, dry, sends);

        chorus_.advance(clock.update, active);
        leftHead_ = chorus_.leftTap();
        rightHead_ = chorus_.rightTap();
    }

private:
    static float limit20(float v) noexcept
    {
        constexpr float top = 524288.0f; // the 20-bit range
        return v > top - 1.0f ? top - 1.0f : (v < -top ? -top : v);
    }

    void processReverbAndChorus(uint16_t now, float spread, float leftBlend, float rightBlend,
        float reverbSend, float chorusSend, float dry[6], float sends[6]) noexcept
    {
        const auto at = [&](uint16_t position) { return memory_[(position + now) & (memoryWords - 1)]; };
        const auto half = [&](uint16_t position) { return at(position) * 0.5f; };
        const auto after = [&](uint16_t position) { return memory_[(position + now + 1) & (memoryWords - 1)]; };
        const auto put = [&](uint16_t position, float v) { memory_[(position + now) & (memoryWords - 1)] = limit20(v); };
        const auto& L = layout_;
        const auto& G = gains_;

        // Sends through one-pole low-passes.
        chorusInput_ = (chorusInput_ * G.chorusInput.feedback + chorusSend * G.chorusInput.forward) * 0.5f;
        reverbInput_ = (reverbInput_ * G.reverbInput.feedback + reverbSend * G.reverbInput.forward) * 0.5f;

        // Four diffusing all-passes over adjacent stretches of memory:
        // diffuser k writes at diffuser[k] and reads at diffuser[k + 1].
        const auto& d1 = G.diffusion[0];
        const float node1 = reverbInput_ * d1.feedbackHalf - (d1.recirculates ? half(L.diffuser[1]) : 0.0f);
        const float out1 = node1 * d1.forward * 0.5f + at(L.diffuser[1]);
        const float node2 = out1 - (d1.recirculates ? half(L.diffuser[2]) : 0.0f);
        const float out2 = node2 * d1.forward * 0.5f + at(L.diffuser[2]);
        const float node3 = out2 - (d1.recirculates ? half(L.diffuser[3]) : 0.0f);
        const float out3 = node3 * d1.forward * 0.5f + at(L.diffuser[3]);
        float leftTank = at(L.leftTankEnd);
        const auto& d4 = G.diffusion[1];
        const float node4 = out3 - (d4.recirculates ? half(L.diffuser[4]) : 0.0f);
        const float diffused = node4 * d4.forward * 0.5f + at(L.diffuser[4]);
        float rightTank = at(L.rightTankEnd);

        // Damped cross-feedback: each half hears the other's tank input.
        leftDamping_ = (leftDamping_ * G.damping[0].feedback + at(L.rightTankStart) * G.damping[0].forward) * 0.5f;
        put(L.diffuser[0], node1);
        rightDamping_ = (rightDamping_ * G.damping[1].feedback + at(L.rightDampingTap) * G.damping[1].forward) * 0.5f;
        put(L.diffuser[1], node2);
        put(L.diffuser[2], node3);
        const float leftIn = diffused + leftDamping_ * spread * 0.5f;
        const float rightIn = diffused + rightDamping_ * spread * 0.5f;
        put(L.diffuser[3], node4);

        // Tank all-passes, then a looped all-pass on each side.
        const auto& t = G.tank;
        const float leftNode = leftIn + leftTank * t.feedback * 0.5f;
        leftTank = leftTank + leftNode * t.forward * 0.5f;
        const float leftLoopIn = at(L.leftLoopEnd);
        const float rightNode = rightIn + rightTank * t.feedback * 0.5f;
        rightTank = rightTank + rightNode * t.forward * 0.5f;
        const float rightLoopIn = at(L.rightLoopEnd);
        const float leftLoopNode = leftLoopIn * t.feedback * 0.5f + at(L.leftLoop);
        const float leftLoopOut = leftLoopNode * t.forward * 0.5f + leftLoopIn;
        put(L.leftTankStart(), leftNode);
        const float rightLoopNode = rightLoopIn * t.feedback * 0.5f + at(L.rightLoop);
        const float rightLoopOut = rightLoopNode * t.forward * 0.5f + rightLoopIn;
        put(L.leftTankEnd, leftTank);
        put(L.rightTankStart, rightNode);

        // Output taps.
        const float left = (at(L.leftOutput[3]) + at(L.leftOutput[2])) + (at(L.leftOutput[0]) + at(L.leftOutput[1]));
        const float rightA = at(L.rightOutput[0]) + at(L.rightOutput[1]);
        const float rightB = at(L.rightOutput[2]);
        const float right = (at(L.rightOutput[3]) + rightA) + rightB;
        put(L.rightTankEnd, rightTank);
        put(L.leftLoop, leftLoopNode);

        dry[0] = left * G.reverbReturn[0].feedback;
        sends[0] = left * G.reverbReturn[0].forward;
        const float leftNext = after(leftHead_);
        put(L.leftLoopEnd, leftLoopOut);
        dry[1] = right * G.reverbReturn[1].feedback;
        sends[1] = right * G.reverbReturn[1].forward;
        const float rightNext = after(rightHead_);

        // Chorus: two swept heads, each between a word and the next.
        const float leftWord = at(leftHead_);
        put(L.rightLoop, rightLoopNode);
        const float chorusLeft = (leftWord - leftWord * leftBlend * 0.5f) + leftNext * leftBlend * 0.5f;
        const float rightWord = at(rightHead_);
        put(L.rightLoopEnd, rightLoopOut);
        const float chorusRight = (rightWord - rightWord * rightBlend * 0.5f) + rightNext * rightBlend * 0.5f;
        put(L.chorusInput, chorusInput_);

        const auto& c = G.chorusReturn;
        dry[2] = chorusLeft * c[0].feedback;  sends[2] = chorusLeft * c[0].forward;
        dry[3] = chorusLeft * c[1].feedback;  sends[3] = chorusLeft * c[1].forward;
        dry[4] = chorusRight * c[2].feedback; sends[4] = chorusRight * c[2].forward;
        dry[5] = chorusRight * c[3].feedback; sends[5] = chorusRight * c[3].forward;
        for (int i = 0; i < 6; ++i) { dry[i] = limit20(dry[i]); sends[i] = limit20(sends[i]); }
        reverbInput_ = limit20(reverbInput_); chorusInput_ = limit20(chorusInput_);
        leftDamping_ = limit20(leftDamping_); rightDamping_ = limit20(rightDamping_);
    }

    // Memory positions, from the controller's reverb and chorus setups.
    struct Layout
    {
        std::array<uint16_t, 5> diffuser{};      // all-pass k: writes [k], reads [k + 1]
        uint16_t leftTankEnd = 0;                // left tank all-pass reads here (written at diffuser[4])
        uint16_t leftLoop = 0, leftLoopEnd = 0;  // left looped all-pass
        uint16_t rightTankStart = 0, rightTankEnd = 0;
        uint16_t rightLoop = 0, rightLoopEnd = 0;
        uint16_t rightDampingTap = 0;
        std::array<uint16_t, 4> leftOutput{}, rightOutput{};
        uint16_t chorusInput = 0;
        uint16_t leftTankStart() const noexcept { return diffuser[4]; }
    };
    struct Gains
    {
        Gain reverbInput, chorusInput, tank;
        std::array<Gain, 2> diffusion{}, damping{}, reverbReturn{};
        std::array<Gain, 4> chorusReturn{};
    };

    Layout layout_;
    Gains gains_;
    ChorusOscillator chorus_;
    EnvelopeRamp spread_;
    uint16_t leftHead_ = 0, rightHead_ = 0;
    float reverbInput_ = 0, chorusInput_ = 0, leftDamping_ = 0, rightDamping_ = 0;
    std::array<float, memoryWords> memory_{};
};
}

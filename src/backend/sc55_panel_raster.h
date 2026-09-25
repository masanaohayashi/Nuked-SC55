#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sc55
{
// Private 7D/"SC55" SysEx extension for a complete 741x268, four-colour LCD
// overlay. Packet assembly and frame staging are fixed-size and allocation-free
// so the plug-in can accept packets on the audio thread.
class PanelRasterSysExController
{
public:
    static constexpr unsigned width = 741;
    static constexpr unsigned height = 268;
    static constexpr std::size_t pixelCount = std::size_t(width) * height;
    static_assert(width % 3 == 0);
    static constexpr std::size_t rasterBytes = pixelCount / 3;
    static constexpr std::size_t maxChunkBytes = 128;
    static constexpr std::size_t maxMessageBytes = 142; // Includes F0 and F7.

    enum class PacketResult { ignored, accepted, committed, cleared, disabled, rejected };

    // Recognised packets are consumed even if malformed or disabled. The
    // non-commercial manufacturer ID and signature keep the extension separate
    // from Roland's existing model-45 text/CGRAM messages.
    PacketResult receiveSysEx(std::span<const uint8_t> message,
                              bool receiveEnabled = true) noexcept
    {
        if (! hasSignature(message)) return PacketResult::ignored;
        if (! receiveEnabled) return PacketResult::disabled;
        if (message.size() > maxMessageBytes || message.size() < 10
            || message.front() != 0xf0 || message.back() != 0xf7
            || message[6] != 1)
            return PacketResult::rejected;

        for (std::size_t i = 1; i + 1 < message.size(); ++i)
            if (message[i] >= 0x80) return PacketResult::rejected;

        unsigned checksum = 0;
        // The extension checksum covers signature through checksum, excluding
        // F0, manufacturer ID 7D, and F7.
        for (std::size_t i = 2; i + 1 < message.size(); ++i)
            checksum += message[i];
        if ((checksum & 0x7f) != 0) return PacketResult::rejected;

        const auto command = message[7];
        switch (command)
        {
            case 1: return beginFrame(message);
            case 2: return receiveChunk(message);
            case 3: return commitFrame(message);
            case 4: return clearFrame(message);
            default: return PacketResult::rejected;
        }
    }

    // Called only while MIDI/audio processing is stopped. A newly selected
    // emulator instance must not inherit a partial or displayed raster.
    void reset() noexcept
    {
        transferActive_ = false;
        receivedBytes_ = 0;
        coverage_.fill(0);
        publish(nullptr, false);
    }

    // Message-thread reader. On a repeated read of the same committed frame,
    // contentChanged is false and the caller's previously copied pixels remain
    // valid. Pixel codes are 0=transparent/background, 1=black, 2=panel orange,
    // 3=white. The destination uses one byte per pixel and may have row padding.
    bool copyLatestToMask(uint8_t* destination, std::size_t destinationStride,
                          bool* contentChanged = nullptr, bool forceCopy = false) noexcept
    {
        if (contentChanged != nullptr) *contentChanged = false;
        if (destination == nullptr || destinationStride < width) return false;
        if (middle_.load(std::memory_order_acquire) & dirty)
            front_ = middle_.exchange(front_, std::memory_order_acq_rel) & indexMask;

        const auto& snapshot = snapshots_[front_];
        if (! snapshot.active)
        {
            readerActive_ = false;
            return false;
        }

        const bool changed = forceCopy || ! readerActive_ || readerRevision_ != snapshot.revision;
        readerActive_ = true;
        readerRevision_ = snapshot.revision;
        if (contentChanged != nullptr) *contentChanged = changed;
        if (! changed) return true;

        for (unsigned y = 0; y < height; ++y)
        {
            const auto* source = snapshot.bytes.data() + std::size_t(y) * (width / 3);
            auto* row = destination + std::size_t(y) * destinationStride;
            for (unsigned x = 0; x < width; x += 3)
            {
                const auto packed = source[x / 3];
                row[x] = packed & 0x03;
                row[x + 1] = (packed >> 2) & 0x03;
                row[x + 2] = (packed >> 4) & 0x03;
            }
        }
        return true;
    }

private:
    struct Snapshot
    {
        std::array<uint8_t, rasterBytes> bytes {};
        uint64_t revision = 0;
        bool active = false;
    };

    static_assert(std::atomic<unsigned>::is_always_lock_free);
    static constexpr unsigned dirty = 4;
    static constexpr unsigned indexMask = 3;

    static bool hasSignature(std::span<const uint8_t> message) noexcept
    {
        constexpr std::array<uint8_t, 5> signature {0x7d, 'S', 'C', '5', '5'};
        return message.size() >= 1 + signature.size()
            && message[0] == 0xf0
            && std::equal(signature.begin(), signature.end(), message.begin() + 1);
    }

    PacketResult beginFrame(std::span<const uint8_t> message) noexcept
    {
        if (message.size() != 11) return PacketResult::rejected;
        transferFrame_ = message[8];
        transferActive_ = true;
        receivedBytes_ = 0;
        coverage_.fill(0);
        return PacketResult::accepted;
    }

    PacketResult receiveChunk(std::span<const uint8_t> message) noexcept
    {
        // Prefix is seven bytes, followed by frame ID, a 21-bit 7-bit-grouped
        // byte offset, 1..128 bytes of raster data, checksum, and F7.
        if (message.size() < 15 || message[8] != transferFrame_ || ! transferActive_)
            return PacketResult::rejected;
        const auto offset = (std::size_t(message[9]) << 14)
                          | (std::size_t(message[10]) << 7)
                          | std::size_t(message[11]);
        const auto dataBytes = message.size() - 14;
        if (dataBytes > maxChunkBytes || offset > rasterBytes
            || dataBytes > rasterBytes - offset)
            return PacketResult::rejected;

        for (std::size_t i = 0; i < dataBytes; ++i)
        {
            const auto value = message[12 + i];
            if (value & 0x40) return PacketResult::rejected;
        }
        for (std::size_t i = 0; i < dataBytes; ++i)
        {
            const auto position = offset + i;
            const auto mask = uint8_t(1u << (position & 7));
            auto& coverage = coverage_[position >> 3];
            if ((coverage & mask) == 0)
            {
                coverage |= mask;
                ++receivedBytes_;
            }
            staging_[position] = message[12 + i];
        }
        return PacketResult::accepted;
    }

    PacketResult commitFrame(std::span<const uint8_t> message) noexcept
    {
        if (message.size() != 11 || ! transferActive_ || message[8] != transferFrame_
            || receivedBytes_ != rasterBytes)
            return PacketResult::rejected;
        publish(staging_.data(), true);
        transferActive_ = false;
        return PacketResult::committed;
    }

    PacketResult clearFrame(std::span<const uint8_t> message) noexcept
    {
        if (message.size() != 10) return PacketResult::rejected;
        transferActive_ = false;
        receivedBytes_ = 0;
        publish(nullptr, false);
        return PacketResult::cleared;
    }

    void publish(const uint8_t* bytes, bool active) noexcept
    {
        auto& back = snapshots_[back_];
        if (active)
            std::copy_n(bytes, rasterBytes, back.bytes.begin());
        back.revision = ++revision_;
        back.active = active;
        back_ = middle_.exchange(back_ | dirty, std::memory_order_acq_rel) & indexMask;
    }

    std::array<uint8_t, rasterBytes> staging_ {};
    std::array<uint8_t, (rasterBytes + 7) / 8> coverage_ {};
    std::array<Snapshot, 3> snapshots_ {};
    std::atomic<unsigned> middle_ {1};
    unsigned back_ = 0; // Audio/MIDI producer owns this slot.
    unsigned front_ = 2; // Message-thread consumer owns this slot.
    std::size_t receivedBytes_ = 0;
    uint64_t revision_ = 0;
    uint64_t readerRevision_ = 0;
    uint8_t transferFrame_ = 0;
    bool transferActive_ = false, readerActive_ = false;
};
}

#include "sc55_panel_raster.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "Panel raster SysEx: %s\n", message);
        std::exit(1);
    }
}

std::vector<uint8_t> packet(uint8_t command, std::span<const uint8_t> fields)
{
    std::vector<uint8_t> result {0xf0, 0x7d, 'S', 'C', '5', '5', 1, command};
    result.insert(result.end(), fields.begin(), fields.end());
    unsigned sum = 0;
    for (size_t i = 2; i < result.size(); ++i)
        sum += result[i];
    result.push_back(uint8_t(-sum) & 0x7f);
    result.push_back(0xf7);
    return result;
}

std::vector<uint8_t> begin(uint8_t frame)
{
    const std::array<uint8_t, 1> fields {frame};
    return packet(1, fields);
}

std::vector<uint8_t> chunk(uint8_t frame, size_t offset, std::span<const uint8_t> data)
{
    const std::array<uint8_t, 4> address {
        frame,
        uint8_t((offset >> 14) & 0x7f),
        uint8_t((offset >> 7) & 0x7f),
        uint8_t(offset & 0x7f)
    };
    std::vector<uint8_t> fields(address.begin(), address.end());
    fields.insert(fields.end(), data.begin(), data.end());
    return packet(2, fields);
}

std::vector<uint8_t> control(uint8_t command, uint8_t frame)
{
    const std::array<uint8_t, 1> fields {frame};
    return packet(command, fields);
}

uint8_t pixelAt(std::span<const uint8_t> packed, size_t pixel)
{
    return uint8_t((packed[pixel / 3] >> ((pixel % 3) * 2)) & 3);
}
}

int main()
{
    using Controller = sc55::PanelRasterSysExController;
    constexpr auto pixels = size_t(Controller::width) * Controller::height;
    constexpr auto rasterBytes = (pixels + 2) / 3;
    static_assert(pixels == 198588 && rasterBytes == 66196);

    Controller controller;
    std::vector<uint8_t> output(pixels + Controller::width, 0xaa);
    bool contentChanged = true;
    require(!controller.copyLatestToMask(output.data(), Controller::width + 1,
                                         &contentChanged) && !contentChanged,
            "an empty controller exposed a frame");

    const auto initial = begin(17);
    require(controller.receiveSysEx(initial) == Controller::PacketResult::accepted,
            "BEGIN was not accepted");

    std::array<uint8_t, Controller::maxChunkBytes> data {};
    for (size_t offset = 0; offset < rasterBytes; offset += data.size())
    {
        const auto count = std::min(data.size(), rasterBytes - offset);
        for (size_t i = 0; i < count; ++i)
            data[i] = uint8_t((offset + i) & 0x3f);
        const auto message = chunk(17, offset, std::span(data).first(count));
        require(message.size() <= Controller::maxMessageBytes,
                "maximum-sized chunk exceeded the MIDI packet bound");
        require(controller.receiveSysEx(message) == Controller::PacketResult::accepted,
                "a valid chunk was rejected");
    }
    const auto commit = control(3, 17);
    require(controller.receiveSysEx(commit) == Controller::PacketResult::committed,
            "a complete frame was not committed");
    require(controller.copyLatestToMask(output.data(), Controller::width + 1,
                                        &contentChanged) && contentChanged,
            "the committed frame was not available to the UI");

    for (size_t pixel : {size_t(0), size_t(1), size_t(2), size_t(740),
                         size_t(741), pixels - 2, pixels - 1})
    {
        const uint8_t sourceByte = uint8_t((pixel / 3) & 0x3f);
        require(output[(pixel / Controller::width) * (Controller::width + 1)
                       + pixel % Controller::width] == pixelAt(
                           std::span(&sourceByte, 1), pixel % 3),
                "packed pixel values did not arrive at their 741x268 coordinates");
    }
    require(output[Controller::width] == 0xaa,
            "the caller's row padding was overwritten");

    output[0] = 0xaa;
    require(controller.copyLatestToMask(output.data(), Controller::width + 1,
                                        &contentChanged) && !contentChanged
            && output[0] == 0xaa,
            "an unchanged frame was decoded again instead of reusing its image");

    require(controller.copyLatestToMask(output.data(), Controller::width + 1,
                                        &contentChanged, true) && contentChanged
            && output[0] == 0,
            "a newly opened editor could not force its initial frame copy");
    output[0] = 0;
    const auto prior = output[0];
    const auto nextBegin = begin(18);
    controller.receiveSysEx(nextBegin);
    const auto incompleteCommit = control(3, 18);
    require(controller.receiveSysEx(incompleteCommit) == Controller::PacketResult::rejected,
            "an incomplete frame was committed");
    controller.copyLatestToMask(output.data(), Controller::width + 1, &contentChanged);
    require(output[0] == prior,
            "an incomplete transfer replaced the previous committed frame");

    auto badChecksum = begin(19);
    badChecksum[badChecksum.size() - 2] ^= 1;
    require(controller.receiveSysEx(badChecksum) == Controller::PacketResult::rejected,
            "a bad checksum was accepted");
    require(controller.receiveSysEx(incompleteCommit, false) == Controller::PacketResult::disabled,
            "a disabled raster extension was processed");

    const auto clear = packet(4, {});
    require(controller.receiveSysEx(clear) == Controller::PacketResult::cleared,
            "CLEAR did not remove the custom frame");
    require(!controller.copyLatestToMask(output.data(), Controller::width + 1,
                                         &contentChanged),
            "CLEAR left the custom frame visible");

    const uint8_t ordinaryMidi[] {0x90, 60, 100};
    require(controller.receiveSysEx(ordinaryMidi) == Controller::PacketResult::ignored,
            "ordinary MIDI was mistaken for the raster extension");
    std::puts("Panel raster SysEx: bounded chunk transfer, checksum, commit, clear and 741x268 pixel mapping PASS");
}

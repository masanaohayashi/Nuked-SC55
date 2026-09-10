#include "sc55_sysex.h"
#include "sc55_part_settings.h"
#include "sc55_note_start.h"
#include "sc55_rhythm_presets.h"
#include <cstdio>
#include <stdexcept>
#include <vector>

using Receiver = sc55::SysExReceiver;
using Status = Receiver::Status;
static void require(bool value) { if (!value) throw std::runtime_error("Native SysEx regression"); }

int main()
{
    sc55::MidiDecoder decoder;
    Receiver receiver;
    sc55::MasterControls master;
    {
        std::array<sc55::RhythmPresetTable::Record,2> maps{};
        const auto before=maps;
        const uint8_t outside[]{0x49,0x0f,0,0,0};
        require(sc55::RhythmPresetTable::writeBulk(maps,outside).status
            ==sc55::RhythmPresetTable::WriteResult::invalidLength && maps==before);
        const uint8_t badAddress[]{0x49,2,1,0,0};
        require(sc55::RhythmPresetTable::writeBulk(maps,badAddress).status
            ==sc55::RhythmPresetTable::WriteResult::unsupported && maps==before);
        const uint8_t nonNibble[]{0x49,2,0,0x71,0x22};
        require(sc55::RhythmPresetTable::writeBulk(maps,nonNibble).status
            ==sc55::RhythmPresetTable::WriteResult::applied && maps[0][0x100]==0x32);
    }
    sc55::DisplayData display;
    Status last = Status::pending;
    unsigned applied = 0;
    bool ignoreChecksum = false;
    auto feed = [&](std::span<const uint8_t> bytes) {
        // Every byte is a separate host fragment; interleaved realtime is kept.
        for (auto byte : bytes) decoder.push(std::span(&byte,1),[&](auto event) {
            const auto result = receiver.receive(event,0x10,true,ignoreChecksum);
            if (event.kind == sc55::MidiDecoder::Kind::realtime) return;
            last = result.status;
            if (last == Status::roland && result.model == 0x42 && result.command == 0x12)
                if (master.write(result.payload) == sc55::MasterControls::WriteResult::applied) ++applied;
            if(last==Status::roland && result.model==0x45 && result.command==0x12)
                (void)display.write(result.payload);
        });
    };
    auto packet = [](std::initializer_list<uint8_t> payload) {
        std::vector<uint8_t> bytes{0xf0,0x41,0x10,0x42,0x12};
        unsigned sum = 0;
        for (auto byte : payload) { bytes.push_back(byte); sum += byte; }
        bytes.push_back(uint8_t((128-(sum&127))&127)); bytes.push_back(0xf7);
        return bytes;
    };
    auto volume = packet({0x40,0,4,37});
    auto textPacket=packet({0x10,0,0,'T',0,'S','T'}); textPacket[3]=0x45;
    feed(std::span(textPacket).first(textPacket.size()-1));
    require(display.textRevision==0);
    feed(std::span(textPacket).last(1));
    require(display.textRevision==1 && display.textLength==4 && display.text[1]==' ');
    auto corruptText=textPacket; corruptText[corruptText.size()-2]^=1;
    feed(corruptText); require(display.textRevision==1);
    std::array<uint8_t,67> dots{}; dots[0]=0x10; dots[1]=1;
    for(unsigned i=0;i<64;++i) dots[i+3]=uint8_t(i);
    require(display.write(dots)==sc55::DisplayData::WriteResult::applied
        && display.bitmapRevision==1 && display.bitmap[63]==63);
    require(display.write(std::span(dots).first(66))==sc55::DisplayData::WriteResult::invalidLength
        && display.bitmapRevision==1);
    dots[2]=1;
    require(display.write(dots)==sc55::DisplayData::WriteResult::invalidAddress
        && display.bitmapRevision==1);
    std::array<uint8_t,36> tooMuchText{}; tooMuchText[0]=0x10;
    require(display.write(tooMuchText)==sc55::DisplayData::WriteResult::invalidLength
        && display.textRevision==1);
    require(display.write(std::span(tooMuchText).first(35))==sc55::DisplayData::WriteResult::applied
        && display.textLength==32);
    feed(std::span(volume).first(volume.size()-1));
    require(master.volume == 100 && applied == 0);
    const uint8_t realtime[]{0xf8,0xfe}; feed(realtime);
    feed(std::span(volume).last(1));
    require(last == Status::roland && master.volume == 37 && applied == 1);
    auto bad = packet({0x40,0,4,99}); bad[bad.size()-2] ^= 1;
    feed(bad); require(last == Status::badChecksum && master.volume == 37);
    ignoreChecksum = true; feed(bad); ignoreChecksum = false;
    require(master.volume == 99);
    auto wrongDevice = volume; wrongDevice[2] = 0x11;
    feed(wrongDevice); require(last == Status::ignored && master.volume == 99);
    feed(std::span(volume).first(7));
    const uint8_t abort[]{0xf6}; feed(abort);
    const uint8_t eox[]{0xf7}; feed(eox); require(master.volume == 99);
    feed(volume); require(master.volume == 37);
    feed(packet({0x40,0,0,0,4,0,0})); require(master.tune == 1024);
    feed(packet({0x40,0,0,0,0,0,0})); require(master.tune == 24);
    feed(packet({0x40,0,0,15,15,15,15})); require(master.tune == 2024);
    feed(packet({0x40,0,0,0,5,0,0,100})); require(master.tune == 2024 && master.volume == 37);
    feed(packet({0x40,0,5,0})); require(master.keyShift == 40);
    feed(packet({0x40,0,5,127})); require(master.keyShift == 88);
    feed(packet({0x40,0,6,0})); require(master.pan == 1);
    feed(packet({0x40,0,6,23,85})); require(master.pan==23 && master.portamentoController==85);
    feed(packet({0x40,0,0x7e,84})); require(master.portamentoController==84);
    feed(packet({0x40,0,4,1,2})); require(master.volume == 1 && master.keyShift == 40);
    feed(volume);
    std::vector<uint8_t> longPacket{0xf0,0x41,0x10,0x42,0x12};
    longPacket.resize(300,0); longPacket.push_back(0xf7);
    feed(longPacket); require(last == Status::tooLong && master.volume == 37);
    feed(volume); require(last == Status::roland && master.volume == 37);
    const uint8_t gm[]{0xf0,0x7e,0x7f,9,1,0xf7}; feed(gm); require(last == Status::gmOn);
    const uint8_t off[]{0xf0,0x7e,0x7f,9,2,0xf7}; feed(off); require(last == Status::gmOff);
    const uint8_t addressedGm[]{0xf0,0x7e,0x10,9,1,0xf7};
    feed(addressedGm); require(last == Status::ignored);
    sc55::PartSettings parts;
    sc55::PartControllerState controllerSettings;
    const uint8_t crossing[]{0x40,0x21,0x0a,23,0,45};
    require(controllerSettings.writeSettings(crossing)==sc55::PartControllerState::WriteResult::applied
        && controllerSettings.parts[1].sourceSensitivity[0][10]==23
        && controllerSettings.parts[1].sourceSensitivity[1][0]==40
        && controllerSettings.parts[1].sourceSensitivity[1][1]==45);
    const uint8_t beyond[]{0x40,0x21,0x5a,71,10};
    require(controllerSettings.writeSettings(beyond)==sc55::PartControllerState::WriteResult::unsupported
        && controllerSettings.parts[1].sourceSensitivity[4][10]==71);
    const uint8_t tone[]{0x40,0x11,0x30,0,127,127,64,0,127,64,64};
    require(parts.write(tone)==sc55::PartSettings::WriteResult::applied
        && parts.parts[1].controls.tone.values[0]==14
        && parts.parts[1].controls.tone.values[1]==114
        && parts.parts[1].controls.tone.values[2]==80);
    require(parts.routing[0].channel == 9 && parts.routing[1].channel == 0
        && parts.routing[9].channel == 8 && parts.routing[10].channel == 10);
    parts.routing[2].channel = 0;
    const sc55::MidiDecoder::Event cc{sc55::MidiDecoder::Kind::message,0xb0,7,55,2};
    require(parts.applyScalar(cc));
    require(parts.parts[1].controls.volume == 55 && parts.parts[2].controls.volume == 55
        && parts.parts[0].controls.volume == 100);
    const uint8_t disableVolume[]{0x40,0x12,0x0c,0};
    require(parts.write(disableVolume) == sc55::PartSettings::WriteResult::applied);
    auto next = cc; next.second = 77; require(parts.applyScalar(next));
    require(parts.parts[1].controls.volume == 77 && parts.parts[2].controls.volume == 55);
    const uint8_t settings[]{0x40,0x11,0x19,32,33,34,0,50,40};
    require(parts.write(settings) == sc55::PartSettings::WriteResult::applied);
    require(parts.parts[1].controls.volume == 32 && parts.parts[1].controls.pan == 0
        && parts.parts[1].velocity.depth == 33 && parts.parts[1].velocity.offset == 34
        && parts.parts[1].keyRange.low == 50 && parts.parts[1].keyRange.high == 40);
    const uint8_t scale[]{0x40,0x11,0x40,65,66,67,68,69,70,71,72,73,74,75,76};
    require(parts.write(scale) == sc55::PartSettings::WriteResult::applied
        && parts.parts[1].scale[0] == 65 && parts.parts[1].scale[11] == 76
        && parts.parts[2].scale[0] == 64);
    require(parts.write(std::span(scale).first(14)) == sc55::PartSettings::WriteResult::invalidLength);
    const uint8_t changeChannel[]{0x40,0x11,2,127};
    require(parts.write(changeChannel)==sc55::PartSettings::WriteResult::unsupported
        && parts.routing[1].channel==0); // no side-effect owner supplied
    unsigned resets=0;
    for (unsigned i=0;i<2;++i)
        require(parts.write(changeChannel,[&](unsigned part) { require(part==1); ++resets; return true; })
            == sc55::PartSettings::WriteResult::applied);
    require(resets==2 && parts.routing[1].channel==16);
    sc55::VoiceAllocator allocator;
    require(allocator.initializeTables());
    const auto group=allocator.createGroup({1,0x80,60,1,2}); require(bool(group));
    std::array<uint8_t,16> retained; retained.fill(255);
    require(allocator.setPartHold(1,true,retained));
    require(allocator.requestGroupReleases(1,false,0x80,0,retained));
    require(allocator.noteGroups[group->group].status==2 && allocator.allocations[group->voices[0]].releaseCommand==0
        && allocator.freeCount==22);
    require(allocator.setPartHold(1,false,retained));
    require(allocator.allocations[group->voices[0]].releaseCommand==255 && allocator.freeCount==22);
    std::puts("Native SysEx PASS: fragmented/realtime/abort, checksum, size, recovery, master controls, GM framing");
}

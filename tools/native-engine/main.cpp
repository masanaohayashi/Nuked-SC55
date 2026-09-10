#include "sc55_synth.h"
#include "rom_loader.h"
#include "MidiFilePlayer.h"
#include "song_notes_probe.h"
#include <cstdio>

#if defined(SC55_CONTROL_TIMING_ORACLE) || defined(SC55_NATIVE_IO_AUDIT)
#error The native-only consumer must use normal product control code.
#endif

// Link/run check of the real public sound interface. H8 is deliberately not
// available to this executable, even as a reference or an idle fallback.
int main(int argc,char** argv)
{
    try {
        if(argc<2 || argc>4) {
            std::fprintf(stderr,"Usage: sc55-native-engine-check ROM_DIRECTORY\n");
            return 2;
        }
        common::LoadRomsetResult loaded;
        if(common::LoadRomset(argv[1],"mk1-v1.21",common::RomLoader::Hashing,{},loaded)
            !=common::LoadRomsetError{}) throw std::runtime_error("Could not load SC-55 v1.21 ROMs");
        const auto& rom=loaded.romset_info.rom_data;
        const auto& rom1=rom[size_t(RomLocation::ROM1)];
        const auto& rom2=rom[size_t(RomLocation::ROM2)];
        const auto encoded=sc55::ImportSoundData(rom1,rom2);
        if(argc==4 && std::string_view(argv[2])=="song-notes")
            return probeSongNotes(argv[3],rom,encoded);
        if(argc==4 && std::string_view(argv[2])=="song-release") {
            MidiFileData song;std::string error;
            if(!song.load(argv[3],error,false)) throw std::runtime_error(error);
            sc55::NativeSynth synth(encoded,rom1,rom2,
                rom[size_t(RomLocation::WAVEROM1)],rom[size_t(RomLocation::WAVEROM2)],
                rom[size_t(RomLocation::WAVEROM3)],sc55::NativeSynth::VoiceRendering::referenceChip,128);
            std::array<AudioFrame<int32_t>,257> audio{};
            uint64_t frame=0;unsigned maximum=0;
            auto renderUntil=[&](uint64_t until) {
                while(frame<until) {
                    const auto count=std::min<uint64_t>(audio.size(),until-frame);
                    synth.render(std::span(audio.data(),count)); frame+=count;
                    if(synth.failed()) throw std::runtime_error("Song engine failed");
                    unsigned voices=0;for(auto& part:synth.state().parts) voices+=part.voices;
                    maximum=std::max(maximum,voices);
                    if(voices>128) throw std::runtime_error("Voice ownership exceeds capacity");
                }
            };
            for(auto& event:song.events) {
                renderUntil(uint64_t(event.seconds*32000));
                if(synth.push(event.bytes)!=event.bytes.size()) throw std::runtime_error("Song MIDI queue overflow");
            }
            for(unsigned ch=0;ch<16;++ch) {
                const uint8_t off[]{uint8_t(0xb0|ch),64,0,66,0,123,0};synth.push(off);
            }
            renderUntil(frame+32000*20);
            unsigned voices=0;for(auto& part:synth.state().parts) voices+=part.voices;
            std::printf("song maximum=%u remaining=%u\n",maximum,voices);
            if(voices) throw std::runtime_error("Song leaves stuck voices after pedal and note release");
            return 0;
        }
        if(argc>=3 && std::string_view(argv[2])=="slot-audio") {
            const unsigned occupied=argc==4 ? unsigned(std::stoul(argv[3])) : 127;
            auto make=[&](unsigned limit) { return std::make_unique<sc55::NativeSynth>(encoded,rom1,rom2,
                rom[size_t(RomLocation::WAVEROM1)],rom[size_t(RomLocation::WAVEROM2)],
                rom[size_t(RomLocation::WAVEROM3)],sc55::NativeSynth::VoiceRendering::referenceChip,limit); };
            for(unsigned program=0;program<128;++program) {
            auto baseline=make(24),expanded=make(128);
            std::array<AudioFrame<int32_t>,257> a{},b{};
            auto render=[&] { baseline->render(a);expanded->render(b); };
            for(unsigned ch=1;ch<=8;++ch) {
                const uint8_t setup[]{uint8_t(0xc0|ch),16,uint8_t(0xb0|ch),7,0,91,0,93,0};
                expanded->push(setup);
            }
            for(unsigned i=0;i<32;++i) render();
            for(unsigned i=0;i<occupied;++i) {
                const uint8_t note[]{uint8_t(0x91+i/16),uint8_t(48+i%16),70};
                expanded->push(note);for(unsigned j=0;j<4;++j) render();
            }
            unsigned voices=0;for(auto& part:expanded->state().parts) voices+=part.voices;
            std::printf("occupied=%u actual=%u\n",occupied,voices);
            if(voices!=occupied) throw std::runtime_error("Silent fillers lost ownership");
            const uint8_t target[]{0xc0,uint8_t(program),0xb0,91,0,93,0,0x90,60,100};
            baseline->push(target);expanded->push(target);
            for(unsigned block=0;block<100;++block) {
                if(block==20 || block==40) {
                    const uint8_t bend[]{0xe0,0,uint8_t(block==20?80:64),0xb0,1,64};
                    baseline->push(bend);expanded->push(bend);
                }
                if(block==60) {
                    const uint8_t off[]{0x80,60,0};baseline->push(off);expanded->push(off);
                }
                render();
                for(unsigned i=0;i<a.size();++i)
                    if(a[i].left!=b[i].left || a[i].right!=b[i].right) {
                        std::fprintf(stderr,"slot audio mismatch program=%u occupied=%u block=%u sample=%u: %d,%d vs %d,%d\n",
                            program,occupied,block,i,a[i].left,a[i].right,b[i].left,b[i].right);
                        return 1;
                    }
            }
            }
            std::puts("PASS: relocated voice audio identical");return 0;
        }
        if(argc>=3 && std::string_view(argv[2])=="capacity-audio") {
            auto make=[&](unsigned limit) { return std::make_unique<sc55::NativeSynth>(encoded,rom1,rom2,
                rom[size_t(RomLocation::WAVEROM1)],rom[size_t(RomLocation::WAVEROM2)],
                rom[size_t(RomLocation::WAVEROM3)],sc55::NativeSynth::VoiceRendering::referenceChip,limit); };
            auto baseline=make(24), expanded=make(128);
            std::array<AudioFrame<int32_t>,257> a{},b{};
            for(unsigned program=0;program<128;++program) {
                const uint8_t reset[]{0xf0,0x7e,0x7f,0x09,0x01,0xf7};
                baseline->push(reset); expanded->push(reset);
                for(unsigned block=0;block<100;++block) {
                    if(block==32) {
                        const uint8_t midi[]{0xc0,uint8_t(program),0x90,60,100,64,100,67,100};
                        baseline->push(midi); expanded->push(midi);
                    }
                    if(block==64) {
                        const uint8_t off[]{0xb0,123,0};baseline->push(off);expanded->push(off);
                    }
                    baseline->render(a);expanded->render(b);
                    for(unsigned i=0;i<a.size();++i)
                        if(a[i].left!=b[i].left || a[i].right!=b[i].right) {
                            std::fprintf(stderr,"capacity audio mismatch program=%u block=%u sample=%u: %d,%d vs %d,%d\n",
                                program,block,i,a[i].left,a[i].right,b[i].left,b[i].right);
                            return 1;
                        }
                }
            }
            std::puts("PASS: all programs have identical audio at 24 and 128 capacity below voice stealing");
            return 0;
        }
        if(argc>=3 && std::string_view(argv[2])=="polyphony") {
            sc55::SoundData soundData;
            if(!soundData.loadEncoded(encoded)) throw std::runtime_error("Invalid test sound data");
            const auto presets=sc55::ImportMelodicPresets(rom1,rom2);
            unsigned pairedProgram=128;
            unsigned singleProgram=128;
            for(unsigned program=16;program<24 && singleProgram==128;++program) {
                const auto tone=presets.resolve(0,program);
                if(!tone) continue;
                bool single=true;
                for(unsigned key=36;key<100;++key) {
                    const auto note=sc55::PrepareMappedNoteVelocity(
                        {sc55::MidiDecoder::Kind::message,0x90,uint8_t(key),70,2},tone->tone,false,0,soundData);
                    single &= note && note->partials.candidates.count==1;
                }
                if(single) singleProgram=program;
            }
            if(singleProgram==128) throw std::runtime_error("No single-partial organ fixture");
            for(unsigned program=88;program<96 && pairedProgram==128;++program) {
                const auto tone=presets.resolve(0,program);
                if(!tone) continue;
                bool paired=true;
                for(unsigned key=36;key<100;++key) {
                    const auto note=sc55::PrepareMappedNoteVelocity(
                        {sc55::MidiDecoder::Kind::message,0x90,uint8_t(key),70,2},tone->tone,false,0,soundData);
                    paired &= note && note->partials.candidates.count==2;
                }
                if(paired) pairedProgram=program;
            }
            if(pairedProgram==128) throw std::runtime_error("No two-partial pad fixture");
            std::printf("single program=%u paired program=%u\n",singleProgram,pairedProgram);
            const unsigned firstLimit=argc==4 ? unsigned(std::stoul(argv[3])) : 24;
            for(unsigned limit=firstLimit;limit<=128;limit+=4) {
                sc55::NativeSynth poly(encoded,rom1,rom2,
                    rom[size_t(RomLocation::WAVEROM1)],rom[size_t(RomLocation::WAVEROM2)],
                    rom[size_t(RomLocation::WAVEROM3)],sc55::NativeSynth::VoiceRendering::referenceChip,limit);
                std::array<AudioFrame<int32_t>,257> audio{};
                auto render=[&] { poly.render(audio); if(poly.failed()) throw std::runtime_error("Polyphony engine failed"); };
                for(unsigned i=0;i<16;++i) render();
                for(unsigned ch=0;ch<9;++ch) {
                    const uint8_t program[]{uint8_t(0xc0|ch),uint8_t(singleProgram)}; poly.push(program);
                }
                for(unsigned i=0;i<limit;++i) {
                    const uint8_t note[]{uint8_t(0x90|(i/16)),uint8_t(48+i%16),70};
                    poly.push(note); for(unsigned j=0;j<4;++j) render();
                }
                const auto state=poly.state();
                unsigned voices=0;for(const auto& part:state.parts) voices+=part.voices;
                std::printf("polyphony limit=%u voices=%u\n",limit,voices); std::fflush(stdout);
                if(voices!=limit) throw std::runtime_error("Configured voice limit not reached");
                const auto audible=std::count_if(state.voiceLevels.begin(),state.voiceLevels.end(),
                    [](const auto& voice){return voice.active && voice.left && voice.right;});
                if(unsigned(audible)!=limit) throw std::runtime_error("Allocated voices lack PCM gain");
                if(std::none_of(audio.begin(),audio.end(),[](auto f){return f.left||f.right;}))
                    throw std::runtime_error("Polyphony output silent");
                const uint8_t extra[]{0x98,80,70}; poly.push(extra);
                for(unsigned j=0;j<32;++j) render();
                voices=0;for(const auto& part:poly.state().parts) voices+=part.voices;
                if(voices!=limit) throw std::runtime_error("Voice stealing changed configured limit");
                for(unsigned ch=0;ch<9;++ch) {
                    const uint8_t off[]{uint8_t(0xb0|ch),123,0}; poly.push(off);
                }
                for(unsigned j=0;j<1000;++j) render();
                voices=0;for(const auto& part:poly.state().parts) voices+=part.voices;
                if(voices) throw std::runtime_error("Expanded voices were not released");
                const uint8_t reset[]{0xf0,0x7e,0x7f,0x09,0x01,0xf7}; poly.push(reset);
                for(unsigned j=0;j<128;++j) render();
                const uint8_t strings[]{0xc0,uint8_t(pairedProgram)}; poly.push(strings);
                for(unsigned i=0;i<limit/2;++i) {
                    const uint8_t note[]{0x90,uint8_t(36+i),70};
                    poly.push(note); for(unsigned j=0;j<4;++j) render();
                }
                voices=0;for(const auto& part:poly.state().parts) voices+=part.voices;
                std::printf("after reset paired limit=%u voices=%u\n",limit,voices); std::fflush(stdout);
                if(voices!=limit) throw std::runtime_error("Reset lost capacity or paired voice allocation failed");
                if(limit==128) {
                    // Reset while all voices sound, then exercise mono ownership.
                    poly.push(reset); for(unsigned j=0;j<1000;++j) render();
                    const uint8_t mono[]{0xc0,uint8_t(singleProgram),0xb0,126,1};
                    poly.push(mono); for(unsigned j=0;j<32;++j) render();
                    const uint8_t monoFirst[]{0x90,60,70}; poly.push(monoFirst);
                    for(unsigned j=0;j<16;++j) render();
                    const uint8_t monoSecond[]{0x90,64,70}; poly.push(monoSecond);
                    for(unsigned j=0;j<32;++j) render();
                    voices=0;for(const auto& part:poly.state().parts) voices+=part.voices;
                    std::printf("mono voices=%u program=%u\n",voices,unsigned(poly.state().parts[1].program)); std::fflush(stdout);
                    if(voices!=1) throw std::runtime_error("128-voice mono mode retained multiple owners");
                    poly.push(reset); for(unsigned j=0;j<1000;++j) render();
                    for(unsigned ch=0;ch<8;++ch) {
                        const uint8_t program[]{uint8_t(0xc0|ch),uint8_t(singleProgram)}; poly.push(program);
                    }
                    for(unsigned i=0;i<128;++i) {
                        const uint8_t note[]{uint8_t(0x90|(i/16)),uint8_t(48+i%16),70};
                        poly.push(note); for(unsigned j=0;j<4;++j) render();
                    }
                    for(const uint8_t key:{36,38,45}) {
                        const uint8_t drum[]{0x99,key,127}; poly.push(drum);
                        for(unsigned j=0;j<4;++j) render();
                        const auto playing=poly.state();
                        voices=0;for(const auto& part:playing.parts) voices+=part.voices;
                        if(voices>128 || !playing.parts[0].voices)
                            throw std::runtime_error("Drum stealing failed at 128 voices");
                    }
                    std::puts("128-voice sounding reset, mono and drum stealing PASS");
                }
            }
            return 0;
        }
        sc55::NativeSynth synth(encoded,rom1,rom2,
            rom[size_t(RomLocation::WAVEROM1)],rom[size_t(RomLocation::WAVEROM2)],
            rom[size_t(RomLocation::WAVEROM3)]);
        std::array<AudioFrame<int32_t>,257> frames{};
        uint64_t checksum=14695981039346656037ull,nonzero=0;
        for(unsigned block=0;block<128;++block) {
            if(block==64 || block==96) {
                const uint8_t midi[]{uint8_t(block==64 ? 0x90 : 0x80),60,100};
                if(synth.push(midi)!=sizeof(midi)) throw std::runtime_error("MIDI input rejected");
            }
            synth.render({});
            synth.render(frames);
            if(synth.failed()) throw std::runtime_error("Native sound engine failed");
            if(block==80) {
                auto sounding=synth.state();sounding.calculateDisplayLevels();
                if(!sounding.parts[1].envelopeLevel)
                    throw std::runtime_error("Sounding note has no part meter");
            }
            for(const auto& frame:frames) {
                nonzero+=frame.left!=0 || frame.right!=0;
                for(const auto value:{uint32_t(frame.left),uint32_t(frame.right)})
                    for(unsigned byte=0;byte<4;++byte)
                        checksum=(checksum^uint8_t(value>>(byte*8)))*1099511628211ull;
            }
        }
        // Same deterministic Note On/Off sequence and reference-PCM output
        // as the existing normal product check; not a new fidelity oracle.
        if(!nonzero || checksum!=0x3b54320560580fd3ull)
            throw std::runtime_error("Native-only output differs from the normal product fixture");
        for(unsigned block=0;block<2000;++block) synth.render(frames);
        auto stopped=synth.state();stopped.calculateDisplayLevels();
        unsigned remainingVoices=0,remainingMeters=0;
        for(const auto& part:stopped.parts) {
            remainingVoices+=part.voices;
            remainingMeters+=part.envelopeLevel!=0;
        }
        std::printf("Ended note: voices=%u meters=%u pcm_keys=%x\n",remainingVoices,remainingMeters,stopped.activeVoiceMask);
        if(remainingVoices || remainingMeters)
            throw std::runtime_error("Ended note leaves a visible part meter");
        std::printf("PASS: native MIDI/control/PCM without H8 or JUCE; checksum=%016llx nonzero=%llu\n",
            (unsigned long long)checksum,(unsigned long long)nonzero);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"Native-only check failed: %s\n",error.what());
        return 1;
    }
}

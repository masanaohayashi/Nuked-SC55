// sc55lists - ボイス管理のリスト構造を不変条件で検査する。
//
// 算術は「入力を与えて 1 つの出力を比べる」で照合できるが、割り当ては状態機械なので
// それでは足りない。代わりに、こちらが理解した構造が実際に成り立っているかを
// 毎ティック突き合わせる。壊れたら理解が間違っている。
//
//   sc55lists <song.mid>
#include "NukedSC55Emulator.h"
#include "MidiFilePlayer.h"
#include "mcu.h"
#include <cstdio>
#include <string>
#include <vector>
extern uint8_t MCU_Read_Impl(mcu_t&,uint32_t);
extern void MCU_Write_Impl(mcu_t&,uint32_t,uint8_t);
extern void MCU_Write16_Impl(mcu_t&,uint32_t,uint16_t);
extern void MCU_Step_Impl(mcu_t&);
void MCU_Write16(mcu_t& m,uint32_t a,uint16_t v){ MCU_Write16_Impl(m,a,v); }
uint8_t MCU_Read(mcu_t& m,uint32_t a){ return MCU_Read_Impl(m,a); }
void MCU_Write(mcu_t& m,uint32_t a,uint8_t v){ MCU_Write_Impl(m,a,v); }

static const int VOICES = 24, PARTS = 16;
enum { FREE_HEAD=0xa3c3, FREE_COUNT=0xa3c1, FREE_NEXT=0xa378,
       PART_HEAD=0xa2b8, PREV=0xcac4, NEXT=0xcadc, PART_COUNT=0xa1f0, VOICE_PART=0xa318 };

static uint64_t checks=0, bad_free=0, bad_part=0, bad_owner=0, bad_cycle=0;
static int shown=0;

static void Verify(mcu_t& m)
{
    auto rb=[&](uint32_t a){ return (int)MCU_Read_Impl(m,a); };
    ++checks;

    // 空きリスト: 先頭から辿った長さが個数と一致し、閉路が無いこと
    bool in_free[VOICES] = {};
    int n = 0, v = rb(FREE_HEAD);
    while (v >= 0 && v < VOICES && n <= VOICES)
    {
        if (in_free[v]) { ++bad_cycle; return; }
        in_free[v] = true; ++n; v = (int8_t) rb(FREE_NEXT + v);
    }
    if (n != rb(FREE_COUNT)) { ++bad_free;
        if (shown<4){ std::printf("  空き個数 実機=%d 辿った長さ=%d\n", rb(FREE_COUNT), n); ++shown; } }

    // パートごとの発音数: 空きリストに載っていないボイスをパートで数えたものと一致するか。
    //
    // 最初は 0xa2b8 をパート単位のリスト先頭だと読んで、前後リンクまで含めて検査した。
    // 総崩れになった。0xa330[voice] = r2 という書き方から、0xa2b8 の添字はパートではなく
    // 別の識別子だと分かる。検査器が読み違いを捕まえた形なので、検査器ではなく前提を直す。
    int per_part[PARTS] = {};
    for (int i = 0; i < VOICES; ++i)
    {
        if (in_free[i]) continue;
        const int p = rb(VOICE_PART + i);
        if (p >= 0 && p < PARTS) ++per_part[p];
    }
    for (int p = 0; p < PARTS; ++p)
        if (per_part[p] != rb(PART_COUNT + p)) ++bad_part;
}

static bool tr=false; static int countdown=0;
void MCU_Step(mcu_t& m){
  if(tr && --countdown <= 0){ countdown = 20000; Verify(m); }
  MCU_Step_Impl(m); }

int main(int argc,char**argv){
  NukedSC55Emulator e; if(!e.initialise("/Users/ring2/Documents/Roland SC-55 v1.21",44100.0)) return 1;
  MidiFileData m; std::string err; std::vector<MidiFileEvent> ev;
  if(argc>1&&m.load(argv[1],err)) ev=m.events;
  std::vector<float> l(256),r(256);
  for(int i=0;i<8000;++i) e.render(l.data(),r.data(),64);
  tr=true; countdown=1; double t=0; size_t n=0; const double dt=256.0/44100.0;
  while(t<20.0){ while(n<ev.size()&&ev[n].seconds<=t){ if(!ev[n].bytes.empty()) e.sendMidi(ev[n].bytes.data(),(int)ev[n].bytes.size()); ++n; } e.render(l.data(),r.data(),256); t+=dt; }
  tr=false;
  std::printf("検査 %llu 回   空きリスト不一致 %llu   パートリスト不一致 %llu   所属不一致 %llu   閉路 %llu\n",
    (unsigned long long)checks,(unsigned long long)bad_free,(unsigned long long)bad_part,
    (unsigned long long)bad_owner,(unsigned long long)bad_cycle);
  return 0; }

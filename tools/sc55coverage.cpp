// sc55coverage - 照合済みのコード範囲が、実行全体のどれだけを占めるかを測る。
//
// ネイティブ実装で置き換えられる分の見積もりに使う。範囲は下の表に手で書く。
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

struct Range { uint16_t lo, hi; const char* what; uint64_t hits; };
static Range R[] = {
    { 0x309b, 0x3188, "音量合成 + 修飾", 0 },
    { 0x35c9, 0x36a9, "エンベロープ生成 + 符号化", 0 },
    { 0x36db, 0x3735, "TVA", 0 },
    { 0x37b1, 0x37fc, "エフェクト送り", 0 },
    { 0x3b26, 0x3d00, "LFO + 遅延／立ち上がり", 0 },
    { 0x3e30, 0x3e6d, "フィルタモード", 0 },
    { 0x41e4, 0x4251, "レゾナンス", 0 },
    { 0x473c, 0x47fc, "カットオフ", 0 },
    { 0x488f, 0x48b4, "ピッチ基準", 0 },
    { 0x51b0, 0x5279, "ピッチ変換", 0 },
    { 0x5300, 0x5368, "ピッチ書き出し", 0 },
    { 0x5cee, 0x5ec9, "コントローラ目盛り", 0 },
};
static bool tr=false; static uint64_t total=0;
void MCU_Step(mcu_t& m){
  if(tr){ ++total;
    if(m.cp==0) for(auto&r:R) if(m.pc>=r.lo&&m.pc<r.hi){ ++r.hits; break; } }
  MCU_Step_Impl(m); }
int main(int argc,char**argv){
  NukedSC55Emulator e; if(!e.initialise("/Users/ring2/Documents/Roland SC-55 v1.21",44100.0)) return 1;
  MidiFileData m; std::string err; std::vector<MidiFileEvent> ev;
  if(argc>1&&m.load(argv[1],err)) ev=m.events;
  std::vector<float> l(256),r(256);
  for(int i=0;i<8000;++i) e.render(l.data(),r.data(),64);
  tr=true; double t=0; size_t n=0; const double dt=256.0/44100.0;
  while(t<20.0){ while(n<ev.size()&&ev[n].seconds<=t){ if(!ev[n].bytes.empty()) e.sendMidi(ev[n].bytes.data(),(int)ev[n].bytes.size()); ++n; } e.render(l.data(),r.data(),256); t+=dt; }
  tr=false;
  uint64_t sum=0;
  std::printf("実行命令 %llu\n", (unsigned long long)total);
  for(auto&r:R){ sum+=r.hits;
    std::printf("  %-28s %8llu  %5.2f%%\n", r.what,(unsigned long long)r.hits,100.0*(double)r.hits/(double)total); }
  std::printf("  %-28s %8llu  %5.2f%%\n","照合済み合計",(unsigned long long)sum,100.0*(double)sum/(double)total);
  return 0; }

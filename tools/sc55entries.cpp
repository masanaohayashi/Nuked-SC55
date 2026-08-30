// sc55entries - 実行から関数の入口（分岐先）を割り出す。
//
// H8/500 は可変長命令なので、適当なアドレスから逆アセンブルすると、もっともらしく
// 読めて中身の違う listing が出る。整列を保証する唯一の方法は、実際に制御が飛んだ先
// から読み始めること。前の命令から 8 バイト以上離れて到達した PC を入口として数える。
//
//   sc55entries <lo> <hi> <song.mid>
#include "NukedSC55Emulator.h"
#include "MidiFilePlayer.h"
#include "mcu.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
extern uint8_t MCU_Read_Impl(mcu_t&,uint32_t);
extern void MCU_Write_Impl(mcu_t&,uint32_t,uint16_t);
extern void MCU_Step_Impl(mcu_t&);
extern void MCU_Write_Impl(mcu_t&,uint32_t,uint8_t);
extern void MCU_Write16_Impl(mcu_t&,uint32_t,uint16_t);
void MCU_Write16(mcu_t& m,uint32_t a,uint16_t v){ MCU_Write16_Impl(m,a,v); }
uint8_t MCU_Read(mcu_t& m,uint32_t a){ return MCU_Read_Impl(m,a); }
void MCU_Write(mcu_t& m,uint32_t a,uint8_t v){ MCU_Write_Impl(m,a,v); }

static bool tr=false; static uint32_t lo=0,hi=0,prev=0xffffffffu;
static std::map<uint16_t,uint64_t> entries, all;

void MCU_Step(mcu_t& m){
  if(tr && m.cp==0){
    const uint32_t pc=m.pc;
    if(pc>=lo && pc<hi){
      all[(uint16_t)pc]++;
      // 直前の命令から遠く離れて来たなら、そこは分岐先＝整列の保証がある
      if(prev==0xffffffffu || pc<prev || pc-prev>8) entries[(uint16_t)pc]++;
    }
    prev=pc;
  }
  MCU_Step_Impl(m);
}
int main(int argc,char**argv){
  if(argc<4){ std::printf("使い方: sc55entries <lo> <hi> <song.mid>\n"); return 1; }
  lo=(uint32_t)std::strtoul(argv[1],nullptr,16); hi=(uint32_t)std::strtoul(argv[2],nullptr,16);
  NukedSC55Emulator e; if(!e.initialise("/Users/ring2/Documents/Roland SC-55 v1.21",44100.0)) return 1;
  MidiFileData m; std::string err; std::vector<MidiFileEvent> ev;
  if(m.load(argv[3],err)) ev=m.events;
  std::vector<float> l(256),r(256);
  for(int i=0;i<8000;++i) e.render(l.data(),r.data(),64);
  tr=true; double t=0; size_t n=0; const double dt=256.0/44100.0;
  while(t<20.0){ while(n<ev.size()&&ev[n].seconds<=t){ if(!ev[n].bytes.empty()) e.sendMidi(ev[n].bytes.data(),(int)ev[n].bytes.size()); ++n; } e.render(l.data(),r.data(),256); t+=dt; }
  tr=false;
  std::vector<std::pair<uint16_t,uint64_t>> v(entries.begin(),entries.end());
  std::sort(v.begin(),v.end(),[](auto&a,auto&b){return a.second>b.second;});
  uint64_t total=0; for(auto&p:all) total+=p.second;
  std::printf("%04x-%04x: 命令 %zu 個、実行 %llu 回、入口 %zu 個\n",
              lo,hi,all.size(),(unsigned long long)total,v.size());
  std::printf("到達回数の多い入口:\n");
  for(size_t i=0;i<v.size()&&i<20;++i) std::printf("  00:%04x  ×%llu\n",v[i].first,(unsigned long long)v[i].second);
  return 0;
}

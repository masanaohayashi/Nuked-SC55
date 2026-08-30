// sc55callers - ある PC に制御が来る直前の PC を数える。呼び出し元を辿るのに使う。
//
//   sc55callers <pc16> <song.mid>
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
extern void MCU_Write_Impl(mcu_t&,uint32_t,uint8_t);
extern void MCU_Write16_Impl(mcu_t&,uint32_t,uint16_t);
extern void MCU_Step_Impl(mcu_t&);
void MCU_Write16(mcu_t& m,uint32_t a,uint16_t v){ MCU_Write16_Impl(m,a,v); }
uint8_t MCU_Read(mcu_t& m,uint32_t a){ return MCU_Read_Impl(m,a); }
void MCU_Write(mcu_t& m,uint32_t a,uint8_t v){ MCU_Write_Impl(m,a,v); }
static bool tr=false; static uint32_t target=0, prev=0; static std::map<uint32_t,uint64_t> from;
void MCU_Step(mcu_t& m){
  if(tr){ const uint32_t pc=((uint32_t)m.cp<<16)|m.pc;
          if(m.cp==0&&m.pc==target) from[prev]++;
          prev=pc; }
  MCU_Step_Impl(m); }
int main(int argc,char**argv){
  if(argc<3){ std::printf("使い方: sc55callers <pc16> <song.mid>\n"); return 1; }
  target=(uint32_t)std::strtoul(argv[1],nullptr,16);
  NukedSC55Emulator e; if(!e.initialise("/Users/ring2/Documents/Roland SC-55 v1.21",44100.0)) return 1;
  MidiFileData m; std::string err; std::vector<MidiFileEvent> ev;
  if(m.load(argv[2],err)) ev=m.events;
  std::vector<float> l(256),r(256);
  for(int i=0;i<8000;++i) e.render(l.data(),r.data(),64);
  tr=true; double t=0; size_t n=0; const double dt=256.0/44100.0;
  while(t<20.0){ while(n<ev.size()&&ev[n].seconds<=t){ if(!ev[n].bytes.empty()) e.sendMidi(ev[n].bytes.data(),(int)ev[n].bytes.size()); ++n; } e.render(l.data(),r.data(),256); t+=dt; }
  tr=false;
  std::vector<std::pair<uint32_t,uint64_t>> v(from.begin(),from.end());
  std::sort(v.begin(),v.end(),[](auto&a,auto&b){return a.second>b.second;});
  std::printf("00:%04x に来る直前の PC:\n",target);
  for(size_t i=0;i<v.size()&&i<10;++i) std::printf("  %02x:%04x  ×%llu\n",v[i].first>>16,v[i].first&0xffff,(unsigned long long)v[i].second);
  return 0; }

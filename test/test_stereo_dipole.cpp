#include "../Plugins/Source/StereoDipole.h"
#include <cstdlib>
#define CHECK(condition) do { if (!(condition)) std::abort(); } while (false)
#include <iostream>
int main() {
 sc55dipole::Coefficients phone(48000.,8.),tablet(48000.,15.);
 CHECK(phone.reversed[1]!=tablet.reversed[1]);
 for(double spacing:{8.,15.})
 for(double rate:{44100.,48000.,88200.,96000.,192000.}) {
  sc55dipole::Processor whole,split;
  whole.prepare(rate,spacing);split.prepare(rate,spacing);
  std::vector<float> l(8192),r(8192);
  for(size_t i=0;i<l.size();++i){l[i]=.05f*std::sin(i*.19);r[i]=.07f*std::cos(i*.13);}
  auto a=l,b=r;
  whole.process(a.data(),b.data(),0,true);
  whole.process(a.data(),b.data(),int(a.size()),false);
  CHECK(a==l&&b==r);
  whole.process(a.data(),b.data(),int(a.size()),true);
  CHECK(a!=l || b!=r);
  for(int offset=0;offset<8192;) {int n=std::min(8192-offset,(offset%127)+1);split.process(l.data()+offset,r.data()+offset,n,true);offset+=n;}
  for(int i=0;i<8192;++i){CHECK(a[i]==l[i]&&b[i]==r[i]);CHECK(std::isfinite(a[i])&&std::isfinite(b[i]));}
  whole.process(a.data(),b.data(),8192,false);
  auto dryA=a,dryB=b;whole.process(a.data(),b.data(),8192,false);CHECK(a==dryA&&b==dryB);
  whole.prepare(rate,spacing);
  std::fill(a.begin(),a.end(),0.f);std::fill(b.begin(),b.end(),0.f);
  whole.process(a.data(),b.data(),8192,true);
  CHECK(std::all_of(a.begin(),a.end(),[](float v){return v==0.f;}));
  CHECK(std::all_of(b.begin(),b.end(),[](float v){return v==0.f;}));

 }
 std::cout<<"Bypass, transitions, finite output and variable block checks passed\n";
}

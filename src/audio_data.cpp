// SPDX-License-Identifier: MIT
#include "audio_data.h"
#include <algorithm>
#include <cmath>
#include <fstream>

namespace sentinel::audio_detail {
namespace {
uint16_t u16(const std::vector<unsigned char>& b,size_t p) {
    return static_cast<uint16_t>(b[p]|(static_cast<unsigned>(b[p+1])<<8));
}
uint32_t u32(const std::vector<unsigned char>& b,size_t p) {
    return static_cast<uint32_t>(u16(b,p))|(static_cast<uint32_t>(u16(b,p+2))<<16);
}
bool tag(const std::vector<unsigned char>& b,size_t p,const char* t) {
    return p+4<=b.size()&&std::equal(b.begin()+static_cast<std::ptrdiff_t>(p),b.begin()+static_cast<std::ptrdiff_t>(p+4),t);
}
}
Pcm read_wav(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);
    if(!in||in.tellg()<12||in.tellg()>16*1024*1024)return {};
    std::vector<unsigned char> b(static_cast<size_t>(in.tellg()));
    in.seekg(0);in.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(b.size()));
    if(!in||!tag(b,0,"RIFF")||!tag(b,8,"WAVE"))return {};
    const auto riff=u32(b,4);
    if(riff<4||static_cast<uint64_t>(riff)+8>b.size())return {};
    const size_t end=static_cast<size_t>(riff)+8;
    Pcm pcm;bool format=false;size_t data=0,bytes=0;
    for(size_t p=12;p<end;) {
        if(end-p<8)return {};
        const size_t n=u32(b,p+4),body=p+8;
        if(n>end-body)return {};
        if(tag(b,p,"fmt ")&&n>=16) {
            if(u16(b,body)!=1||u16(b,body+14)!=16)return {};
            pcm.channels=u16(b,body+2);pcm.rate=u32(b,body+4);
            if(pcm.channels<1||pcm.channels>2||pcm.rate<8000||pcm.rate>96000||
                u16(b,body+12)!=pcm.channels*2||u32(b,body+8)!=pcm.rate*pcm.channels*2)return {};
            format=true;
        }else if(tag(b,p,"data")){data=body;bytes=n;}
        const auto padded=n+(n&1u);
        if(padded>end-body)return {};
        p=body+padded;
    }
    if(!format||!bytes||bytes%(pcm.channels*2u)||bytes/(pcm.channels*2u)>pcm.rate*30u)return {};
    pcm.samples.reserve(bytes/2);
    for(size_t p=data;p<data+bytes;p+=2) {
        const auto raw=u16(b,p);
        const int sample=raw<32768?static_cast<int>(raw):static_cast<int>(raw)-65536;
        pcm.samples.push_back(static_cast<int16_t>(sample));
    }
    return pcm;
}
Pcm make_tone(const std::string& name) {
    Pcm pcm;std::vector<double> notes={660,880};double length=.14;
    if(name=="critical"){notes={880,660,880,660};length=.15;}
    else if(name=="alert"){notes={880,880};length=.19;}
    else if(name=="question")notes={523.25,783.99};
    else if(name=="default")notes={660};
    constexpr double pi=3.14159265358979323846;
    for(const double hz:notes) {
        const unsigned count=static_cast<unsigned>(pcm.rate*length);
        for(unsigned i=0;i<count;++i) {
            const double t=static_cast<double>(i)/pcm.rate;
            const double envelope=std::min(1.0,t/.015)*std::min(1.0,(length-t)/.045);
            const double wave=std::sin(2*pi*hz*t)*.8+std::sin(2*pi*hz*2*t)*.15;
            pcm.samples.push_back(static_cast<int16_t>(wave*envelope*13500));
        }
        pcm.samples.insert(pcm.samples.end(),pcm.rate/22,0);
    }
    return pcm;
}
void apply_volume(Pcm& pcm,int volume) {
    const double gain=std::clamp(volume,0,100)/100.0;
    for(auto& sample:pcm.samples)sample=static_cast<int16_t>(sample*gain);
}
}

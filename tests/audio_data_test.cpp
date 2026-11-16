// SPDX-License-Identifier: MIT
// File/tone tests use no output device, SAPI voice, or platform audio backend.
#include "audio_data.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
using namespace sentinel::audio_detail;
int checks=0;
void check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
void append16(Bytes& bytes,uint16_t value) {bytes.push_back(static_cast<unsigned char>(value));bytes.push_back(static_cast<unsigned char>(value>>8));}
void append32(Bytes& bytes,uint32_t value) {append16(bytes,static_cast<uint16_t>(value));append16(bytes,static_cast<uint16_t>(value>>16));}
void set16(Bytes& bytes,size_t at,uint16_t value) {bytes.at(at)=static_cast<unsigned char>(value);bytes.at(at+1)=static_cast<unsigned char>(value>>8);}
void set32(Bytes& bytes,size_t at,uint32_t value) {set16(bytes,at,static_cast<uint16_t>(value));set16(bytes,at+2,static_cast<uint16_t>(value>>16));}
void tag(Bytes& bytes,const char* text) {for(int i=0;i<4;++i)bytes.push_back(static_cast<unsigned char>(text[i]));}
void chunk(Bytes& bytes,const char* name,const Bytes& body) {
    tag(bytes,name);append32(bytes,static_cast<uint32_t>(body.size()));bytes.insert(bytes.end(),body.begin(),body.end());
    if(body.size()%2)bytes.push_back(0);
}
Bytes wav(unsigned rate,unsigned channels,const std::vector<int16_t>& samples,bool ancillary=false) {
    Bytes body,format,data;
    tag(body,"WAVE");append16(format,1);append16(format,static_cast<uint16_t>(channels));
    append32(format,rate);append32(format,rate*channels*2);append16(format,static_cast<uint16_t>(channels*2));append16(format,16);
    chunk(body,"fmt ",format);
    if(ancillary)chunk(body,"JUNK",Bytes{1,2,3});
    for(const auto sample:samples)append16(data,static_cast<uint16_t>(sample));
    chunk(body,"data",data);
    Bytes result;tag(result,"RIFF");append32(result,static_cast<uint32_t>(body.size()));
    result.insert(result.end(),body.begin(),body.end());return result;
}
struct Fixture {
    fs::path root;
    unsigned next=0;
    Fixture() {
        const auto serial=std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root=fs::temp_directory_path()/("sentinel-audio-data-test-"+std::to_string(serial));fs::create_directories(root);
    }
    ~Fixture() {std::error_code ec;fs::remove_all(root,ec);}
    fs::path write(const Bytes& bytes) {
        const auto path=root/(std::to_string(++next)+".wav");
        std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        if(!out)throw std::runtime_error("Could not write WAV fixture");return path;
    }
    void rejects(const Bytes& bytes,const char* message) {check(read_wav(write(bytes)).samples.empty(),message);}
};
void valid_pcm() {
    Fixture f;
    const std::vector<int16_t> signed_samples={-32768,-20000,-1,0,1,20000,32767};
    auto pcm=read_wav(f.write(wav(22050,1,signed_samples)));
    check(pcm.rate==22050&&pcm.channels==1,"Mono rate and channel count preserved");
    check(pcm.samples==signed_samples,"Signed PCM endpoints and negative samples decode exactly");
    const std::vector<int16_t> stereo={-32768,32767,-1,1,1234,-5678};
    pcm=read_wav(f.write(wav(48000,2,stereo,true)));
    check(pcm.rate==48000&&pcm.channels==2,"Stereo format preserved with odd ancillary chunk");
    check(pcm.samples==stereo,"Stereo channels stay interleaved and signed");
    for(const unsigned rate:{8000u,96000u})check(read_wav(f.write(wav(rate,1,{1,-1}))).rate==rate,"Supported sample-rate boundary loads");
    auto trailing=wav(22050,1,{1,-1});trailing.insert(trailing.end(),{'o','u','t','s','i','d','e'});
    check(read_wav(f.write(trailing)).samples==std::vector<int16_t>({1,-1}),"Bytes outside RIFF cannot alter declared PCM");
    const std::vector<int16_t> thirty_seconds(8000*30,42);
    check(read_wav(f.write(wav(8000,1,thirty_seconds))).samples.size()==thirty_seconds.size(),"Thirty-second boundary is accepted");
}
void malformed_pcm() {
    Fixture f;const auto valid=wav(22050,1,{-32768,32767});
    f.rejects({},"Empty file is not PCM");f.rejects(Bytes{'R','I','F','F'},"Short header is not PCM");
    check(read_wav(f.root/"not-present.wav").samples.empty(),"Missing sound file is rejected");
    auto changed=valid;changed[0]='X';f.rejects(changed,"RIFF signature is required");
    changed=valid;changed[8]='X';f.rejects(changed,"WAVE form type is required");
    changed=valid;set32(changed,4,3);f.rejects(changed,"RIFF size cannot omit the form type");
    changed=valid;set32(changed,4,static_cast<uint32_t>(changed.size()));f.rejects(changed,"RIFF cannot extend past file end");
    changed=valid;set32(changed,4,static_cast<uint32_t>(changed.size()-10));f.rejects(changed,"PCM cannot extend past the declared RIFF boundary");
    changed=valid;changed.pop_back();f.rejects(changed,"Truncated PCM payload is rejected");
    changed=valid;set32(changed,40,0xffffffffu);f.rejects(changed,"Unknown or unbounded streaming data length is rejected");
    changed=valid;set32(changed,40,100);f.rejects(changed,"Chunk payload cannot overrun the RIFF container");
    changed=valid;set16(changed,20,3);f.rejects(changed,"Floating-point format is not treated as signed PCM");
    changed=valid;set16(changed,34,8);f.rejects(changed,"Eight-bit samples are rejected");
    changed=valid;set16(changed,34,24);f.rejects(changed,"Twenty-four-bit samples are rejected");
    changed=valid;set16(changed,22,0);f.rejects(changed,"Zero channels are rejected");
    changed=valid;set16(changed,22,3);f.rejects(changed,"Unsupported channel count is rejected");
    changed=valid;set16(changed,32,4);f.rejects(changed,"Incorrect PCM block alignment is rejected");
    changed=valid;set32(changed,28,1);f.rejects(changed,"Incorrect byte rate is rejected");
    for(const unsigned rate:{7999u,96001u})f.rejects(wav(rate,1,{1,-1}),"Unsupported sample rate is rejected");
    f.rejects(wav(22050,1,{}),"Empty data chunk is rejected");
    f.rejects(wav(22050,2,{1,2,3}),"Partial stereo frame is rejected");
    changed=valid;changed.push_back('J');changed.push_back('U');changed.push_back('N');
    set32(changed,4,static_cast<uint32_t>(changed.size()-8));f.rejects(changed,"Partial trailing chunk header is rejected");
    changed=valid;tag(changed,"JUNK");append32(changed,1);changed.push_back(42);
    set32(changed,4,static_cast<uint32_t>(changed.size()-8));f.rejects(changed,"Missing odd-chunk padding is rejected");
    const std::vector<int16_t> overlong(8000*30+1,42);
    f.rejects(wav(8000,1,overlong),"PCM longer than thirty seconds is rejected");
    const auto oversized=f.root/"oversized.wav";
    {std::ofstream out(oversized,std::ios::binary);out.seekp(16*1024*1024);out.put('x');}
    check(read_wav(oversized).samples.empty(),"Files larger than sixteen MiB are rejected before decoding");
}
void generated_tones_and_volume() {
    std::vector<std::vector<int16_t>> generated;
    for(const auto* name:{"chime","alert","critical","question","default"}) {
        const auto pcm=make_tone(name);
        check(pcm.rate==22050&&pcm.channels==1,"Built-in tone has portable mono PCM format");
        check(!pcm.samples.empty()&&pcm.samples.size()<pcm.rate*2,"Built-in tone length is nonempty and bounded");
        check(std::any_of(pcm.samples.begin(),pcm.samples.end(),[](auto sample){return sample!=0;}),"Built-in tone contains audible samples");
        check(pcm.samples.front()==0&&pcm.samples.back()==0,"Built-in tone starts and ends at silence");
        generated.push_back(pcm.samples);
    }
    for(size_t i=0;i<generated.size();++i)for(size_t j=i+1;j<generated.size();++j)
        check(generated[i]!=generated[j],"Built-in tone choices have distinct waveforms");
    check(make_tone("unknown-tone").samples==make_tone("chime").samples,"Unknown built-in tone falls back to the chime");
    Pcm pcm;pcm.samples={-32768,-1,0,1,32767};
    const auto original=pcm.samples;apply_volume(pcm,100);check(pcm.samples==original,"Full volume preserves signed samples");
    apply_volume(pcm,500);check(pcm.samples==original,"Volume above one hundred is clamped without overflow");
    apply_volume(pcm,50);check(pcm.samples==std::vector<int16_t>({-16384,0,0,0,16383}),"Half volume scales signed endpoints without wrapping");
    apply_volume(pcm,-1);check(std::all_of(pcm.samples.begin(),pcm.samples.end(),[](auto sample){return sample==0;}),"Negative volume clamps to silence");
    pcm.samples=original;apply_volume(pcm,0);check(std::all_of(pcm.samples.begin(),pcm.samples.end(),[](auto sample){return sample==0;}),"Zero volume mutes every sample");
}
}
int main() {
    try {valid_pcm();malformed_pcm();generated_tones_and_volume();std::cout<<"Audio data checks passed: "<<checks<<'\n';return 0;}
    catch(const std::exception& error){std::cerr<<"Audio data test failed after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}
}

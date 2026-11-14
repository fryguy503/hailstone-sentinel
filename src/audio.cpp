// SPDX-License-Identifier: MIT
#include "audio.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <sapi.h>
#endif

namespace sentinel {
namespace {
#ifdef _WIN32
struct Pcm { unsigned rate = 22050, channels = 1; std::vector<int16_t> samples; };
uint16_t u16(const std::vector<unsigned char>& b, size_t p) {
    return static_cast<uint16_t>(b[p] | (static_cast<unsigned>(b[p + 1]) << 8));
}
uint32_t u32(const std::vector<unsigned char>& b, size_t p) {
    return static_cast<uint32_t>(u16(b,p)) | (static_cast<uint32_t>(u16(b,p+2)) << 16);
}
bool tag(const std::vector<unsigned char>& b, size_t p, const char* t) {
    return p + 4 <= b.size() && std::equal(b.begin()+static_cast<std::ptrdiff_t>(p),b.begin()+static_cast<std::ptrdiff_t>(p+4),t);
}
Pcm wav(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() < 12 || in.tellg() > 16*1024*1024) return {};
    std::vector<unsigned char> b(static_cast<size_t>(in.tellg()));
    in.seekg(0); in.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(b.size()));
    if (!in || !tag(b,0,"RIFF") || !tag(b,8,"WAVE")) return {};
    Pcm pcm; bool format = false; size_t data = 0, bytes = 0;
    for (size_t p=12;p+8<=b.size();) {
        const size_t n=u32(b,p+4), body=p+8;
        if (n>b.size()-body) return {};
        if (tag(b,p,"fmt ") && n>=16) {
            if (u16(b,body)!=1 || u16(b,body+14)!=16) return {};
            pcm.channels=u16(b,body+2); pcm.rate=u32(b,body+4);
            if (pcm.channels<1 || pcm.channels>2 || pcm.rate<8000 || pcm.rate>96000) return {};
            format=true;
        } else if (tag(b,p,"data")) { data=body; bytes=n; }
        p=body+n+(n&1u);
    }
    if (!format || !bytes || bytes% (pcm.channels*2u) || bytes/(pcm.channels*2u)>pcm.rate*30u) return {};
    pcm.samples.reserve(bytes/2);
    for (size_t p=data;p<data+bytes;p+=2) pcm.samples.push_back(static_cast<int16_t>(u16(b,p)));
    return pcm;
}
Pcm tone(const std::string& name) {
    Pcm pcm;
    std::vector<double> notes = {660,880}; double length=.14;
    if(name=="critical") {notes={880,660,880,660}; length=.15;}
    else if(name=="alert") {notes={880,880}; length=.19;}
    else if(name=="question") notes={523.25,783.99};
    else if(name=="default") notes={660};
    constexpr double pi=3.14159265358979323846;
    for(double hz:notes) {
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
std::wstring wide(const std::string& s) {
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.c_str(),-1,nullptr,0);
    if(count<=0)return {};
    std::wstring out(static_cast<size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,out.data(),count); out.pop_back(); return out;
}
HRESULT voices(IEnumSpObjectTokens** out) {
    ISpObjectTokenCategory* category=nullptr;
    auto result=CoCreateInstance(CLSID_SpObjectTokenCategory,nullptr,CLSCTX_INPROC_SERVER,
        IID_ISpObjectTokenCategory,reinterpret_cast<void**>(&category));
    if(FAILED(result))return result;
    result=category->SetId(SPCAT_VOICES,FALSE);
    if(SUCCEEDED(result))result=category->EnumTokens(nullptr,nullptr,out);
    category->Release();return result;
}
#endif
}
Audio::Audio() : worker_([this]{ run(); }) {}
Audio::~Audio() { stopping_=true; cancel(); ready_.notify_all(); if(worker_.joinable())worker_.join(); }
bool Audio::sound_available() const {return sound_;}
bool Audio::speech_available() const {return speech_;}
bool Audio::safe_filename(const std::string& name) {
    if(name.empty() || name.size()>200 || name=="." || name=="..")return false;
    if(name.find_first_of("/\\:\r\n")!=std::string::npos || name.front()=='.') return false;
    return std::all_of(name.begin(),name.end(),[](unsigned char c){return c>=32 && c!=127;});
}
std::vector<std::string> Audio::files(const std::filesystem::path& folder) {
    std::vector<std::string> out; std::error_code ec;
    for(const auto& entry:std::filesystem::directory_iterator(folder,ec)) {
        if(out.size()>=128)break;
        const auto encoded=entry.path().filename().u8string();
        const std::string name(encoded.begin(),encoded.end());auto ext=entry.path().extension().string();
        std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        if(entry.is_regular_file(ec) && !entry.is_symlink(ec) && ext==".wav" && safe_filename(name))out.push_back(name);
    }
    std::sort(out.begin(),out.end());return out;
}
void Audio::enqueue(const Fired& f,const std::filesystem::path& sounds) {
    if(!f.alert.value("sound",false)&&!f.alert.value("speech",false))return;
    std::lock_guard<std::mutex> lock(mutex_);
    if(queue_.size()>=8)queue_.pop_front();
    queue_.push_back({f,sounds,generation_.load()});ready_.notify_one();
}
void Audio::cancel() {++generation_;std::lock_guard<std::mutex> lock(mutex_);queue_.clear();ready_.notify_all();}
void Audio::run() {
#ifdef _WIN32
    const HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    ISpVoice* voice=nullptr;
    if(SUCCEEDED(co))CoCreateInstance(CLSID_SpVoice,nullptr,CLSCTX_INPROC_SERVER,IID_ISpVoice,reinterpret_cast<void**>(&voice));
    sound_=waveOutGetNumDevs()>0;speech_=voice!=nullptr;
#endif
    while(!stopping_) {
        Request request;
        {std::unique_lock<std::mutex> lock(mutex_);ready_.wait(lock,[this]{return stopping_||!queue_.empty();});
         if(stopping_)break;
         request=std::move(queue_.front());queue_.pop_front();}
        try {
#ifdef _WIN32
            const auto valid=[&]{return !stopping_ && request.generation==generation_.load();};
            const auto& a=request.fired.alert;
            if(a.value("sound",false)&&valid()) {
                Pcm pcm;const auto file=a.value("sound_file",std::string{});
                if(safe_filename(file)) {
                    std::error_code ec; const auto path=request.sounds/std::filesystem::path(std::u8string(file.begin(),file.end()));
                    if(!std::filesystem::is_symlink(path,ec))pcm=wav(path);
                }
                if(pcm.samples.empty())pcm=tone(a.value("tone",std::string("chime")));
                const double volume=std::clamp(a.value("volume",85),0,100)/100.0;
                for(auto& sample:pcm.samples)sample=static_cast<int16_t>(sample*volume);
                WAVEFORMATEX fmt{};fmt.wFormatTag=WAVE_FORMAT_PCM;fmt.nChannels=static_cast<WORD>(pcm.channels);
                fmt.nSamplesPerSec=pcm.rate;fmt.wBitsPerSample=16;fmt.nBlockAlign=static_cast<WORD>(pcm.channels*2);
                fmt.nAvgBytesPerSec=fmt.nSamplesPerSec*fmt.nBlockAlign;
                HWAVEOUT device=nullptr;
                if(waveOutOpen(&device,WAVE_MAPPER,&fmt,0,0,CALLBACK_NULL)==MMSYSERR_NOERROR) {
                    WAVEHDR header{};header.lpData=reinterpret_cast<LPSTR>(pcm.samples.data());
                    header.dwBufferLength=static_cast<DWORD>(pcm.samples.size()*2);
                    if(waveOutPrepareHeader(device,&header,sizeof header)==MMSYSERR_NOERROR) {
                        if(waveOutWrite(device,&header,sizeof header)==MMSYSERR_NOERROR)
                            while(valid()&&!(header.dwFlags&WHDR_DONE))std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        waveOutReset(device);waveOutUnprepareHeader(device,&header,sizeof header);
                    }
                    waveOutClose(device);
                }
            }
            if(a.value("speech",false)&&voice&&valid()) {
                voice->SetRate(std::clamp(a.value("rate",0),-10,10));
                voice->SetVolume(static_cast<USHORT>(std::clamp(a.value("volume",85),0,100)));
                IEnumSpObjectTokens* tokens=nullptr;
                if(SUCCEEDED(voices(&tokens))) {
                    ULONG count=0; tokens->GetCount(&count);ISpObjectToken* token=nullptr;
                    const ULONG index=count?static_cast<ULONG>(std::clamp(a.value("voice",0),0,1))%count:0;
                    if(count&&SUCCEEDED(tokens->Item(index,&token))){voice->SetVoice(token);token->Release();}
                    tokens->Release();
                }
                const auto text=wide(request.fired.speech.empty()?request.fired.message:request.fired.speech);
                voice->Speak(text.c_str(),SPF_ASYNC|SPF_IS_NOT_XML|SPF_PURGEBEFORESPEAK,nullptr);
                while(valid()&&voice->WaitUntilDone(20)==S_FALSE){}
                if(!valid())voice->Speak(nullptr,SPF_PURGEBEFORESPEAK,nullptr);
            }
#endif
        }catch(...) { /* A missing device or malformed WAV cannot stop observation. */ }
    }
#ifdef _WIN32
    if(voice){voice->Speak(nullptr,SPF_PURGEBEFORESPEAK,nullptr);voice->Release();}
    if(SUCCEEDED(co))CoUninitialize();
#endif
}
}

// SPDX-License-Identifier: MIT
#include "audio.h"
#include "audio_data.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <sapi.h>
#else
#include "miniaudio_config.h"
#include "miniaudio.h"
#ifdef __APPLE__
#include "speech_macos.h"
#else
#include "speech_linux.h"
#endif
#endif

namespace sentinel {
namespace {
using audio_detail::Pcm;
auto playback_deadline(const Pcm& pcm) {
    const auto milliseconds=static_cast<int64_t>(pcm.samples.size())*1000/(pcm.rate*pcm.channels);
    return std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds+2000);
}
#ifdef _WIN32
std::wstring wide(const std::string& s) {
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.c_str(),-1,nullptr,0);
    if(count<=0)return {};
    std::wstring out(static_cast<size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,out.data(),count);out.pop_back();return out;
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
bool play_pcm(Pcm& pcm,const std::function<bool()>& valid) {
    WAVEFORMATEX fmt{};fmt.wFormatTag=WAVE_FORMAT_PCM;fmt.nChannels=static_cast<WORD>(pcm.channels);
    fmt.nSamplesPerSec=pcm.rate;fmt.wBitsPerSample=16;fmt.nBlockAlign=static_cast<WORD>(pcm.channels*2);
    fmt.nAvgBytesPerSec=fmt.nSamplesPerSec*fmt.nBlockAlign;
    HWAVEOUT device=nullptr;
    if(waveOutOpen(&device,WAVE_MAPPER,&fmt,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR)return false;
    WAVEHDR header{};header.lpData=reinterpret_cast<LPSTR>(pcm.samples.data());
    header.dwBufferLength=static_cast<DWORD>(pcm.samples.size()*2);
    bool played=false;
    if(waveOutPrepareHeader(device,&header,sizeof header)==MMSYSERR_NOERROR) {
        if(waveOutWrite(device,&header,sizeof header)==MMSYSERR_NOERROR) {
            played=true;
            const auto deadline=playback_deadline(pcm);
            while(valid()&&!(header.dwFlags&WHDR_DONE)&&std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if(valid()&&!(header.dwFlags&WHDR_DONE))played=false;
        }
        waveOutReset(device);waveOutUnprepareHeader(device,&header,sizeof header);
    }
    waveOutClose(device);return played;
}
#else
struct Playback {
    const Pcm* pcm=nullptr;
    size_t cursor=0,drain=0;
    std::atomic<bool> done{false};
};
void pcm_callback(ma_device* device,void* output,const void*,ma_uint32 frames) {
    auto& state=*static_cast<Playback*>(device->pUserData);
    auto* samples=static_cast<int16_t*>(output);
    const size_t size=static_cast<size_t>(frames)*state.pcm->channels;
    const auto count=std::min(size,state.pcm->samples.size()-state.cursor);
    if(count)std::copy_n(state.pcm->samples.data()+state.cursor,count,samples);
    std::fill(samples+count,samples+size,0);
    state.cursor+=count;
    // Keep supplying silence briefly so the backend drains the final sound block.
    if(count==0)state.drain+=frames;
    if(state.drain>=state.pcm->rate/5)state.done=true;
}
bool portable_pcm(Pcm& pcm,const std::function<bool()>& valid,bool start) {
    Playback playback;playback.pcm=&pcm;
    auto config=ma_device_config_init(ma_device_type_playback);
    config.playback.format=ma_format_s16;config.playback.channels=pcm.channels;
    config.sampleRate=pcm.rate;config.dataCallback=pcm_callback;config.pUserData=&playback;
    config.periodSizeInMilliseconds=10;
    ma_device device{};
    if(ma_device_init(nullptr,&config,&device)!=MA_SUCCESS)return false;
    bool result=true;
    if(start) {
        result=valid()&&ma_device_start(&device)==MA_SUCCESS;
        const auto deadline=playback_deadline(pcm);
        while(result&&valid()&&!playback.done.load()&&std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if(valid()&&!playback.done.load())result=false;
    }
    // Joins backend callbacks before the PCM, worker, or plugin can be destroyed.
    ma_device_uninit(&device);return result;
}
bool play_pcm(Pcm& pcm,const std::function<bool()>& valid) {return portable_pcm(pcm,valid,true);}
#endif
}
Audio::Audio() : worker_([this]{try{run();}catch(...){sound_=false;speech_=false;}}) {}
Audio::~Audio() {stopping_=true;cancel();ready_.notify_all();if(worker_.joinable())worker_.join();}
bool Audio::sound_available() const {return sound_;}
bool Audio::speech_available() const {return speech_;}
bool Audio::safe_filename(const std::string& name) {
    if(name.empty()||name.size()>200||name=="."||name=="..")return false;
    if(name.find_first_of("/\\:\r\n")!=std::string::npos||name.front()=='.')return false;
    return std::all_of(name.begin(),name.end(),[](unsigned char c){return c>=32&&c!=127;});
}
std::vector<std::string> Audio::files(const std::filesystem::path& folder) {
    std::vector<std::string> out;std::error_code ec;
    for(const auto& entry:std::filesystem::directory_iterator(folder,ec)) {
        if(out.size()>=128)break;
        const auto encoded=entry.path().filename().u8string();
        const std::string name(encoded.begin(),encoded.end());auto ext=entry.path().extension().string();
        std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        if(entry.is_regular_file(ec)&&!entry.is_symlink(ec)&&ext==".wav"&&safe_filename(name))out.push_back(name);
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
#else
    Pcm probe;sound_=portable_pcm(probe,[]{return true;},false);
#ifdef __APPLE__
    speech_=macos_speech_available();
#else
    auto speech_program=linux_speech_program();speech_=!speech_program.empty();
#endif
#endif
    while(!stopping_) {
        Request request;
        {std::unique_lock<std::mutex> lock(mutex_);ready_.wait(lock,[this]{return stopping_||!queue_.empty();});
         if(stopping_)break;
         request=std::move(queue_.front());queue_.pop_front();}
        try {
            const auto valid=[&]{return !stopping_&&request.generation==generation_.load();};
            const auto& a=request.fired.alert;
            if(a.value("sound",false)&&valid()) {
                Pcm pcm;const auto file=a.value("sound_file",std::string{});
                if(safe_filename(file)) {
                    std::error_code ec;const auto path=request.sounds/std::filesystem::path(std::u8string(file.begin(),file.end()));
                    if(!std::filesystem::is_symlink(path,ec))pcm=audio_detail::read_wav(path);
                }
                if(pcm.samples.empty())pcm=audio_detail::make_tone(a.value("tone",std::string("chime")));
                audio_detail::apply_volume(pcm,a.value("volume",85));
                const bool result=play_pcm(pcm,valid);if(valid())sound_=result;
            }
            if(a.value("speech",false)&&valid()) {
#ifdef _WIN32
                if(voice) {
                    voice->SetRate(std::clamp(a.value("rate",0),-10,10));
                    voice->SetVolume(static_cast<USHORT>(std::clamp(a.value("volume",85),0,100)));
                    IEnumSpObjectTokens* tokens=nullptr;
                    if(SUCCEEDED(voices(&tokens))) {
                        ULONG count=0;tokens->GetCount(&count);ISpObjectToken* token=nullptr;
                        const ULONG index=count?static_cast<ULONG>(std::clamp(a.value("voice",0),0,1))%count:0;
                        if(count&&SUCCEEDED(tokens->Item(index,&token))){voice->SetVoice(token);token->Release();}
                        tokens->Release();
                    }
                    const auto text=wide(request.fired.speech.empty()?request.fired.message:request.fired.speech);
                    voice->Speak(text.c_str(),SPF_ASYNC|SPF_IS_NOT_XML|SPF_PURGEBEFORESPEAK,nullptr);
                    while(valid()&&voice->WaitUntilDone(20)==S_FALSE){}
                    if(!valid())voice->Speak(nullptr,SPF_PURGEBEFORESPEAK,nullptr);
                }
#elif defined(__APPLE__)
                speech_=macos_speech_available();
                macos_speak(request.fired.speech.empty()?request.fired.message:request.fired.speech,
                    a.value("voice",0),a.value("rate",0),a.value("volume",85),valid);
#else
                if(speech_program.empty())speech_program=linux_speech_program();
                const bool result=linux_speak(speech_program,request.fired.speech.empty()?request.fired.message:request.fired.speech,
                    a.value("voice",0),a.value("rate",0),a.value("volume",85),valid);
                if(valid())speech_=result;
#endif
            }
        }catch(...) { /* A device or voice failure cannot stop observation. */ }
    }
#ifdef _WIN32
    if(voice){voice->Speak(nullptr,SPF_PURGEBEFORESPEAK,nullptr);voice->Release();}
    if(SUCCEEDED(co))CoUninitialize();
#endif
}
}

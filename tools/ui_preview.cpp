// SPDX-License-Identifier: MIT
// Development-only preview: runs the shipped Lua UI through Dear ImGui's actual
// layout and draw lists, then rasterizes those lists without a GPU or game assets.
// This is a fixture, not a substitute for testing inside the Hailstone client.
#include "imgui.h"
#include "imgui_internal.h"
#include "lua.hpp"
#include "engine.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
std::vector<std::pair<std::string,int>> windows;
std::string click;
float preview_ui_scale=1;
ImVec2 forced_window_size{};
bool drawing_main_window=false;
std::string environment(const char* name) {
#ifdef _WIN32
  char* value=nullptr;size_t length=0;
  if(_dupenv_s(&value,&length,name)!=0)return {};
  std::string result=value?value:"";std::free(value);return result;
#else
  const char* value=std::getenv(name);return value?value:"";
#endif
}
const char* S(lua_State* l,int i) {return luaL_checkstring(l,i);}
int I(lua_State* l,int i,int d=0) {return static_cast<int>(luaL_optinteger(l,i,d));}
float F(lua_State* l,int i,float d=0) {return static_cast<float>(luaL_optnumber(l,i,d));}
bool B(lua_State* l,int i,bool d=false) {return lua_isnoneornil(l,i)?d:lua_toboolean(l,i)!=0;}
ImVec4 color(lua_State* l,int i) {return {F(l,i,1),F(l,i+1,1),F(l,i+2,1),F(l,i+3,1)};}
int ret(lua_State* l,bool b) {lua_pushboolean(l,b);return 1;}
bool forced(const char* label) {if(click==label){click.clear();return true;}return false;}
int window(lua_State* l) {std::string name=S(l,1);luaL_checktype(l,2,LUA_TFUNCTION);lua_pushvalue(l,2);windows.emplace_back(name,luaL_ref(l,LUA_REGISTRYINDEX));return 0;}
int begin(lua_State* l) {bool open=B(l,2,true);auto& style=ImGui::GetStyle();const auto menu=style.WindowMenuButtonPosition;if(B(l,4))style.WindowMenuButtonPosition=ImGuiDir_Right;if(drawing_main_window&&forced_window_size.x>0)ImGui::SetNextWindowSize(forced_window_size,ImGuiCond_Always);bool v=ImGui::Begin(S(l,1),lua_isnoneornil(l,2)?nullptr:&open,I(l,3));style.WindowMenuButtonPosition=menu;lua_pushboolean(l,v);lua_pushboolean(l,open);return 2;}
int end(lua_State*) {ImGui::End();return 0;}
int child(lua_State* l) {return ret(l,ImGui::BeginChild(S(l,1),{F(l,2),F(l,3)},B(l,4)?ImGuiChildFlags_Borders:0,I(l,5)));}
int endchild(lua_State*) {ImGui::EndChild();return 0;}
int text(lua_State* l) {ImGui::TextUnformatted(S(l,1));return 0;}
int wrapped(lua_State* l) {ImGui::TextWrapped("%s",S(l,1));return 0;}
int colored(lua_State* l) {ImGui::TextColored(color(l,1),"%s",S(l,5));return 0;}
int disabled(lua_State* l) {ImGui::TextDisabled("%s",S(l,1));return 0;}
int label(lua_State* l) {ImGui::LabelText(S(l,1),"%s",S(l,2));return 0;}
int bullet(lua_State* l) {ImGui::BulletText("%s",S(l,1));return 0;}
int button(lua_State* l) {bool hit=ImGui::Button(S(l,1),{F(l,2),F(l,3)});return ret(l,forced(S(l,1))||hit);}
int smallbutton(lua_State* l) {bool hit=ImGui::SmallButton(S(l,1));return ret(l,forced(S(l,1))||hit);}
int selectable(lua_State* l) {bool hit=ImGui::Selectable(S(l,1),B(l,2),I(l,3),{F(l,4),F(l,5)});return ret(l,forced(S(l,1))||hit);}
int checkbox(lua_State* l) {bool b=B(l,2),changed=ImGui::Checkbox(S(l,1),&b);if(forced(S(l,1))){b=!b;changed=true;}lua_pushboolean(l,b);lua_pushboolean(l,changed);return 2;}
int sliderint(lua_State* l) {int v=I(l,2);bool c=ImGui::SliderInt(S(l,1),&v,I(l,3),I(l,4));lua_pushinteger(l,v);lua_pushboolean(l,c);return 2;}
int sliderfloat(lua_State* l) {float v=F(l,2);bool c=ImGui::SliderFloat(S(l,1),&v,F(l,3),F(l,4));lua_pushnumber(l,v);lua_pushboolean(l,c);return 2;}
int input(lua_State* l) {std::vector<char>b(std::max(32,I(l,3,256)));std::snprintf(b.data(),b.size(),"%s",S(l,2));bool c=ImGui::InputText(S(l,1),b.data(),b.size(),I(l,4));lua_pushstring(l,b.data());lua_pushboolean(l,c);return 2;}
int inputhint(lua_State* l) {std::vector<char>b(std::max(32,I(l,4,256)));std::snprintf(b.data(),b.size(),"%s",S(l,3));bool c=ImGui::InputTextWithHint(S(l,1),S(l,2),b.data(),b.size(),I(l,5));lua_pushstring(l,b.data());lua_pushboolean(l,c);return 2;}
int multiline(lua_State* l) {std::vector<char>b(std::max(32,I(l,3,4096)));std::snprintf(b.data(),b.size(),"%s",S(l,2));bool c=ImGui::InputTextMultiline(S(l,1),b.data(),b.size(),{F(l,4),F(l,5)},I(l,6));lua_pushstring(l,b.data());lua_pushboolean(l,c);return 2;}
int combo(lua_State* l) {return ret(l,ImGui::BeginCombo(S(l,1),S(l,2),I(l,3)));}
int endcombo(lua_State*) {ImGui::EndCombo();return 0;}
int collapse(lua_State* l) {return ret(l,ImGui::CollapsingHeader(S(l,1),I(l,2)));}
int pushcolor(lua_State* l) {ImGui::PushStyleColor(I(l,1),color(l,2));return 0;}
int popcolor(lua_State* l) {ImGui::PopStyleColor(I(l,1,1));return 0;}
int pushvar(lua_State* l) {if(lua_isnoneornil(l,3))ImGui::PushStyleVar(I(l,1),F(l,2));else ImGui::PushStyleVar(I(l,1),ImVec2(F(l,2),F(l,3)));return 0;}
int popvar(lua_State* l) {ImGui::PopStyleVar(I(l,1,1));return 0;}
int nextsize(lua_State* l) {ImGui::SetNextWindowSize({F(l,1),F(l,2)},I(l,3,ImGuiCond_FirstUseEver));return 0;}
int nextpos(lua_State* l) {ImGui::SetNextWindowPos({F(l,1),F(l,2)},I(l,3,ImGuiCond_FirstUseEver));return 0;}
int nextalpha(lua_State* l) {ImGui::SetNextWindowBgAlpha(F(l,1));return 0;}
int fontscale(lua_State* l) {ImGui::SetWindowFontScale(F(l,1,1));return 0;}
int content(lua_State* l) {auto v=ImGui::GetContentRegionAvail();lua_pushnumber(l,v.x);lua_pushnumber(l,v.y);return 2;}
int cursor(lua_State* l) {auto v=ImGui::GetCursorScreenPos();lua_pushnumber(l,v.x);lua_pushnumber(l,v.y);return 2;}
int pos(lua_State* l) {auto v=ImGui::GetWindowPos();lua_pushnumber(l,v.x);lua_pushnumber(l,v.y);return 2;}
int size(lua_State* l) {auto v=ImGui::GetWindowSize();lua_pushnumber(l,v.x);lua_pushnumber(l,v.y);return 2;}
int setcursor(lua_State* l) {ImGui::SetCursorScreenPos({F(l,1),F(l,2)});return 0;}
int nextwidth(lua_State* l) {ImGui::SetNextItemWidth(F(l,1));return 0;}
int sameline(lua_State* l) {ImGui::SameLine(F(l,1),F(l,2,-1));return 0;}
int dummy(lua_State* l) {ImGui::Dummy({F(l,1),F(l,2)});return 0;}
int separator(lua_State*) {ImGui::Separator();return 0;}
int pushid(lua_State* l) {ImGui::PushID(S(l,1));return 0;}
int popid(lua_State*) {ImGui::PopID();return 0;}
int pushwrap(lua_State* l) {ImGui::PushTextWrapPos(F(l,1));return 0;}
int popwrap(lua_State*) {ImGui::PopTextWrapPos();return 0;}
int hover(lua_State* l) {return ret(l,ImGui::IsItemHovered());}
int windowhover(lua_State* l) {return ret(l,ImGui::IsWindowHovered());}
int tooltip(lua_State* l) {ImGui::SetTooltip("%s",S(l,1));return 0;}
int mousedown(lua_State* l) {return ret(l,ImGui::IsMouseDown(I(l,1)));}
int mousereleased(lua_State* l) {return ret(l,ImGui::IsMouseReleased(I(l,1)));}
int io(lua_State* l) {lua_newtable(l);lua_pushnumber(l,ImGui::GetIO().DisplaySize.x);lua_setfield(l,-2,"width");lua_pushnumber(l,ImGui::GetIO().DisplaySize.y);lua_setfield(l,-2,"height");lua_pushnumber(l,preview_ui_scale);lua_setfield(l,-2,"uiScale");return 1;}
int framecount(lua_State* l) {lua_pushinteger(l,ImGui::GetFrameCount());return 1;}
int time(lua_State* l) {lua_pushnumber(l,ImGui::GetTime());return 1;}
int calcsize(lua_State* l) {const auto v=ImGui::CalcTextSize(S(l,1));lua_pushnumber(l,v.x);lua_pushnumber(l,v.y);return 2;}
int drawrect(lua_State* l) {ImGui::GetWindowDrawList()->AddRectFilled({F(l,1),F(l,2)},{F(l,3),F(l,4)},ImGui::GetColorU32(color(l,5)),F(l,9));return 0;}
int gradient(lua_State* l) {auto a=ImGui::GetColorU32(color(l,5)),b=ImGui::GetColorU32(color(l,9));ImGui::GetWindowDrawList()->AddRectFilledMultiColor({F(l,1),F(l,2)},{F(l,3),F(l,4)},a,a,b,b);return 0;}
int drawline(lua_State* l) {ImGui::GetWindowDrawList()->AddLine({F(l,1),F(l,2)},{F(l,3),F(l,4)},ImGui::GetColorU32(color(l,5)),F(l,9,1));return 0;}
int circle(lua_State* l) {ImGui::GetWindowDrawList()->AddCircle({F(l,1),F(l,2)},F(l,3),ImGui::GetColorU32(color(l,4)),0,F(l,8,1));return 0;}
int circfill(lua_State* l) {ImGui::GetWindowDrawList()->AddCircleFilled({F(l,1),F(l,2)},F(l,3),ImGui::GetColorU32(color(l,4)));return 0;}
void constants(lua_State* l,const char* key,std::initializer_list<std::pair<const char*,int>> vals) {lua_newtable(l);for(auto [n,v]:vals){lua_pushinteger(l,v);lua_setfield(l,-2,n);}lua_setfield(l,-2,key);}
void bindings(lua_State* l) {
  const luaL_Reg fns[]={{"window",window},{"Begin",begin},{"End",end},{"BeginChild",child},{"EndChild",endchild},
  {"Text",text},{"TextWrapped",wrapped},{"TextColored",colored},{"TextDisabled",disabled},{"LabelText",label},{"BulletText",bullet},
  {"Button",button},{"SmallButton",smallbutton},{"Selectable",selectable},{"Checkbox",checkbox},{"SliderInt",sliderint},{"SliderFloat",sliderfloat},
  {"InputText",input},{"InputTextWithHint",inputhint},{"InputTextMultiline",multiline},{"BeginCombo",combo},{"EndCombo",endcombo},{"CollapsingHeader",collapse},
  {"PushStyleColor",pushcolor},{"PopStyleColor",popcolor},{"PushStyleVar",pushvar},{"PopStyleVar",popvar},
  {"SetNextWindowSize",nextsize},{"SetNextWindowPos",nextpos},{"SetNextWindowBgAlpha",nextalpha},{"SetWindowFontScale",fontscale},
  {"GetContentRegionAvail",content},{"GetCursorScreenPos",cursor},{"GetWindowPos",pos},{"GetWindowSize",size},{"SetCursorScreenPos",setcursor},{"SetNextItemWidth",nextwidth},
  {"SameLine",sameline},{"Dummy",dummy},{"Separator",separator},{"PushID",pushid},{"PopID",popid},{"PushTextWrapPos",pushwrap},{"PopTextWrapPos",popwrap},
  {"IsItemHovered",hover},{"IsWindowHovered",windowhover},{"SetTooltip",tooltip},{"IsMouseDown",mousedown},{"IsMouseReleased",mousereleased},{"GetIO",io},{"GetFrameCount",framecount},{"GetTime",time},{"CalcTextSize",calcsize},
  {"DrawRectFilled",drawrect},{"DrawRectGradient",gradient},{"DrawLine",drawline},{"DrawCircle",circle},{"DrawCircleFilled",circfill},{nullptr,nullptr}};
  luaL_newlib(l,fns);
#define COL(n) {#n,ImGuiCol_##n}
  constants(l,"Col",{COL(Text),COL(TextDisabled),COL(WindowBg),COL(ChildBg),COL(PopupBg),COL(Border),COL(BorderShadow),
    COL(FrameBg),COL(FrameBgHovered),COL(FrameBgActive),COL(Button),COL(ButtonHovered),COL(ButtonActive),COL(Header),COL(HeaderHovered),COL(HeaderActive),
    COL(CheckMark),COL(SliderGrab),COL(SliderGrabActive),COL(Separator),COL(SeparatorHovered),COL(SeparatorActive),COL(TitleBg),COL(TitleBgActive),COL(TitleBgCollapsed),
    COL(MenuBarBg),COL(ScrollbarBg),COL(ScrollbarGrab),COL(ScrollbarGrabHovered),COL(ScrollbarGrabActive),COL(ResizeGrip),COL(ResizeGripHovered),COL(ResizeGripActive),
    COL(Tab),COL(TabHovered),COL(TabSelected),COL(TabSelectedOverline),COL(TabDimmed),COL(TabDimmedSelected),COL(PlotHistogram),COL(PlotHistogramHovered),
    COL(TableHeaderBg),COL(TableBorderStrong),COL(TableBorderLight),COL(TableRowBg),COL(TableRowBgAlt),COL(TextSelectedBg),COL(NavCursor),COL(ModalWindowDimBg)});
#define VAR(n) {#n,ImGuiStyleVar_##n}
  constants(l,"StyleVar",{VAR(Alpha),VAR(DisabledAlpha),VAR(WindowPadding),VAR(FramePadding),VAR(ItemSpacing),VAR(ItemInnerSpacing),
    VAR(FrameRounding),VAR(ChildRounding),VAR(WindowRounding),VAR(PopupRounding),VAR(ScrollbarRounding),VAR(GrabRounding),VAR(TabRounding),
    VAR(WindowBorderSize),VAR(ChildBorderSize),VAR(FrameBorderSize),VAR(PopupBorderSize),VAR(ScrollbarSize),VAR(WindowTitleAlign),VAR(TabBarBorderSize),
    VAR(CellPadding),VAR(GrabMinSize),VAR(SeparatorTextBorderSize)});
#define FLAG(n) {#n,ImGuiWindowFlags_##n}
  constants(l,"WindowFlags",{FLAG(NoCollapse),FLAG(NoDecoration),FLAG(NoSavedSettings),FLAG(NoFocusOnAppearing),FLAG(NoBringToFrontOnFocus),FLAG(NoInputs),FLAG(NoMove),
    FLAG(NoTitleBar),FLAG(NoResize),FLAG(NoScrollbar),FLAG(NoScrollWithMouse),FLAG(AlwaysAutoResize),FLAG(NoBackground),FLAG(MenuBar)});
  constants(l,"Cond",{{"Always",ImGuiCond_Always},{"Once",ImGuiCond_Once},{"FirstUseEver",ImGuiCond_FirstUseEver},{"Appearing",ImGuiCond_Appearing}});
  constants(l,"InputTextFlags",{{"CharsDecimal",ImGuiInputTextFlags_CharsDecimal},{"ReadOnly",ImGuiInputTextFlags_ReadOnly}});
  lua_setglobal(l,"ui");
}
void push_json(lua_State* l,const sentinel::Json& value,int depth=0) {
  if(depth>16)throw std::runtime_error("Preview data exceeds nesting limit");
  if(value.is_array()) {
    lua_createtable(l,static_cast<int>(value.size()),0);lua_Integer index=1;
    for(const auto& item:value){push_json(l,item,depth+1);lua_rawseti(l,-2,index++);}
  }else if(value.is_object()) {
    lua_createtable(l,0,static_cast<int>(value.size()));
    for(const auto& [key,item]:value.items()){push_json(l,item,depth+1);lua_setfield(l,-2,key.c_str());}
  }else if(value.is_boolean())lua_pushboolean(l,value.get<bool>());
  else if(value.is_number_integer())lua_pushinteger(l,value.get<lua_Integer>());
  else if(value.is_number())lua_pushnumber(l,value.get<lua_Number>());
  else if(value.is_string()){const auto& text=value.get_ref<const std::string&>();lua_pushlstring(l,text.data(),text.size());}
  else lua_pushnil(l);
}
void check(lua_State* l,int status) {if(status!=LUA_OK){throw std::runtime_error(lua_tostring(l,-1));}}
float edge(ImVec2 a,ImVec2 b,ImVec2 p) {return (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x);}
void render(const char* out,int width,int height) {
  unsigned char* atlas=nullptr;int tw=0,th=0;
  ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&atlas,&tw,&th);
  std::vector<unsigned char> pix(static_cast<size_t>(width)*height*3);
  for(int y=0;y<height;++y)for(int x=0;x<width;++x){size_t p=(static_cast<size_t>(y)*width+x)*3;float glow=std::max(0.f,1.f-std::hypot(float(x-width*.65),float(y-height*.4))/700.f);pix[p]=static_cast<unsigned char>(10+glow*5);pix[p+1]=static_cast<unsigned char>(17+glow*10);pix[p+2]=static_cast<unsigned char>(21+glow*11);}
  auto sample=[&](float u,float v,int channel){float fx=std::clamp(u*tw-.5f,0.f,float(tw-1)),fy=std::clamp(v*th-.5f,0.f,float(th-1));int x=int(fx),y=int(fy),nx=std::min(x+1,tw-1),ny=std::min(y+1,th-1);float a=fx-x,b=fy-y;return ((1-a)*atlas[(y*tw+x)*4+channel]+a*atlas[(y*tw+nx)*4+channel])*(1-b)+((1-a)*atlas[(ny*tw+x)*4+channel]+a*atlas[(ny*tw+nx)*4+channel])*b;};
  const auto* dd=ImGui::GetDrawData();
  for(int list=0;list<dd->CmdListsCount;++list){const ImDrawList* dl=dd->CmdLists[list];for(const auto& cmd:dl->CmdBuffer){if(cmd.UserCallback)continue;for(unsigned k=0;k<cmd.ElemCount;k+=3){
    const auto&a=dl->VtxBuffer[dl->IdxBuffer[cmd.IdxOffset+k]+cmd.VtxOffset];const auto&b=dl->VtxBuffer[dl->IdxBuffer[cmd.IdxOffset+k+1]+cmd.VtxOffset];const auto&c=dl->VtxBuffer[dl->IdxBuffer[cmd.IdxOffset+k+2]+cmd.VtxOffset];float area=edge(a.pos,b.pos,c.pos);if(std::abs(area)<.001f)continue;
    int x0=std::max({0,int(std::floor(std::min({a.pos.x,b.pos.x,c.pos.x}))),int(cmd.ClipRect.x)}),x1=std::min({width-1,int(std::ceil(std::max({a.pos.x,b.pos.x,c.pos.x}))),int(cmd.ClipRect.z)-1});
    int y0=std::max({0,int(std::floor(std::min({a.pos.y,b.pos.y,c.pos.y}))),int(cmd.ClipRect.y)}),y1=std::min({height-1,int(std::ceil(std::max({a.pos.y,b.pos.y,c.pos.y}))),int(cmd.ClipRect.w)-1});
    for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){ImVec2 p{float(x)+.5f,float(y)+.5f};float wa=edge(b.pos,c.pos,p)/area,wb=edge(c.pos,a.pos,p)/area,wc=1-wa-wb;if(wa<0||wb<0||wc<0)continue;float u=wa*a.uv.x+wb*b.uv.x+wc*c.uv.x,v=wa*a.uv.y+wb*b.uv.y+wc*c.uv.y;auto col=[&](int ch){return wa*((a.col>>(ch*8))&255)+wb*((b.col>>(ch*8))&255)+wc*((c.col>>(ch*8))&255);};float alpha=col(3)*sample(u,v,3)/(255.f*255.f);size_t index=(static_cast<size_t>(y)*width+x)*3;for(int ch=0;ch<3;++ch){float src=col(ch)*sample(u,v,ch)/255.f;pix[index+ch]=static_cast<unsigned char>(std::clamp(src*alpha+pix[index+ch]*(1-alpha),0.f,255.f));}}
  }}}
  if(!stbi_write_png(out,width,height,3,pix.data(),width*3))throw std::runtime_error("Could not write preview PNG");
}
} // namespace
int main(int argc,char**argv) {
  if(argc<4){std::cerr<<"Usage: ui_preview <window.lua> <fixture.lua> <output.png> [library|editor|presets|sharing|settings|activity|overlay] [width height]\n";return 2;}
  lua_State* l=nullptr;
  try{
    const std::string mode=argc>4?argv[4]:"library";
    const int width=argc>5?std::stoi(argv[5]):1280,height=argc>6?std::stoi(argv[6]):900;
    const auto scale=environment("HAILSTONE_UI_SCALE");if(!scale.empty())preview_ui_scale=std::clamp(std::stof(scale),.5f,3.f);
    const auto saved=environment("SENTINEL_PREVIEW_WINDOW");
    if(const auto separator=saved.find('x');separator!=std::string::npos)
      forced_window_size={static_cast<float>(std::clamp(std::stoi(saved.substr(0,separator)),240,4096)),static_cast<float>(std::clamp(std::stoi(saved.substr(separator+1)),200,4096))};
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.DisplaySize={float(width),float(height)};io.DeltaTime=1.f/60;io.IniFilename=nullptr;io.LogFilename=nullptr;
    ImFontConfig fc;fc.OversampleH=2;fc.OversampleV=2;
#ifdef _WIN32
    io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf",18*preview_ui_scale,&fc);
#else
    fc.SizePixels=13*preview_ui_scale;io.Fonts->AddFontDefault(&fc);
#endif
    unsigned char* pixels;int tw,th;io.Fonts->GetTexDataAsRGBA32(&pixels,&tw,&th);io.Fonts->SetTexID(static_cast<ImTextureID>(1));
    l=luaL_newstate();luaL_openlibs(l);bindings(l);lua_pushstring(l,mode.c_str());lua_setglobal(l,"PREVIEW_MODE");
    push_json(l,sentinel::events());lua_setglobal(l,"PREVIEW_CATALOG");
    push_json(l,sentinel::presets());lua_setglobal(l,"PREVIEW_PRESETS");
    check(l,luaL_dofile(l,argv[2]));check(l,luaL_dofile(l,argv[1]));
    if(!environment("HAILSTONE_UI_DIR").empty()) {
      lua_getglobal(l,"PREVIEW_THEME");const bool loaded=lua_istable(l,-1);lua_pop(l,1);
      if(!loaded)throw std::runtime_error("Requested host UI theme did not load; refusing a fallback screenshot.");
    }
    for(int frame=0;frame<5;++frame){
      if(frame==1){if(mode=="library"||mode=="editor")click="  Critical health##a1";else if(mode=="presets")click="Starter kits";else if(mode=="sharing")click="Share & import";else if(mode=="settings")click="Settings & help";else if(mode=="activity")click="Activity";}
      if(frame==2&&mode=="editor")click="Edit alert";
      if(frame==2&&mode=="sharing")click="Export all alerts";
      ImGui::NewFrame();
      for(auto [name,ref]:windows){drawing_main_window=name=="Sentinel";
        if(drawing_main_window)ImGui::SetNextWindowPos({55,50},ImGuiCond_FirstUseEver);
        lua_rawgeti(l,LUA_REGISTRYINDEX,ref);check(l,lua_pcall(l,0,0,0));drawing_main_window=false;}
      ImGui::Render();
    }
    render(argv[3],width,height);lua_close(l);ImGui::DestroyContext();std::cout<<"Rendered actual Lua UI: "<<mode<<" -> "<<argv[3]<<"\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<"\n";if(l)lua_close(l);if(ImGui::GetCurrentContext())ImGui::DestroyContext();return 1;}
}

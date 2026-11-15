-- SPDX-License-Identifier: MIT
-- Run: lua tools/check_ui.lua [ui/modern/sentinel.lua]
-- The host is mocked; this verifies UI flows and balanced stacks, not rendering.
local windows,colors,vars,depth={},0,0,0
local click=nil
local inputs,labels,textcolors={}, {}, {}
local nextsize,sizes,background=nil,{},nil
local colorstack={}
local function hit(label) if click==label then click=nil;return true end;return false end
local constant_id=0
local constants=setmetatable({}, {__index=function(t,k) constant_id=constant_id+1;rawset(t,k,constant_id);return constant_id end})
ui=setmetatable({Col=constants,StyleVar=constants,WindowFlags=constants,Cond=constants,InputTextFlags=constants}, {
  __index=function(_,k) error('Unmocked ui API: '..k) end})
function ui.window(name,fn) windows[name]=fn end
function ui.Begin(name) depth=depth+1;if nextsize then sizes[name]=nextsize;nextsize=nil end;return true,true end
function ui.End() depth=depth-1;assert(depth>=0) end
function ui.BeginChild() depth=depth+1;return true end
function ui.EndChild() depth=depth-1;assert(depth>=0) end
function ui.PushStyleColor(key,r,g,b,a)
  colors=colors+1;colorstack[#colorstack+1]={key,r,g,b,a}
  if key==ui.Col.WindowBg then background={r,g,b,a} end
end
function ui.PopStyleColor(n)
  colors=colors-(n or 1);assert(colors>=0,'color underflow')
  for _=1,(n or 1) do table.remove(colorstack) end
end
function ui.PushStyleVar() vars=vars+1 end
function ui.PopStyleVar(n) vars=vars-(n or 1);assert(vars>=0,'var underflow') end
function ui.GetContentRegionAvail() return 960,600 end
function ui.GetCursorScreenPos() return 20,30 end
function ui.GetWindowPos() return 200,150 end
function ui.GetWindowSize() return 900,660 end
function ui.CalcTextSize(text) return #text*8,18 end
local io={width=1280,height=800,uiScale=1}
function ui.GetIO() return io end
function ui.Button(label) return hit(label) end
function ui.SmallButton(label) return hit(label) end
function ui.Selectable(label) labels[#labels+1]=label;return hit(label) end
function ui.Checkbox(label,v) if hit(label) then return not v,true end;return v,false end
function ui.InputText(id,v) assert(type(v)=='string');if inputs[id]~=nil then return inputs[id],inputs[id]~=v end;return v,false end
function ui.InputTextWithHint(id,_,v) return ui.InputText(id,v) end
function ui.InputTextMultiline(_,v) assert(type(v)=='string','multiline value must be text');return v,false end
function ui.SliderInt(_,v) return v,false end
function ui.SliderFloat(_,v) return v,false end
function ui.BeginCombo() return true end
function ui.EndCombo() end
function ui.CollapsingHeader() return true end
function ui.IsItemHovered() return false end
function ui.IsMouseDown() return false end
function ui.IsMouseReleased() return false end
function ui.IsWindowHovered() return false end
local function record(text)
  for i=#colorstack,1,-1 do local c=colorstack[i];if c[1]==ui.Col.Text then textcolors[text]={c[2],c[3],c[4],c[5]};break end end
end
function ui.Text(text) record(text) end
function ui.TextWrapped(text) record(text) end
function ui.TextColored(r,g,b,a,text) textcolors[text]={r,g,b,a} end
function ui.SetNextWindowSize(w,h) nextsize={w,h} end
for _,name in ipairs({'TextDisabled','Dummy','Separator','SetTooltip','SetNextItemWidth','SetWindowFontScale',
 'DrawCircle','DrawLine','DrawCircleFilled','DrawRectGradient','SetCursorScreenPos',
 'SameLine','PushTextWrapPos','PopTextWrapPos','DrawRectFilled','LabelText','PushID','PopID','BulletText',
 'SetNextWindowPos','SetNextWindowBgAlpha'}) do ui[name]=function() end end
-- A mutable host palette: the same references change when the player switches themes.
local palette={}
for _,key in ipairs({'window','panel','bg','raised','frame','frameHover','frameActive','hover','selected','popup',
 'title','titleActive','border','edge','separator','ink','text','muted','dim','accent','onAccent','heading',
 'danger','bad','gold','hpLow','good','chatBg'}) do palette[key]={.2,.3,.4,1} end
palette.window={.05,.06,.08,1};palette.chatBg={.03,.04,.06,1}
palette.text={.94,.95,.98,1};palette.accent={.48,.78,1,1}
local theme_calls,style_calls=0,0
local is_gamepad=(arg[1] or ''):find('gamepad',1,true)~=nil
local theme={S=1,frame=function() theme_calls=theme_calls+1 end,TestPaletteCycle=function() end}
if is_gamepad then theme.C=palette else theme.color=palette end
local style={}
function style.Push()
  style_calls=style_calls+1
  for _,entry in ipairs({{ui.Col.WindowBg,palette.window},{ui.Col.Text,palette.text},{ui.Col.Button,palette.frame}}) do
    ui.PushStyleColor(entry[1],table.unpack(entry[2]))
  end
end
function style.Pop() ui.PopStyleColor(3) end
local original_dofile=dofile
dofile=function(path)
  if path=='lib/theme.lua' then
    if arg[2]=='inherit' then error('theme intentionally unavailable') end
    theme_calls=theme_calls+1;return theme
  end
  if path=='lib/style.lua' then return style end
  return original_dofile(path)
end
local rule={id='one',name='Low health',enabled=true,event='health_below',category='Survival',section='General',subsection='',
 zone_id=0,subject='',threshold=30,message='{C} needs healing',cooldown=10,chat=true,overlay=true,sound=true,speech=true,
 volume=75,tone='chime',sound_file='',speech_text='',voice=0,rate=0,color='cyan',size=2,duration=4}
local state={open=true,enabled=true,in_zone=true,character='Tester',zone_id=1,zone_name='qeynos',alerts={rule},
 catalog={{id='health_below',label='Health is low',help='Watch health',group='Survival',threshold_label='Health (%)'}},
 presets={rule},history={{name='Low health',message='Heal',time='10:00',test=true,color='red'}},
 overlays={{message='Heal Tester now!',color='red',size=3,remaining=2,duration=4}},
 overlay_locked=true,overlay_x=.5,overlay_y=.22,quiet=false,audio_available=true,speech_available=false,
 sounds={'bell.wav'},config_path='settings/config.json'}
local pet={};for k,v in pairs(rule) do pet[k]=v end
pet.id='pet';pet.name='Protect my pet';pet.event='pet_health_below'
local tell={};for k,v in pairs(rule) do tell[k]=v end
tell.id='tell';tell.name='Never miss a tell';tell.event='incoming_tell'
state.presets={rule,pet,tell}
state.catalog[#state.catalog+1]={id='pet_health_below',label='Pet health is low',group='Party',threshold_label='Health (%)'}
state.catalog[#state.catalog+1]={id='incoming_tell',label='Receive a tell',group='Messages'}
local calls,export_error={},false
local p={}
function p.state() return state end
function p.action(op,id) calls[#calls+1]=op;return {ok=true,id=id} end
function p.save(a) calls[#calls+1]='save';return {ok=true,id=a.id or 'new'} end
function p.preview(a) calls[#calls+1]='preview';return {ok=true} end
function p.export() if export_error then return {ok=false,error='Export is too large.'} end;return '{"format":"hailstone-sentinel"}' end
function p.inspect() return {ok=true,alerts={rule}} end
function p.import() return {ok=true,count=1} end
function p.settings(s) for k,v in pairs(s) do state[k]=v end;return {ok=true} end
eq={Plugin=function() return p end}
assert(loadfile(arg[1] or 'ui/modern/sentinel.lua'))()
local frames=0
local function frame(button)
  labels={};click=button;windows.Sentinel();windows['Sentinel overlay']()
  assert(colors==0,'leaked colors '..colors);assert(vars==0,'leaked styles '..vars);assert(depth==0,'leaked windows '..depth)
  if button then assert(click==nil,'button not reached: '..button) end
  frames=frames+1
end
frame();frame('  Low health##one');frame('Edit alert');frame('Preview');frame('Cancel edits')
local function preset_rows()
  local count=0;for _,label in ipairs(labels) do if label:find('##preset-',1,true) then count=count+1 end end
  return count
end
frame('Starter kits');assert(preset_rows()==3,'starter list must show every matching preset')
inputs['##presetsearch']='no such preset';frame();assert(preset_rows()==0,'unmatched search must have an empty list')
inputs['##presetsearch']='pet';frame();assert(preset_rows()==1,'search must narrow starter rows')
frame('Preview starter')
inputs['##presetsearch']='';frame('Messages##Messages');assert(preset_rows()==1,'category must narrow starter rows')
frame('All categories##All categories');assert(preset_rows()==3,'clearing category must restore starter rows')
frame('Low health##preset-one');frame('Customize this alert');frame('Save alert');frame('Activity')
frame('Share & import');frame('Export all alerts');frame('Review import');frame('Add reviewed alerts (paused)')
export_error=true;frame('Export all alerts') -- a failed export must never become a table in the edit box
frame('Settings & help');frame('Lock alert banners in place');frame()
state.overlay_locked=true
if arg[2]~='inherit' then
  -- A palette switch updates chrome immediately, preserving the chosen alert color.
  state.open=false;frame()
  local semantic=textcolors['Heal Tester now!'];assert(semantic,'overlay message was drawn')
  palette.window[1]=.91;palette.text[1]=.11;palette.accent[1]=.24;palette.chatBg[1]=.08
  state.open=true;frame()
  local unchanged=textcolors['Heal Tester now!']
  assert(unchanged[1]==semantic[1] and unchanged[2]==semantic[2] and unchanged[3]==semantic[3],'theme must not change alert colors')
  assert(background[1]==palette.chatBg[1],'overlay must use the active chat background')
  assert(theme_calls>0,'active pack theme was not loaded')
  if not is_gamepad then assert(style_calls>0,'Modern host style was not used') end
  rule.color='white';rule.message='Readable light-theme message'
  frame('Starter kits')
  local message=textcolors[rule.message]
  assert(message and message[1]==palette.text[1],'inline message previews must stay legible on light palettes')
end
state.overlays={};frame();state.open=false;frame()
local compact
for title,size in pairs(sizes) do if title:find('SentinelMain',1,true) then compact=size end end
assert(compact and compact[1]<=960 and compact[2]<=700,'default window should be moderately compact')
assert(table.concat(calls,','):find('preview'),'preview not called')
assert(table.concat(calls,','):find('save'),'save not called')
print('UI flow check passed: '..frames..' main/overlay frames; balanced styles and windows.')

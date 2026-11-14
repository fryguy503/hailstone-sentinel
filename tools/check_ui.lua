-- SPDX-License-Identifier: MIT
-- Run: lua tools/check_ui.lua [ui/modern/sentinel.lua]
-- The host is mocked; this verifies UI flows and balanced stacks, not rendering.
local windows,colors,vars,depth={},0,0,0
local click=nil
local function hit(label) if click==label then click=nil;return true end;return false end
local constants=setmetatable({}, {__index=function(t,k) rawset(t,k,1);return 1 end})
ui=setmetatable({Col=constants,StyleVar=constants,WindowFlags=constants,Cond=constants,InputTextFlags=constants}, {
  __index=function(_,k) error('Unmocked ui API: '..k) end})
function ui.window(name,fn) windows[name]=fn end
function ui.Begin() depth=depth+1;return true,true end
function ui.End() depth=depth-1;assert(depth>=0) end
function ui.BeginChild() depth=depth+1;return true end
function ui.EndChild() depth=depth-1;assert(depth>=0) end
function ui.PushStyleColor() colors=colors+1 end
function ui.PopStyleColor(n) colors=colors-(n or 1);assert(colors>=0,'color underflow') end
function ui.PushStyleVar() vars=vars+1 end
function ui.PopStyleVar(n) vars=vars-(n or 1);assert(vars>=0,'var underflow') end
function ui.GetContentRegionAvail() return 960,600 end
function ui.GetCursorScreenPos() return 20,30 end
function ui.GetWindowPos() return 200,150 end
function ui.GetIO() return {width=1280,height=800} end
function ui.Button(label) return hit(label) end
function ui.SmallButton(label) return hit(label) end
function ui.Selectable(label) return hit(label) end
function ui.Checkbox(label,v) if hit(label) then return not v,true end;return v,false end
function ui.InputText(_,v) assert(type(v)=='string');return v,false end
function ui.InputTextWithHint(_,_,v) assert(type(v)=='string');return v,false end
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
for _,name in ipairs({'TextColored','Dummy','Separator','SetTooltip','SetNextItemWidth','SetWindowFontScale',
 'Text','TextWrapped','DrawCircle','DrawLine','DrawCircleFilled','DrawRectGradient','SetCursorScreenPos',
 'SameLine','PushTextWrapPos','PopTextWrapPos','DrawRectFilled','LabelText','PushID','PopID','BulletText',
 'SetNextWindowSize','SetNextWindowPos','SetNextWindowBgAlpha'}) do ui[name]=function() end end
local rule={id='one',name='Low health',enabled=true,event='health_below',category='Survival',section='General',subsection='',
 zone_id=0,subject='',threshold=30,message='{C} needs healing',cooldown=10,chat=true,overlay=true,sound=true,speech=true,
 volume=75,tone='chime',sound_file='',speech_text='',voice=0,rate=0,color='cyan',size=2,duration=4}
local state={open=true,enabled=true,in_zone=true,character='Tester',zone_id=1,zone_name='qeynos',alerts={rule},
 catalog={{id='health_below',label='Health is low',help='Watch health',group='Survival',threshold_label='Health (%)'}},
 presets={rule},history={{name='Low health',message='Heal',time='10:00',test=true,color='red'}},
 overlays={{message='Heal Tester now!',color='red',size=3,remaining=2,duration=4}},
 overlay_locked=true,overlay_x=.5,overlay_y=.22,quiet=false,audio_available=true,speech_available=false,
 sounds={'bell.wav'},config_path='settings/config.json'}
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
  click=button;windows.Sentinel();windows['Sentinel overlay']()
  assert(colors==0,'leaked colors '..colors);assert(vars==0,'leaked styles '..vars);assert(depth==0,'leaked windows '..depth)
  if button then assert(click==nil,'button not reached: '..button) end
  frames=frames+1
end
frame();frame('  Low health##one');frame('Edit alert');frame('Preview');frame('Cancel edits')
frame('Starter kits');frame('Customize this alert');frame('Save alert');frame('Activity')
frame('Share & import');frame('Export all alerts');frame('Review import');frame('Add reviewed alerts (paused)')
export_error=true;frame('Export all alerts') -- a failed export must never become a table in the edit box
frame('Settings & help');frame('Lock alert banners in place');frame()
state.overlay_locked=true;state.overlays={};frame();state.open=false;frame()
assert(table.concat(calls,','):find('preview'),'preview not called')
assert(table.concat(calls,','):find('save'),'save not called')
print('UI flow check passed: '..frames..' main/overlay frames; balanced styles and windows.')

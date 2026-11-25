-- SPDX-License-Identifier: MIT
-- Run: lua tools/check_ui.lua [ui/modern/sentinel.lua]
-- The host is mocked; this verifies UI flows and balanced stacks, not rendering.
local windows,colors,vars,depth={},0,0,0
local click,click_combo,current_combo
local inputs,labels,textcolors={}, {}, {}
local fields,last_saved={}
local nextsize,sizes,background=nil,{},nil
local nextpos,current_window,rects,draws,window_flags=nil,nil,{},{},{}
local mouse_down,mouse_released=false,false
local colorstack={}
local varstack,window_bg_alpha,next_bg={}, {},nil
local clip_depth,child_flags=0,{}
local function hit(label) if click==label and (not click_combo or current_combo==click_combo) then click=nil;return true end;return false end
local constant_id=0
local constants=setmetatable({}, {__index=function(t,k) constant_id=constant_id+1;rawset(t,k,constant_id);return constant_id end})
ui=setmetatable({Col=constants,StyleVar=constants,WindowFlags=constants,Cond=constants,InputTextFlags=constants}, {
  __index=function(_,k) error('Unmocked ui API: '..k) end})
function ui.window(name,fn) windows[name]=fn end
function ui.Begin(name,_,flags)
  depth=depth+1
  current_window=name
  local rect=rects[name] or {200,150,900,660}
  if nextpos then rect[1],rect[2]=table.unpack(nextpos);nextpos=nil end
  if nextsize then sizes[name]=nextsize;rect[3],rect[4]=table.unpack(nextsize);nextsize=nil end
  if next_bg then window_bg_alpha[name]=next_bg;next_bg=nil end
  rects[name]=rect;draws[name]={};window_flags[name]=flags
  return true,true
end
function ui.End() depth=depth-1;current_window=nil;assert(depth>=0) end
function ui.BeginChild(id,_,_,_,flags) depth=depth+1;child_flags[id]=flags;return true end
function ui.EndChild() depth=depth-1;assert(depth>=0) end
function ui.PushStyleColor(key,r,g,b,a)
  colors=colors+1;colorstack[#colorstack+1]={key,r,g,b,a}
  if key==ui.Col.WindowBg then background={r,g,b,a} end
end
function ui.PopStyleColor(n)
  colors=colors-(n or 1);assert(colors>=0,'color underflow')
  for _=1,(n or 1) do table.remove(colorstack) end
end
function ui.PushStyleVar(key,v) vars=vars+1;varstack[vars]={key,v} end
function ui.PopStyleVar(n)
  for _=1,(n or 1) do varstack[vars]=nil;vars=vars-1 end
  assert(vars>=0,'var underflow')
end
function ui.GetContentRegionAvail() return 960,600 end
function ui.GetCursorScreenPos() return 20,30 end
function ui.GetWindowPos() return table.unpack(rects[current_window],1,2) end
function ui.GetWindowSize() return table.unpack(rects[current_window],3,4) end
function ui.CalcTextSize(text) return #text*8,18 end
local io={width=1280,height=800,uiScale=1}
function ui.GetIO() return io end
function ui.Button(label) return hit(label) end
function ui.SmallButton(label) return hit(label) end
function ui.Selectable(label) labels[#labels+1]=label;return hit(label) end
function ui.Checkbox(label,v) if hit(label) then return not v,true end;return v,false end
function ui.InputText(id,v) fields[id]=true;assert(type(v)=='string');if inputs[id]~=nil then return inputs[id],inputs[id]~=v end;return v,false end
function ui.InputTextWithHint(id,_,v) return ui.InputText(id,v) end
function ui.InputTextMultiline(_,v) assert(type(v)=='string','multiline value must be text');return v,false end
function ui.SliderInt(id,v) return inputs[id] or v,inputs[id]~=nil and inputs[id]~=v end
function ui.SliderFloat(_,v) return v,false end
function ui.BeginCombo(id) current_combo=id;return true end
function ui.EndCombo() current_combo=nil end
function ui.CollapsingHeader() return true end
function ui.IsItemHovered() return false end
function ui.IsMouseDown() return mouse_down end
function ui.IsMouseReleased() return mouse_released end
function ui.IsWindowHovered() return false end
local texts={}
local function record(text)
  texts[text]=true
  for i=#colorstack,1,-1 do local c=colorstack[i];if c[1]==ui.Col.Text then textcolors[text]={c[2],c[3],c[4],c[5]};break end end
end
function ui.Text(text) record(text) end
function ui.TextWrapped(text) record(text) end
function ui.TextColored(r,g,b,a,text) texts[text]=true;textcolors[text]={r,g,b,a} end
function ui.SetNextWindowSize(w,h) nextsize={w,h} end
function ui.SetNextWindowPos(x,y) nextpos={x,y} end
function ui.SetNextWindowBgAlpha(a) next_bg=a end
function ui.PushClipRect() clip_depth=clip_depth+1 end
function ui.PopClipRect() clip_depth=clip_depth-1;assert(clip_depth>=0) end
function ui.DrawRectFilled(x1,y1,x2,y2,r,g,b,a)
  local opacity=1
  for i=#varstack,1,-1 do if varstack[i][1]==ui.StyleVar.Alpha then opacity=varstack[i][2];break end end
  local window_draws=draws[current_window]
  window_draws[#window_draws+1]={x1,y1,x2,y2,r,g,b,a,opacity}
end
for _,name in ipairs({'TextDisabled','Dummy','Separator','SetTooltip','SetNextItemWidth','SetWindowFontScale',
 'DrawCircle','DrawLine','DrawCircleFilled','DrawRectGradient','SetCursorScreenPos',
 'SameLine','PushTextWrapPos','PopTextWrapPos','LabelText','PushID','PopID','BulletText',
 'SetNextWindowSizeConstraints'}) do ui[name]=function() end end
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
for _,kind in ipairs({'buff','debuff'}) do
  state.catalog[#state.catalog+1]={id=kind..'_received',label='I receive a '..kind,group='Buffs',
    subject_label='Exact spell name (optional)',threshold_label='',help='Watch newly received effects.'}
end
local calls,export_error={},false
local p={}
function p.state() return state end
function p.action(op,id) calls[#calls+1]=op;return {ok=true,id=id} end
function p.save(a) last_saved=a;calls[#calls+1]='save';return {ok=true,id=a.id or 'new'} end
function p.preview(a) calls[#calls+1]='preview';return {ok=true} end
function p.export() if export_error then return {ok=false,error='Export is too large.'} end;return '{"format":"hailstone-sentinel"}' end
local inspected_rules={rule}
function p.inspect() return {ok=true,alerts=inspected_rules} end
function p.import() return {ok=true,count=1} end
function p.settings(s) for k,v in pairs(s) do state[k]=v end;return {ok=true} end
local saved_timer,timer_actions,layouts=nil,{},{}
function p.timer_save(timer)
  saved_timer=timer
  local t=state.timers[1] or {id='race',x=.55,y=.2,width=280,height=74,minimized=false}
  for k,v in pairs(timer) do t[k]=v end
  state.timers={t}
  return {ok=true,id=t.id}
end
function p.timer_action(op,id)
  timer_actions[#timer_actions+1]={op,id}
  if op=='lock' or op=='unlock' then state.timers_locked=op=='lock';return {ok=true} end
  if op=='group' or op=='separate' then state.timers_grouped=op=='group';return {ok=true} end
  if op=='group_alpha' then state.timer_group_layout.alpha=id;return {ok=true} end
  local t=state.timers[1]
  assert(id==t.id)
  if op=='start' then t.remaining,t.run_duration,t.running,t.finished=t.duration,t.duration,true,false end
  if op=='stop' then t.running,t.finished=false,false end
  if op=='delete' then state.timers={} end
  return {ok=true}
end
function p.timer_layout(id,x,y,width,height,minimized)
  local t=id=='group' and state.timer_group_layout or state.timers[1]
  assert(id=='group' or id==t.id)
  layouts[#layouts+1]={x,y,width,height,minimized,id}
  t.x,t.y,t.width,t.height,t.minimized=x,y,width,height,minimized
  return {ok=true}
end
eq={Plugin=function() return p end}
assert(loadfile(arg[1] or 'ui/modern/sentinel.lua'))()
local frames=0
local function frame(button,combo)
  labels={};fields={};texts={};click=button;click_combo=combo;windows.Sentinel();windows['Sentinel overlay']()
  assert(colors==0,'leaked colors '..colors);assert(vars==0,'leaked styles '..vars);assert(depth==0,'leaked windows '..depth)
  assert(clip_depth==0,'leaked clips '..clip_depth)
  if button then assert(click==nil,'button not reached: '..button) end
  frames=frames+1
end
frame();frame('  Low health##one');frame('Edit alert');frame('Preview');frame('Cancel edits')
for _,kind in ipairs({'buff','debuff'}) do
  frame('+ New alert');frame('I receive a '..kind..'##'..kind..'_received')
  assert(fields['##subject'] and not fields['##threshold'],'received effects need an optional name, without a threshold')
  inputs['##subject']=kind=='buff' and 'Clarity' or '';frame();frame('Preview');frame('Save alert')
  assert(last_saved.event==kind..'_received' and last_saved.subject==inputs['##subject'],'received effect selection and optional filter must save')
  inputs['##subject']=nil
end
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

state.open,state.timer_profile,state.timers=true,'profile-one',{}
frame('Timer bars');frame('+ New timer');frame('Save & start')
assert(not saved_timer,'a timer needs a name')
inputs['##timername'],inputs['##timerduration']='  Elixir refresh  ','12'
inputs['##timerwarning'],inputs['##timercritical']='7','7'
inputs['##timeropacity']=70
frame('Save & start');assert(not saved_timer,'critical must be below warning when both are enabled')
inputs['##timercritical']='3'
frame('Save & start')
assert(saved_timer.name=='Elixir refresh' and saved_timer.duration==12 and saved_timer.warning_at==7 and saved_timer.critical_at==3)
assert(saved_timer.alpha==70,'timer opacity belongs to the saved definition')
assert(timer_actions[1][1]=='start' and timer_actions[1][2]=='race','save and start must use the new ID')
inputs={}
local hud='Elixir refresh###SentinelTimer-race'
local timer=state.timers[1]
timer.remaining=7.01;frame();assert(draws[hud][2][5]==.29,'base color before the warning threshold')
assert(window_bg_alpha[hud]==.96*.7 and draws[hud][2][9]==.7,'timer opacity affects both the window and its content')
timer.remaining=7;frame();assert(draws[hud][2][5]==.98 and draws[hud][2][6]==.73,'warning color at the threshold')
timer.remaining=3;frame();assert(draws[hud][2][5]==.98 and draws[hud][2][6]==.4,'critical color at the threshold')
timer.remaining=0;timer.running=false;timer.finished=true;frame()
assert(#draws[hud]==2 and draws[hud][2][3]-draws[hud][2][1]<=2,'completed bars keep only a status marker')
frame('Start');assert(timer.remaining==12 and timer.running)

mouse_down=true
rects[hud]={333,222,340,90}
frame();assert(#layouts==0,'drag and resize do not write while held')
mouse_down,mouse_released=false,true
frame();mouse_released=false
assert(#layouts==1 and layouts[1][1]==333/1280 and layouts[1][2]==222/800 and layouts[1][3]==340 and layouts[1][4]==90)
frame('-##timerminimize');frame()
assert(timer.minimized and rects[hud][3]==192 and rects[hud][4]==52,'minimized HUD must have a smaller footprint')
frame('+##timerexpand');frame()
assert(not timer.minimized and rects[hud][3]==340 and rects[hud][4]==90,'expanding must restore saved dimensions')
local unlocked_flags=window_flags[hud]
frame('Lock timer bars')
assert(state.timers_locked and window_flags[hud]==unlocked_flags+ui.WindowFlags.NoInputs+ui.WindowFlags.NoMove+ui.WindowFlags.NoResize)
frame('Minimize');assert(timer.minimized,'manager can minimize a locked HUD')
frame('Expand');frame('Lock timer bars');frame('Restart');assert(timer.running)
frame('Stop');assert(not timer.running and timer_actions[#timer_actions][1]=='stop')

state.timer_group_layout={x=.63,y=.17,width=320,height=220,minimized=false,alpha=100}
frame('Start')
state.timers[2]={id='second',name='Second bar',duration=8,color='green',minimized=true,alpha=40,
  warning_at=0,critical_at=0,remaining=5,running=true}
frame('Group timer bars')
local group_hud='Sentinel timers###SentinelTimer-group'
assert(rects[group_hud][3]==320 and rects[group_hud][4]==220,'group should use independent default geometry')
assert(#draws[group_hud]==3 and texts['Second bar'],'group should draw both active timers and retain compact rows')
inputs['##timerpaneopacity']=55;frame();frame('Apply pane opacity');inputs={}
assert(state.timer_group_layout.alpha==55 and window_bg_alpha[group_hud]==.96*.55)
assert(draws[group_hud][2][9]==.7*.55 and draws[group_hud][3][9]==.4*.55,'group rows combine both alpha settings')
mouse_down=true;rects[group_hud]={400,275,360,420};frame()
mouse_down,mouse_released=false,true;frame();mouse_released=false
local group_layout=layouts[#layouts]
assert(group_layout[6]=='group' and group_layout[3]==360 and group_layout[4]==420,'pane drag and resize persist only the group layout')
frame('-##timerpaneminimize');frame()
assert(state.timer_group_layout.minimized and rects[group_hud][4]==52 and #draws[group_hud]==0)
frame('Lock timer bars');frame('Expand pane')
assert(child_flags['##timergrouplist']==ui.WindowFlags.NoBackground+ui.WindowFlags.NoInputs,'group scroll child also passes input through when locked')
frame('Lock timer bars');state.timers[2]=nil;frame('Group timer bars')
assert(rects[hud][3]==340 and rects[hud][4]==90,'separate view restores each timer geometry')
frame('Stop')

state.catalog[#state.catalog+1]={id='timer_remaining',label='A timer bar reaches its threshold',group='Timer bars',
  subject_label='Timer bar',threshold_label='Seconds remaining'}
state.timers[2]={id='follow-up',name='Refresh now',duration=8,color='green'}
frame('My alerts');frame('+ New alert');frame('A timer bar reaches its threshold##timer_remaining')
assert(fields['##threshold'] and not fields['##subject'],'timer event needs a picker and seconds threshold')
frame('Elixir refresh (12s)##race','##eventtimer')
frame('Refresh now (8s)##follow-up','##resulttimer')
frame('On-screen banner');frame('Chat message');frame('Play a sound')
assert(texts['When timer "Elixir refresh" finishes, start or restart timer "Refresh now".'])
frame('Preview');frame('Save alert')
assert(last_saved.event=='timer_remaining' and last_saved.subject=='race' and last_saved.threshold==0)
assert(last_saved.timer_id=='follow-up' and not last_saved.overlay and not last_saved.chat and not last_saved.sound and not last_saved.speech)

local imported={}
for k,v in pairs(last_saved) do imported[k]=v end
imported.id,imported.enabled,imported.subject,imported.timer_id='new',false,'unassigned-timer','unassigned-timer'
inspected_rules={imported}
frame('Share & import');frame('Review import')
assert(texts['Check timer conditions and results in the alert editor before enabling imported rules.'])
state.alerts={imported};frame('My alerts')
assert(texts['Edit the alert to choose a timer. You can create one in Timer bars.'])
frame('Edit alert');frame()
assert(texts['Choose an existing timer to replace the missing link.'])
frame('Elixir refresh (12s)##race','##eventtimer');frame('Refresh now (8s)##follow-up','##resulttimer')
frame('Save alert');assert(last_saved.subject=='race' and last_saved.timer_id=='follow-up')
state.alerts={last_saved};state.timers[2]=nil;frame('Timer bars')

frame('Edit timer');frame('Cancel timer edits');frame('Remove timer');frame('Keep timer');assert(#state.timers==1)
frame('Remove timer');frame('Confirm removal');assert(#state.timers==0)
frame('+ New timer');state.in_zone=false;frame();state.in_zone=true
frame('Cancel timer edits')
frame('+ New timer');state.timer_profile='profile-two';frame();frame('+ New timer');frame('Cancel timer edits')
frame('My alerts');frame('+ New alert');frame('A timer bar reaches its threshold##timer_remaining')
state.in_zone=false;frame();state.in_zone=true;frame()
assert(fields['##threshold'],'zoning keeps drafts that reference timers')
state.timer_profile='profile-three';frame()
assert(not fields['##threshold'],'changing characters closes drafts that reference their timers')
print('UI flow check passed: '..frames..' main/overlay frames; balanced styles and windows.')

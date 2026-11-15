-- SPDX-License-Identifier: MIT
-- Invented preview data. No accounts, server captures, or game assets are used.
dofile("tools/preview_theme.lua")
local function rule(id,name,event,color,threshold,section)
  return {id=id,name=name,event=event,color=color,threshold=threshold,enabled=true,category="Personal",section=section or "Survival",
    subsection="",zone_id=0,subject="",message="",cooldown=10,chat=true,overlay=true,sound=true,speech=false,
    volume=75,tone="chime",sound_file="",speech_text="",voice=0,rate=0,size=2,duration=4}
end
local a=rule("a1","Critical health","health_below","red",15)
a.message="HEAL ME NOW"; a.tone="critical"; a.size=3; a.speech=true
local b=rule("a2","Low mana","mana_below","cyan",20)
local c=rule("a3","Protect my pet","pet_health_below","orange",35,"Party")
local d=rule("a4","Never miss a tell","incoming_tell","cyan",0,"Messages")
local e=rule("a5","Invisibility fading","invisibility_fading","purple",0,"Buffs")
e.enabled=false
local catalog={
 {id="health_below",label="My health is low",group="Survival",subject_label="",threshold_label="Health (%)",help="Warn once when your health reaches this percentage. Recover by more than 3% to rearm."},
 {id="mana_below",label="My mana is low",group="Survival",subject_label="",threshold_label="Mana (%)",help="Only evaluates when a valid mana reading is available."},
 {id="pet_health_below",label="My pet's health is low",group="Party",subject_label="",threshold_label="Health (%)",help="Watch your current pet. No pet means no health warning."},
 {id="incoming_tell",label="I receive a tell",group="Messages",subject_label="Text filter (optional)",threshold_label="",help="Match received tells, optionally containing text or matching a wildcard pattern."},
 {id="invisibility_fading",label="My invisibility starts fading",group="Buffs",subject_label="Text filter (optional)",threshold_label="",help="Recognize the client's invisibility-fading warning messages."},
}
local presets={}
for _,v in ipairs({a,b,c,d,e}) do local r={};for k,x in pairs(v) do r[k]=x end;r.description="A ready-made rule. Choose your moment and make it your own.";presets[#presets+1]=r end
presets[1].description="A clear emergency warning at 15% health."
presets[2].description="Know when your mana reserve is running low."
presets[3].description="Warn when your pet reaches 35% health."
catalog=PREVIEW_CATALOG or catalog
presets=PREVIEW_PRESETS or presets
local state={open=PREVIEW_MODE~="overlay",enabled=true,in_zone=true,character="Elowen",zone_id=2,zone_name="Northern Qeynos",alerts={a,b,c,d,e},
  catalog=catalog,presets=presets,history={{name="Critical health",message="HEAL ME NOW",time="14:32:08",test=true,color="red"},
  {name="Low mana",message="Your mana is at 20%",time="14:25:13",test=false,color="cyan"},
  {name="Never miss a tell",message="Mira tells you, 'Ready when you are.'",time="14:22:41",test=false,color="cyan"}},
  overlays={},status="Watching your world. 5 rules ready; 4 enabled.",error="",audio_available=true,speech_available=true,sounds={"soft-bell.wav","raid-warning.wav"},
  config_path="Settings are kept with this character.",revision=1,hp=100,mana=86,endurance=100,overlay_x=.5,overlay_y=.22,overlay_locked=true,quiet=false}
if PREVIEW_MODE=="overlay" then state.overlays={{message="HEAL ME NOW",color="red",size=3,remaining=3.2,duration=4}} end
local p={}
function p.state() return state end
function p.action(op,id)
  if op=="hide" then state.open=false elseif op=="toggle" then state.enabled=not state.enabled end
  return {ok=true,id=id}
end
function p.save(a) return {ok=true,id=a.id or "new"} end
function p.preview() return {ok=true} end
function p.export() return '{\n  "format": "hailstone-sentinel",\n  "version": 1,\n  "alerts": [{\n    "name": "Critical health",\n    "event": "health_below",\n    "threshold": 15,\n    "message": "HEAL ME NOW"\n  }]\n}' end
function p.inspect() return {ok=true,alerts={a,b}} end
function p.import() return {ok=true,count=2} end
function p.settings(s) for k,v in pairs(s) do state[k]=v end;return {ok=true} end
eq={Plugin=function() return p end}

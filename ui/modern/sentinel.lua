-- SPDX-License-Identifier: MIT
-- Sentinel's own UI. Uses only the public Hailstone Lua window API.
local C = {
  bg = {0.045, 0.065, 0.078}, panel = {0.065, 0.090, 0.105}, raised = {0.085, 0.120, 0.137},
  ink = {0.91, 0.94, 0.92}, muted = {0.55, 0.65, 0.68}, teal = {0.29, 0.85, 0.74},
  amber = {0.98, 0.73, 0.36}, red = {0.98, 0.40, 0.42}, line = {0.15, 0.22, 0.24},
}
local COLORS = {
  {"red", "Coral", {0.98,0.40,0.42}}, {"orange", "Orange", {1,0.58,0.29}},
  {"yellow", "Amber", C.amber}, {"green", "Green", {0.50,0.86,0.48}},
  {"cyan", "Teal", C.teal}, {"blue", "Blue", {0.42,0.68,1}},
  {"purple", "Lavender", {0.74,0.62,1}}, {"white", "White", C.ink},
}
local SIZES = {{0,"Small"},{1,"Medium"},{2,"Large"},{3,"Extra large"},{4,"Huge"}}
local TONES = {{"chime","Chime"},{"alert","Alert"},{"critical","Critical"},{"question","Question"},{"default","Default"}}
local P, st
local tab, selected, draft, search = "library", nil, nil, ""
local category, notice, notice_bad = "All categories", "", false
local share_in, share_out, inspected, delete_id = "", "", nil, nil
local preset_search, library_scroll = "", false
local overlay_dragged = false
local overlay_height,overlay_signature

local function copy(t)
  local r = {}; for k,v in pairs(t or {}) do r[k] = type(v)=="table" and copy(v) or v end; return r
end
local function tint(c, text) ui.TextColored(c[1],c[2],c[3],1,tostring(text or "")) end
local function muted(text) tint(C.muted,text) end
local function gap(n) ui.Dummy(0,n or 8) end
local function line() ui.Separator() end
local function hint(text) if ui.IsItemHovered() then ui.SetTooltip(text) end end
local function color(id)
  for _,v in ipairs(COLORS) do if v[1]==id then return v[3] end end; return C.teal
end
local function label(list,id)
  for _,v in ipairs(list) do if (v.id or v[1])==id then return v.label or v[2] end end
  return tostring(id or "Choose one")
end
local function result(r,success)
  if r and r.ok then notice,notice_bad = success or "Saved.",false; return true end
  notice,notice_bad = (r and r.error) or "Sentinel could not complete this action.",true; return false
end
local function act(op,id,message) return result(P.action(op,id or ""),message) end
local function export_alerts(id)
  local value=P.export(id or "")
  if type(value)=="string" then share_out=value; return true end
  result(value); return false
end
local function sounds_path()
  local config=st.config_path or ""
  local dir=config:match("^(.*[/\\])[^/\\]+$")
  return dir and (dir.."Sounds") or "the character's Sentinel settings folder / Sounds"
end
local function selected_alert()
  for _,a in ipairs(st.alerts or {}) do if a.id==selected then return a end end
  return nil
end
local function event_for(id)
  for _,e in ipairs(st.catalog or {}) do if e.id==id then return e end end
  return {label=tostring(id or "Event"),help="Choose when this alert should notify you."}
end
local function combo(id,list,current,width)
  ui.SetNextItemWidth(width or -1)
  local value=current
  if ui.BeginCombo(id,label(list,current)) then
    for _,v in ipairs(list) do
      local key = v.id or v[1]
      if ui.Selectable((v.label or v[2]).."##"..tostring(key),current==key) then value=key end
    end
    ui.EndCombo()
  end
  return value
end
local function check(id,v) local n,changed=ui.Checkbox(id,v==true); return changed and n or (not changed and v==true) end
local function textfield(id,v,help,capacity)
  ui.SetNextItemWidth(-1)
  local n=ui.InputTextWithHint(id,help or "",tostring(v or ""),capacity or 256)
  return n
end
local function heading(kicker,title,desc)
  tint(C.teal,kicker); gap(3)
  ui.SetWindowFontScale(1.3); ui.Text(title); ui.SetWindowFontScale(1)
  if desc then muted(desc) end
  gap(8)
end
local function section(n,title)
  gap(10); tint(C.teal,string.format("%02d",n).."  "..title); line(); gap(3)
end
local function field(label_text)
  gap(4); muted(label_text)
end
local function pill(text,c)
  c=c or C.teal
  ui.PushStyleColor(ui.Col.Button,c[1],c[2],c[3],0.14)
  ui.PushStyleColor(ui.Col.Text,c[1],c[2],c[3],1)
  ui.Button(text); ui.PopStyleColor(2)
end
local function primary(text,w,h)
  ui.PushStyleColor(ui.Col.Button,0.14,0.50,0.43,1)
  ui.PushStyleColor(ui.Col.ButtonHovered,0.19,0.63,0.54,1)
  local hit=ui.Button(text,w or 0,h or 30); ui.PopStyleColor(2); return hit
end
local function setstyles()
  local cols={
    {ui.Col.Text,C.ink},{ui.Col.TextDisabled,C.muted},{ui.Col.WindowBg,C.bg},{ui.Col.ChildBg,C.panel},
    {ui.Col.PopupBg,C.panel},{ui.Col.Border,C.line},{ui.Col.FrameBg,C.raised},
    {ui.Col.FrameBgHovered,{0.11,0.20,0.22}},{ui.Col.FrameBgActive,{0.12,0.26,0.27}},
    {ui.Col.Button,C.raised},{ui.Col.ButtonHovered,{0.12,0.24,0.26}},
    {ui.Col.ButtonActive,{0.15,0.35,0.34}},{ui.Col.Header,{0.10,0.28,0.26}},
    {ui.Col.HeaderHovered,{0.12,0.30,0.29}},{ui.Col.HeaderActive,{0.14,0.38,0.34}},
    {ui.Col.CheckMark,C.teal},{ui.Col.SliderGrab,C.teal},{ui.Col.SliderGrabActive,C.teal},
    {ui.Col.Separator,C.line},{ui.Col.TitleBg,C.bg},{ui.Col.TitleBgActive,C.panel},
    {ui.Col.ScrollbarBg,C.bg},{ui.Col.ScrollbarGrab,C.line},{ui.Col.TextSelectedBg,{0.14,0.38,0.34}},
  }
  for _,v in ipairs(cols) do ui.PushStyleColor(v[1],v[2][1],v[2][2],v[2][3],1) end
  ui.PushStyleVar(ui.StyleVar.WindowPadding,20,16)
  ui.PushStyleVar(ui.StyleVar.FramePadding,11,7)
  ui.PushStyleVar(ui.StyleVar.ItemSpacing,9,7)
  ui.PushStyleVar(ui.StyleVar.FrameRounding,5)
  ui.PushStyleVar(ui.StyleVar.ChildRounding,8)
  ui.PushStyleVar(ui.StyleVar.WindowRounding,10)
  return #cols,6
end
local function clearstyles(c,v) ui.PopStyleVar(v); ui.PopStyleColor(c) end

local function shield(x,y)
  local t=C.teal
  ui.DrawCircle(x+27,y+28,25,t[1],t[2],t[3],0.13,1)
  ui.DrawCircle(x+27,y+28,18,t[1],t[2],t[3],0.17,1)
  ui.DrawLine(x+27,y+3,x+27,y+53,t[1],t[2],t[3],0.12,1)
  ui.DrawLine(x+2,y+28,x+52,y+28,t[1],t[2],t[3],0.12,1)
  local pts={{27,10},{42,17},{39,33},{27,45},{15,33},{12,17},{27,10}}
  for i=1,#pts-1 do ui.DrawLine(x+pts[i][1],y+pts[i][2],x+pts[i+1][1],y+pts[i+1][2],t[1],t[2],t[3],1,2) end
  ui.DrawLine(x+19,y+27,x+25,y+33,t[1],t[2],t[3],1,2)
  ui.DrawLine(x+25,y+33,x+35,y+22,t[1],t[2],t[3],1,2)
  ui.DrawCircleFilled(x+46,y+10,3,C.amber[1],C.amber[2],C.amber[3],1)
end

local function header()
  local x,y=ui.GetCursorScreenPos(); local w=ui.GetContentRegionAvail()
  ui.DrawRectGradient(x-4,y-4,x+w+4,y+75,0.075,0.16,0.17,0.8,C.bg[1],C.bg[2],C.bg[3],0)
  shield(x,y+1)
  ui.SetCursorScreenPos(x+71,y+3)
  ui.SetWindowFontScale(1.65); ui.Text("SENTINEL"); ui.SetWindowFontScale(1)
  ui.SetCursorScreenPos(x+72,y+35); muted("Your watch. Your rules. Your peace of mind.")
  if w>680 then
    ui.SetCursorScreenPos(x+w-191,y+7)
    ui.PushStyleColor(ui.Col.Text,(st.enabled and C.teal or C.amber)[1],(st.enabled and C.teal or C.amber)[2],(st.enabled and C.teal or C.amber)[3],1)
    if ui.Button(st.enabled and "Watching  |  Pause" or "Paused  |  Resume",191,33) then act("toggle",nil,"Sentinel "..(st.enabled and "paused." or "resumed.")) end
    ui.PopStyleColor()
    ui.SetCursorScreenPos(x+w-191,y+48); muted(st.character~="" and (st.character or "No character") or "Waiting for a character")
  end
  ui.SetCursorScreenPos(x,y+78)
  if w<=680 then if ui.Button(st.enabled and "Pause all alerts" or "Resume alerts") then act("toggle") end end
  local count=0; for _,a in ipairs(st.alerts or {}) do if a.enabled then count=count+1 end end
  local status=not st.enabled and "ALL ALERTS PAUSED" or (st.in_zone and "WATCHING" or "WAITING FOR ZONE")
  tint(st.enabled and C.teal or C.amber,status)
  ui.SameLine(); muted("  /  "..count.." active rules  /  "..tostring(st.zone_name or "No zone"))
  gap(5); line()
end

local function newdraft(a)
  if draft then notice,notice_bad="Finish or cancel your current edits before starting another alert.",true; return end
  draft=copy(a or {name="New alert",event="health_below",enabled=true,category="Personal",section="",subsection="",zone_id=0,
    subject="",threshold=30,message="",cooldown=10,chat=true,overlay=true,sound=true,speech=false,volume=80,tone="alert",
    sound_file="",speech_text="",voice=0,rate=0,color="cyan",size=2,duration=4})
  tab="library"; notice=""; delete_id=nil
end
local function description(a)
  local e=event_for(a.event)
  local s=e.label or tostring(a.event)
  if a.subject and a.subject~="" then s=s..' "'..a.subject..'"' end
  if tostring(a.event):match("_below$") then s=s.." at "..tostring(a.threshold or 0).."%"
  elseif a.event=="buff_fading" then s=s.." within "..tostring(a.threshold or 0).." seconds"
  elseif a.event=="spawn_appears" or a.event=="player_appears" then s=s.." within "..tostring(a.threshold or 0).." units"
  elseif e.threshold_label and e.threshold_label~="" then s=s.." ("..tostring(a.threshold or 0)..")" end
  return s
end
local function sentence(a)
  local outs={}
  if a.overlay then outs[#outs+1]="an on-screen banner" end
  if a.chat then outs[#outs+1]="a chat message" end
  if a.sound then outs[#outs+1]="a sound" end
  if a.speech then outs[#outs+1]="spoken words" end
  return "When "..description(a):lower()..", notify me with "..(#outs>0 and table.concat(outs,", ") or "no outputs selected").."."
end

local function editor()
  local e=event_for(draft.event)
  heading("RULE BUILDER",draft.id and "Refine your alert" or "Create your alert","Start with the moment that matters. Choose how it reaches you.")
  if ui.Button("Cancel edits") then draft=nil; return end
  ui.SameLine()
  if primary("Save alert") then
    local r=P.save(draft)
    if result(r,"Alert saved.") then selected=r.id or draft.id; draft=nil; return end
  end
  ui.SameLine()
  if ui.Button("Preview") then result(P.preview(draft),"Preview sent. Test alerts are marked in Activity.") end
  field("Alert name"); draft.name=textfield("##name",draft.name,"Give this alert a memorable name",160)
  section(1,"WHEN SHOULD SENTINEL NOTIFY YOU?")
  field("Event"); draft.event=combo("##event",st.catalog or {},draft.event)
  e=event_for(draft.event)
  ui.PushTextWrapPos(0); muted(e.help or ""); ui.PopTextWrapPos()
  if e.subject_label and e.subject_label~="" then
    field(e.subject_label); draft.subject=textfield("##subject",draft.subject,"Enter a name or text to watch",512)
  end
  if e.threshold_label and e.threshold_label~="" then
    field(e.threshold_label)
    ui.SetNextItemWidth(-1)
    local text=ui.InputText("##threshold",tostring(draft.threshold or 0),12,ui.InputTextFlags.CharsDecimal)
    draft.threshold=tonumber(text) or draft.threshold
  end
  field("Cooldown between notifications (seconds)")
  ui.SetNextItemWidth(-1); draft.cooldown=ui.SliderInt("##cooldown",math.floor(draft.cooldown or 10),0,300,tostring(draft.cooldown or 10).." seconds")
  hint("A condition alerts when it becomes true. Cooldown limits repeated events. It does not create a repeating alarm.")
  section(2,"HOW SHOULD IT REACH YOU?")
  draft.overlay=check("On-screen banner",draft.overlay); ui.SameLine(); draft.chat=check("Chat message",draft.chat)
  draft.sound=check("Play a sound",draft.sound); ui.SameLine(); draft.speech=check("Speak the message",draft.speech)
  field("Message (leave empty for a helpful automatic message)")
  draft.message=textfield("##message",draft.message,"For example: Heal {C} now!",1024)
  muted("Tokens: {C} your name  /  {L} matched text  /  {COUNTER} times fired")
  muted("Chat patterns can also insert captures such as {S1} and {N1}.")
  if draft.overlay then
    field("Banner color"); draft.color=combo("##color",COLORS,draft.color)
    field("Banner size"); draft.size=combo("##size",SIZES,draft.size or 2)
    field("Time on screen"); ui.SetNextItemWidth(-1)
    draft.duration=ui.SliderFloat("##duration",draft.duration or 4,0.5,30)
  end
  if draft.sound then
    gap(5)
    if not st.audio_available then tint(C.amber,"Audio is unavailable on this build; visual and chat alerts still work.") end
    field("Sound"); draft.tone=combo("##tone",TONES,draft.tone or "alert")
    field("Custom WAV file (optional)")
    local sounds={{"","Use built-in sound"}}
    for _,f in ipairs(st.sounds or {}) do sounds[#sounds+1]={f,f} end
    draft.sound_file=combo("##soundfile",sounds,draft.sound_file or "")
    hint("Place WAV files in "..sounds_path()..", then rescan. Use a plain filename.")
    if ui.SmallButton("Rescan sounds") then act("rescan_sounds",nil,"Sound list refreshed.") end
    field("Volume"); ui.SetNextItemWidth(-1); draft.volume=ui.SliderInt("##volume",draft.volume or 80,0,100)
  end
  if draft.speech then
    gap(5)
    if not st.speech_available then tint(C.amber,"Speech is unavailable on this platform. Other outputs will still work.") end
    field("Spoken message (empty uses the alert message)")
    draft.speech_text=textfield("##speech",draft.speech_text,"Time to heal",1024)
    field("Voice"); draft.voice=combo("##voice",{{0,"System default"},{1,"Alternate system voice"}},draft.voice or 0)
    field("Speech rate"); ui.SetNextItemWidth(-1); draft.rate=ui.SliderInt("##rate",draft.rate or 0,-10,10)
  end
  section(3,"KEEP IT ORGANIZED")
  field("Category"); draft.category=textfield("##category",draft.category,"Personal, Raids, or your own category",80)
  field("Section"); draft.section=textfield("##section",draft.section,"Optional: encounter or activity",80)
  field("Subsection"); draft.subsection=textfield("##subsection",draft.subsection,"Optional: phase or group",80)
  field("Zone restriction (0 = any zone)")
  ui.SetNextItemWidth(-1)
  local z=ui.InputText("##zone",tostring(draft.zone_id or 0),12,ui.InputTextFlags.CharsDecimal)
  draft.zone_id=tonumber(z) or draft.zone_id
  if ui.SmallButton("Use this zone") then draft.zone_id=st.zone_id or 0 end
  ui.SameLine(); if ui.SmallButton("Any zone") then draft.zone_id=0 end
  gap(5); draft.enabled=check("Enable this alert after saving",draft.enabled)
  gap(12); line(); ui.TextWrapped(sentence(draft)); gap(12)
  if primary("Save alert##bottom",145) then
    local r=P.save(draft); if result(r,"Alert saved.") then selected=r.id or draft.id; draft=nil end
  end
end

local function library_list(w,h)
  ui.BeginChild("##librarylist",w,h,true)
  tint(C.teal,"YOUR WATCHLIST")
  search=textfield("##search",search,"Search alerts, events, or sections")
  local cats={{"All categories","All categories"}}; local seen={}
  for _,a in ipairs(st.alerts or {}) do
    local c=a.category or "Personal"; if not seen[c] then seen[c]=true; cats[#cats+1]={c,c} end
  end
  category=combo("##categories",cats,category)
  gap(3)
  if primary("+ New alert",-1) then newdraft() end
  gap(6); line()
  local found,lastgroup=0,nil
  local ordered,groups={},{}
  local function groupkey(a) return table.concat({a.category or "",a.section or "",a.subsection or ""},"\t") end
  for _,a in ipairs(st.alerts or {}) do
    ordered[#ordered+1]=a
    local key=groupkey(a); if not groups[key] then groups[key]=#ordered end
  end
  table.sort(ordered,function(a,b)
    local ag,bg=groups[groupkey(a)],groups[groupkey(b)]
    if ag~=bg then return ag<bg end
    return (a.name or ""):lower()<(b.name or ""):lower()
  end)
  for _,a in ipairs(ordered) do
    local hay=table.concat({a.name or "",a.category or "",a.section or "",a.subsection or "",event_for(a.event).label or ""}," "):lower()
    if (category=="All categories" or a.category==category) and hay:find(search:lower(),1,true) then
      found=found+1
      local group=(a.category or "Personal")..(a.section~="" and a.section and (" / "..a.section) or "")
      if a.subsection and a.subsection~="" then group=group.." / "..a.subsection end
      if group~=lastgroup then gap(7); muted(group); lastgroup=group end
      local is_selected=selected==a.id and not draft
      ui.PushStyleColor(ui.Col.Text,(a.enabled and C.ink or C.muted)[1],(a.enabled and C.ink or C.muted)[2],(a.enabled and C.ink or C.muted)[3],1)
      if ui.Selectable((a.enabled and "  " or "[off] ")..(a.name or "Untitled").."##"..tostring(a.id),is_selected,0,0,35) then
        if not draft then selected=a.id end
      end
      ui.PopStyleColor()
      hint(description(a)..(draft and "\nFinish or cancel your edits before selecting another alert." or ""))
    end
  end
  if found==0 then gap(12); ui.TextWrapped(search~="" and "No alerts match your search." or "A quiet watchlist. Add your first alert or explore Starter kits.") end
  gap(8); muted(found.." of "..#(st.alerts or {}).." alerts")
  ui.EndChild()
end
local function details(a)
  if not a then
    heading("WELCOME TO YOUR WATCH","Notice the moment.","Stay in the fight. Sentinel keeps an eye on what matters.")
    gap(16)
    tint(C.teal,"01   CHOOSE YOUR MOMENT")
    ui.TextWrapped("Watch health and resources, buffs, casts, nearby spawns, chat, and more.")
    gap(13); tint(C.teal,"02   MAKE IT YOURS")
    ui.TextWrapped("Pick a clear banner, a sound, a chat message, or a spoken reminder. Preview before saving.")
    gap(13); tint(C.teal,"03   PLAY WITH CONFIDENCE")
    ui.TextWrapped("Rules watch automatically. Pause them all with one click whenever you need quiet.")
    gap(22)
    if primary("Explore starter kits",190,36) then tab="presets" end
    ui.SameLine(); if ui.Button("Create my own",155,36) then newdraft() end
    gap(18); muted("Open anytime with /sentinel or the Plugins menu.")
    return
  end
  heading("ALERT DETAILS",a.name or "Untitled",a.enabled and "This rule is enabled." or "This rule is paused.")
  if primary("Edit alert") then newdraft(a); return end
  ui.SameLine(); if ui.Button("Preview") then act("test",a.id,"Preview sent.") end
  ui.SameLine(); if ui.Button(a.enabled and "Pause rule" or "Enable rule") then act("toggle_alert",a.id,"Rule updated.") end
  gap(16); tint(C.teal,"WHEN"); ui.TextWrapped(description(a)); gap(5)
  ui.TextWrapped(event_for(a.event).help or "")
  gap(14); tint(C.teal,"THEN"); ui.TextWrapped(sentence(a))
  if a.message and a.message~="" then gap(8); tint(color(a.color),a.message) end
  gap(14); line(); gap(6)
  local function detail(k,v) muted(k);ui.SameLine(125);ui.Text(v) end
  detail("Cooldown",tostring(a.cooldown or 0).." seconds")
  detail("Scope",(tonumber(a.zone_id) or 0)==0 and "Any zone" or ("Zone "..a.zone_id))
  detail("Collection",a.category or "Personal")
  if a.section and a.section~="" then detail("Section",a.section) end
  if a.subsection and a.subsection~="" then detail("Subsection",a.subsection) end
  gap(20)
  if ui.Button("Duplicate") then act("duplicate",a.id,"Alert duplicated.") end
  ui.SameLine(); if ui.Button("Share this alert") and export_alerts(a.id) then tab="sharing" end
  ui.SameLine()
  ui.PushStyleColor(ui.Col.Text,C.red[1],C.red[2],C.red[3],1)
  if ui.Button("Delete") then delete_id=a.id end
  ui.PopStyleColor()
  if delete_id==a.id then
    gap(9); tint(C.amber,"Delete this alert permanently?")
    if ui.Button("Keep alert") then delete_id=nil end
    ui.SameLine(); if ui.Button("Yes, delete") then
      if act("delete",a.id,"Alert deleted.") then delete_id=nil; selected=nil end
    end
  end
end
local function library()
  local w,h=ui.GetContentRegionAvail()
  local lw=math.min(300,math.max(230,w*0.29)); local narrow=w<660
  library_list(narrow and w or lw,narrow and 230 or h)
  if not narrow then ui.SameLine() end
  ui.BeginChild("##details",narrow and w or 0,narrow and 400 or h,false)
  if draft then editor() else details(selected_alert()) end
  ui.EndChild()
end

local function presets()
  heading("A GREAT PLACE TO START","Make your first watch.","Ready-made rules with sensible defaults. Review, customize, then save.")
  preset_search=textfield("##presetsearch",preset_search,"Find a starter: health, mana, pet, buff...")
  gap(10)
  local width=ui.GetContentRegionAvail()
  local columns=width>820 and 2 or 1
  local card_width=(width-10*(columns-1))/columns
  local shown=0
  for i,a in ipairs(st.presets or {}) do
    if ((a.name or "").." "..(a.description or "")):lower():find(preset_search:lower(),1,true) then
      if shown%columns~=0 then ui.SameLine() end
      ui.PushID("preset"..i)
      ui.BeginChild("##card",card_width,145,true)
      tint(color(a.color),a.name or "Starter alert")
      ui.TextWrapped(a.description or event_for(a.event).help or "")
      if primary("Customize this alert",177) then local d=copy(a); d.id=nil; newdraft(d) end
      ui.SameLine(); muted(event_for(a.event).group or "")
      ui.EndChild(); ui.PopID(); shown=shown+1
      if shown%columns==0 then gap(4) end
    end
  end
end
local function activity()
  heading("ACTIVITY","Your recent moments.","See what fired, what it said, and whether it was a preview.")
  if ui.Button("Clear activity") then act("clear_history",nil,"Activity cleared.") end
  gap(8)
  if #(st.history or {})==0 then gap(20); tint(C.teal,"Nothing to report. You're all caught up."); ui.TextWrapped("Your alerts will appear here as they happen. Use Preview on any rule to try it out.") end
  for i,v in ipairs(st.history or {}) do
    ui.PushID("history"..i)
    tint(color(v.color),v.name or "Alert")
    ui.SameLine(); muted(v.test and "PREVIEW" or "LIVE")
    if v.time and v.time~="" then ui.SameLine(); muted(v.time) end
    ui.TextWrapped(v.message or ""); gap(5); line(); gap(5)
    ui.PopID()
  end
end
local function sharing()
  heading("BETTER TOGETHER","Share the watch.","Send a rule or a complete collection. Imports are reviewed and added paused.")
  tint(C.teal,"EXPORT")
  if primary("Export all alerts") then export_alerts("") end
  ui.SameLine()
  if selected_alert() and ui.Button("Export selected") then export_alerts(selected) end
  gap(4); muted("Select the text below, then press Ctrl+C. Send it however you prefer.")
  ui.SetNextItemWidth(-1)
  share_out=ui.InputTextMultiline("##export",share_out,131072,-1,100,ui.InputTextFlags.ReadOnly)
  gap(12); tint(C.teal,"IMPORT")
  muted("Click below and press Ctrl+V, then review the alerts before adding them.")
  ui.SetNextItemWidth(-1)
  local incoming,changed=ui.InputTextMultiline("##import",share_in,131072,-1,120)
  share_in=incoming; if changed then inspected=nil end
  if ui.Button("Review import") then
    inspected=P.inspect(share_in)
    result(inspected,"Import checked. Review the rules below.")
  end
  ui.SameLine(); if ui.Button("Clear text") then share_in=""; inspected=nil end
  if inspected and inspected.ok then
    gap(10); tint(C.teal,tostring(#(inspected.alerts or {})).." alerts ready for review")
    for _,a in ipairs(inspected.alerts or {}) do ui.BulletText((a.name or "Untitled").."  /  "..description(a)) end
    gap(5); ui.TextWrapped("These rules will be added as separate, paused alerts. Enable the ones you want after reviewing their settings.")
    gap(5)
    if primary("Add reviewed alerts (paused)",260) then
      local r=P.import(share_in)
      if result(r,tostring(r and r.count or 0).." alerts imported, paused and ready to review.") then inspected=nil; share_in="" end
    end
  end
end

local function settings()
  heading("YOUR SENTINEL","Settle in.","A few comfort settings, and answers when you need them.")
  tint(C.teal,"OVERLAY POSITION")
  local locked=check("Lock alert banners in place",st.overlay_locked~=false)
  if locked~=(st.overlay_locked~=false) then result(P.settings({overlay_locked=locked}),locked and "Overlay locked." or "Drag the overlay handle to position it.") end
  ui.TextWrapped("Unlock to reveal a draggable preview on your screen. Move it, then lock it when it feels right.")
  if ui.Button("Reset banner position") then result(P.settings({overlay_x=0.5,overlay_y=0.22}),"Banner position reset.") end
  gap(15); tint(C.teal,"QUIET MODE")
  local quiet=check("Mute sounds and speech",st.quiet==true)
  if quiet~=(st.quiet==true) then result(P.settings({quiet=quiet}),quiet and "Quiet mode enabled." or "Audio notifications resumed.") end
  ui.TextWrapped("Banners and chat messages remain visible. Master Pause stops all automatic alerts.")
  gap(16); line(); gap(12); tint(C.teal,"GOOD TO KNOW")
  if ui.CollapsingHeader("How alerts work",0) then
    ui.TextWrapped("State alerts notify you when their condition becomes true. They rearm after the condition clears. Cooldowns prevent repeated events from getting noisy. Previews let you check presentation without changing your character.")
    ui.TextWrapped("Sentinel observes and notifies. It never casts spells, targets enemies, or controls your character.")
  end
  if ui.CollapsingHeader("Missing data and zone changes",0) then
    ui.TextWrapped("The client can only report what the server shares. Unknown health, unavailable mana, or missing target buffs do not count as zero. Group health checks watch group members visible in your zone. State comparisons reset when you zone.")
    ui.TextWrapped("Some servers do not send target buffs or other players' mana. Very short casts may finish between observations.")
  end
  if ui.CollapsingHeader("Custom sounds and speech",0) then
    ui.TextWrapped("Place WAV files in the Sounds folder beside this character's Sentinel settings, then use Rescan sounds in the rule builder. Select a file for your rule. Short, recognizable sounds work best.")
    ui.TextWrapped(sounds_path())
    ui.TextWrapped("Speech uses installed Windows SAPI voices. Speech is currently unavailable on Linux and macOS; visual and chat alerts work independently.")
    muted("Audio: "..(st.audio_available and "available" or "unavailable").."   /   Speech: "..(st.speech_available and "available" or "unavailable"))
  end
  if ui.CollapsingHeader("Sharing and your settings",0) then
    ui.TextWrapped("Sharing exports alert settings as text. Review imports before adding them; imported alerts begin paused. Your existing rules stay in your watchlist. Rules are saved separately for each server and character.")
    ui.TextWrapped(st.config_path or "Your settings folder is available after entering the world.")
  end
  if ui.CollapsingHeader("Quick commands",0) then
    ui.TextWrapped("/sentinel opens this window. /sentinel on, off, and toggle control the master switch. /sentinel list lists rules; /sentinel test <number> previews one. Enable the addon for your character through the Plugins controls.")
  end
  gap(16); muted("SENTINEL  /  Crafted for Hailstone")
end

local TABS={{"library","My alerts"},{"presets","Starter kits"},{"activity","Activity"},{"sharing","Share & import"},{"settings","Settings & help"}}
ui.window("Sentinel",function()
  P=eq.Plugin("sentinel"); if not P then return end
  st=P.state(); if not st or not st.open then return end
  local nc,nv=setstyles()
  local io=ui.GetIO()
  ui.SetNextWindowSize(math.min(1060,math.max(540,(io.width or 1280)-80)),math.min(760,math.max(420,(io.height or 900)-90)),ui.Cond.FirstUseEver)
  local visible,open=ui.Begin("Sentinel##SentinelMain",true,ui.WindowFlags.NoCollapse)
  if not open then P.action("hide","") end
  if visible then
    header()
    local avail=ui.GetContentRegionAvail()
    local bw=math.max(90,math.floor((avail-36)/5))
    for i,v in ipairs(TABS) do
      if i>1 then ui.SameLine() end
      local chosen=tab==v[1]
      if chosen then ui.PushStyleColor(ui.Col.Button,0.12,0.32,0.29,1); ui.PushStyleColor(ui.Col.Text,C.teal[1],C.teal[2],C.teal[3],1) end
      if ui.Button(v[2],bw,32) then tab=v[1] end
      if chosen then ui.PopStyleColor(2) end
    end
    gap(10)
    local _,h=ui.GetContentRegionAvail()
    ui.BeginChild("##sentinelbody",0,math.max(100,h-43),false)
    if tab=="library" then library()
    elseif tab=="presets" then presets()
    elseif tab=="activity" then activity()
    elseif tab=="sharing" then sharing()
    else settings() end
    ui.EndChild()
    gap(4); line()
    if notice~="" then
      tint(notice_bad and C.red or C.teal,notice)
      hint(notice)
    elseif st.error and st.error~="" then
      tint(C.red,st.error)
    else
      muted(st.status or "Ready when you are. Choose a rule, or make your first one.")
    end
  end
  ui.End(); clearstyles(nc,nv)
end)

-- A separate, input-free HUD while locked. Unlocking reveals a movable placeholder,
-- so its position can be adjusted even when no real alert is currently visible.
ui.window("Sentinel overlay",function()
  local p=eq.Plugin("sentinel"); if not p then return end
  local s=p.state(); if not s then return end
  local overlays=s.overlays or {}; local locked=s.overlay_locked~=false
  if #overlays==0 and locked then return end
  local io=ui.GetIO(); local sw,sh=io.width or 1280,io.height or 720
  local w=math.min(620,sw-32)
  local h=not locked and 32 or 0
  local signature=locked and "locked" or "unlocked"
  for _,a in ipairs(overlays) do
    h=h+math.max(67,({22,26,32,40,50})[(tonumber(a.size) or 2)+1]+38)
    signature=signature..tostring(a.message)..tostring(a.size)
  end
  if #overlays==0 then h=h+90 end
  if signature==overlay_signature and overlay_height then h=overlay_height end
  local flags=ui.WindowFlags.NoDecoration+ui.WindowFlags.NoSavedSettings+ui.WindowFlags.NoFocusOnAppearing+ui.WindowFlags.NoBringToFrontOnFocus
  if locked then flags=flags+ui.WindowFlags.NoInputs+ui.WindowFlags.NoMove end
  local c,v=setstyles()
  ui.PushStyleVar(ui.StyleVar.WindowPadding,14,11)
  ui.SetNextWindowBgAlpha(0.94)
  local px=(s.overlay_x or 0.5)*sw-w/2
  local py=(s.overlay_y or 0.22)*sh
  if locked or not overlay_dragged then ui.SetNextWindowPos(math.max(0,math.min(sw-w,px)),math.max(0,math.min(sh-h,py)),ui.Cond.Always) end
  ui.SetNextWindowSize(w,math.min(h+20,sh-20),ui.Cond.Always)
  local visible=ui.Begin("##SentinelBanners",nil,flags)
  if visible then
    local _,content_start_y=ui.GetCursorScreenPos()
    if not locked then
      muted("DRAG TO POSITION  /  Lock in Settings & help")
      line()
    end
    if #overlays==0 then
      local x,y=ui.GetCursorScreenPos()
      shield(x,y+2)
      ui.SetCursorScreenPos(x+68,y+15); tint(C.teal,"Your important moments appear here.")
      ui.SetCursorScreenPos(x+68,y+40); muted("Drag this banner to a comfortable spot.")
    end
    for i,a in ipairs(overlays) do
      local ac=color(a.color)
      local x,y=ui.GetCursorScreenPos(); local aw=ui.GetContentRegionAvail()
      local sz=({16,20,26,34,44})[(tonumber(a.size) or 2)+1] or 26
      ui.DrawRectFilled(x-3,y,x+1,y+sz+15,ac[1],ac[2],ac[3],1,2)
      ui.SetCursorScreenPos(x+11,y)
      ui.SetWindowFontScale(sz/18)
      ui.PushStyleColor(ui.Col.Text,ac[1],ac[2],ac[3],1)
      ui.PushTextWrapPos(0); ui.TextWrapped(a.message or "Sentinel alert"); ui.PopTextWrapPos()
      ui.PopStyleColor(); ui.SetWindowFontScale(1)
      local remain=math.max(0,math.min(1,(tonumber(a.remaining) or 0)/math.max(0.01,tonumber(a.duration) or 4)))
      local bx,by=ui.GetCursorScreenPos()
      ui.DrawRectFilled(bx,by+3,bx+aw,by+5,C.line[1],C.line[2],C.line[3],0.65,1)
      ui.DrawRectFilled(bx,by+3,bx+aw*remain,by+5,ac[1],ac[2],ac[3],0.8,1)
      gap(i<#overlays and 17 or 9)
    end
    local _,content_end_y=ui.GetCursorScreenPos()
    overlay_height=content_end_y-content_start_y+22
    overlay_signature=signature
    if not locked then
      if ui.IsWindowHovered() and ui.IsMouseDown(0) then overlay_dragged=true end
      if overlay_dragged and ui.IsMouseReleased(0) then
        local x,y=ui.GetWindowPos()
        p.settings({overlay_x=math.max(0,math.min(1,(x+w/2)/sw)),overlay_y=math.max(0,math.min(1,y/sh))})
        overlay_dragged=false
      end
    else overlay_dragged=false end
  end
  ui.End(); ui.PopStyleVar(); clearstyles(c,v)
end)

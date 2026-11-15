-- SPDX-License-Identifier: MIT
-- Preview-only bridge to an existing client UI pack. No theme code is copied.
local pack=os.getenv("HAILSTONE_UI_DIR")
if not pack or pack=="" then return end
local original=dofile
local settings={palette=os.getenv("HAILSTONE_PALETTE") or "midnight"}
ui.GetSetting=function(key) return settings[key] end
ui.SetSetting=function(key,value) settings[key]=value end
ui.Log=function() end
dofile=function(path)
  if path:match("^lib/") then
    local value=original(pack.."/"..path)
    if path=="lib/theme.lua" then PREVIEW_THEME=value end
    return value
  end
  return original(path)
end

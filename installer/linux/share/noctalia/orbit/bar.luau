--!nonstrict
-- Orbit bar widget. Draws the status the service publishes (see service.luau).
--
--   horizontal bar: [icon] 45% · 12M/s
--   vertical bar:   icon, a thin progress line, then 45 (like the CPU/RAM gauges)
--
-- Left click opens Orbit; right click pauses everything, or resumes it when nothing
-- is moving. The tooltip has the details.

local status = nil -- decoded JSON line from `orbit status --watch --json`
local hasTooltip = false
-- barWidget.isVertical() is only right once the host calls update(), so nothing is
-- drawn before that.
local vertical = nil

local function command()
  local cmd = noctalia.getConfig("command")
  if cmd == nil or cmd == "" then
    cmd = "orbit"
  end
  return cmd
end

local function num(value)
  return type(value) == "number" and value or 0
end

-- 12.3M, 850K, 4.0G: short enough for a bar.
local function shortBytes(bytes)
  local units = { "B", "K", "M", "G", "T" }
  local i = 1
  while bytes >= 1000 and i < #units do
    bytes /= 1024
    i += 1
  end
  if i == 1 then
    return `{math.floor(bytes)}B`
  end
  return string.format(bytes < 10 and "%.1f%s" or "%.0f%s", bytes, units[i])
end

local function longBytes(bytes)
  local units = { "B", "KB", "MB", "GB", "TB" }
  local i = 1
  while bytes >= 1024 and i < #units do
    bytes /= 1024
    i += 1
  end
  return string.format(i == 1 and "%.0f %s" or "%.1f %s", bytes, units[i])
end

local function eta(seconds)
  seconds = math.floor(seconds)
  if seconds <= 0 then
    return nil
  elseif seconds < 60 then
    return `{seconds}s`
  elseif seconds < 3600 then
    return string.format("%dm %02ds", seconds // 60, seconds % 60)
  end
  return string.format("%dh %02dm", seconds // 3600, (seconds % 3600) // 60)
end

local function setTooltip(rows)
  if rows then
    barWidget.setTooltip(rows)
    hasTooltip = true
  elseif hasTooltip then
    barWidget.clearTooltip()
    hasTooltip = false
  end
end

local function tooltipRows(s)
  if not s or not s.running then
    if s and s.missing then
      return { { key = "Orbit", value = "command not found (plugin settings)" } }
    end
    if s and s.outdated then
      return { { key = "Orbit", value = "update Orbit to 3.2 or newer for this widget" } }
    end
    return { { key = "Orbit", value = "not running" } }
  end
  local rows = {}
  local current = s.current
  if type(current) == "table" and current.name and current.name ~= "" then
    table.insert(rows, { key = "Now", value = current.name })
  end
  local counts = {}
  for _, pair in ipairs({ { "active", "downloading" }, { "queued", "queued" }, { "paused", "paused" }, { "seeding", "seeding" } }) do
    local n = num(s[pair[1]])
    if n > 0 then
      table.insert(counts, `{n} {pair[2]}`)
    end
  end
  table.insert(rows, { key = "Downloads", value = #counts > 0 and table.concat(counts, ", ") or "idle" })
  if num(s.total) > 0 then
    table.insert(rows, {
      key = "Progress",
      value = `{math.floor(num(s.progress) * 100)}%  ({longBytes(num(s.downloaded))} of {longBytes(num(s.total))})`,
    })
  end
  if num(s.downloadRate) > 0 or num(s.uploadRate) > 0 then
    table.insert(rows, { key = "Speed", value = `↓ {longBytes(num(s.downloadRate))}/s   ↑ {longBytes(num(s.uploadRate))}/s` })
  end
  local left = eta(num(s.eta))
  if left then
    table.insert(rows, { key = "Time left", value = left })
  end
  if s.metered then
    table.insert(rows, { key = "Paused", value = "metered connection" })
  end
  if type(s.networkInterface) == "string" and s.networkInterface ~= "" then
    table.insert(rows, { key = "Interface", value = s.networkInterface })
  end
  return rows
end

local function render()
  if vertical == nil then
    return
  end
  local s = status
  local running = s ~= nil and s.running == true
  local active = running and num(s.active) or 0
  local busy = running and (active + num(s.queued) + num(s.seeding)) > 0
  local hideIdle = noctalia.getConfig("hide_when_idle") ~= false

  if hideIdle and not busy and not (running and s.metered) then
    barWidget.setVisible(false)
    setTooltip(nil)
    return
  end
  barWidget.setVisible(true)
  setTooltip(tooltipRows(s))

  local glyph = "download"
  local color = nil
  local opacity = 1
  if not running then
    opacity = 0.45
  elseif s.metered then
    glyph = "player-pause"
  elseif active > 0 then
    color = "primary"
  elseif num(s.seeding) > 0 then
    glyph = "arrow-up"
  end

  local hasProgress = active > 0 and num(s.total) > 0
  local percent = hasProgress and math.floor(num(s.progress) * 100) or nil
  local rate = active > 0 and num(s.downloadRate) > 0 and shortBytes(num(s.downloadRate)) or nil

  if vertical then
    local children = { ui.glyph({ name = glyph, color = color, opacity = opacity }) }
    if percent then
      -- Bar over the number, like the built-in CPU/RAM gauges.
      table.insert(children, ui.progress({ progress = num(s.progress), width = 16, height = 3, radius = 2, fill = "primary" }))
      table.insert(children, ui.label({ text = tostring(percent), fontSize = 10, fontWeight = "bold", color = color }))
    elseif rate then
      table.insert(children, ui.label({ text = rate, fontSize = 9, fontWeight = "bold", color = color }))
    end
    barWidget.render(ui.column({ align = "center", gap = 3 }, children))
  else
    local parts = {}
    if percent then
      table.insert(parts, `{percent}%`)
    end
    if rate and noctalia.getConfig("show_speed") ~= false then
      table.insert(parts, `{rate}/s`)
    end
    local children = { ui.glyph({ name = glyph, color = color, opacity = opacity }) }
    if #parts > 0 then
      table.insert(children, ui.label({ text = table.concat(parts, " · "), fontWeight = "bold", color = color }))
    end
    barWidget.render(ui.row({ align = "center", gap = 6 }, children))
  end
end

local function setStatus(line)
  if type(line) ~= "string" then
    return
  end
  local decoded = noctalia.json.decode(line)
  if type(decoded) == "table" then
    status = decoded
    render()
  end
end

noctalia.state.watch("status", setStatus)
setStatus(noctalia.state.get("status"))

-- Called on the widget's update interval; all real updates come from the watch above.
function update()
  noctalia.setUpdateInterval(60000)
  vertical = barWidget.isVertical()
  render()
end

function onClick()
  noctalia.runAsync({ command(), "show" })
end

function onRightClick()
  local s = status
  if not s or not s.running then
    return
  end
  local moving = num(s.active) + num(s.queued) > 0
  noctalia.runAsync({ command(), moving and "pause" or "resume", "--all" })
end

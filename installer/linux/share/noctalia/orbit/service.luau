--!nonstrict
-- Headless service: one `orbit status --watch --json` for every bar on every output.
-- Orbit prints a line only when something changes, keeps running while Orbit itself
-- is closed (it prints {"running":false} and waits), and exits once nobody reads it.
-- Each line goes to the shared state as text; the bar widget decodes it.

local NEEDLES = { "status", "--watch", "--json" }
-- No new attempt before this time: gives a fresh watcher time to show up in the
-- process list, and backs off while Orbit is missing or too old.
local nextTryAt = 0

local function command()
  local cmd = noctalia.getConfig("command")
  if cmd == nil or cmd == "" then
    cmd = "orbit"
  end
  return cmd
end

local function stream(cmd)
  -- exec, so the watcher is our direct child and ends with the shell.
  noctalia.runStream(`exec {cmd} status --watch --json`, function(line)
    if line ~= "" then
      noctalia.state.set("status", line)
    end
  end)
end

local function start()
  local cmd = command()
  if not noctalia.commandExists(cmd) then
    noctalia.log(`orbit: '{cmd}' not found, set the command in the plugin settings`)
    noctalia.state.set("status", '{"running":false,"missing":true}')
    nextTryAt = noctalia.nowMs() + 60000
    return
  end
  nextTryAt = noctalia.nowMs() + 20000
  -- Orbit before 3.2 has no `status --watch`; it would fail at once, every time.
  noctalia.runAsync({ cmd, "help" }, function(result)
    if result.exitCode == 0 and string.find(result.stdout, "--watch", 1, true) then
      stream(cmd)
    else
      noctalia.log("orbit: this Orbit has no 'status --watch', update it")
      noctalia.state.set("status", '{"running":false,"outdated":true}')
      nextTryAt = noctalia.nowMs() + 300000
    end
  end, 5000)
end

-- runStream has no exit callback, so look for the watcher now and then and start it
-- again if it ended (an Orbit update replaced the binary, it crashed, ...).
local function ensureRunning()
  if noctalia.nowMs() < nextTryAt then
    return
  end
  local ok = noctalia.processMatches(function(matched)
    if not matched then
      start()
    end
  end, table.unpack(NEEDLES))
  if not ok then
    start()
  end
end

noctalia.setUpdateInterval(30000)
start()

function update()
  ensureRunning()
end

function onConfigChanged()
  nextTryAt = 0
  ensureRunning()
end

-- Signing in to this game's backend (backends/<game>/server.py) with an emailed code, like Supercell ID.
-- Copied in by: python tools/dev.py backend <game>
--
--   local auth = include("auth.lua")
--   auth.send_code(email, function(ok, err) end)          -- the code is emailed (or shown in the dev.py window)
--   auth.verify(email, code, function(ok, err) end)       -- signed in from now on, until auth.logout()
--   auth.me(function(user, err) end)                      -- user = { id =, email = }
--   auth.logout(function() end)      auth.delete(function(ok, err) end)      auth.signed_in()
--   auth.call(method, path, body, function(text, err) end)   -- your own calls, sent signed in
--
-- For the code, show the keyboard with input.keyboard(true, "code"): iOS offers a code that has just
-- arrived in Mail or Messages above the keys.
-- ponytail: the token is kept with save.set, which is a plain file. Move it to the iOS Keychain before release.

local auth = {}
local token = save.get("auth_token", nil)

-- Text as a JSON string, safe whatever the player typed.
local function quote(s)
  return '"' .. (tostring(s):gsub('[%c"\\]', function(c) return string.format("\\u%04x", c:byte()) end)) .. '"'
end

function auth.call(method, path, body, done)
  local headers = { ["Content-Type"] = "application/json" }
  if token then headers.Authorization = "Bearer " .. token end
  net.backend({ path = path, method = method, headers = headers, body = body }, function(status, text, err)
    if err then return done(nil, err) end
    if status == 401 and token and path ~= "/verify" then token = nil save.set("auth_token", nil) end   -- the session ended
    if status >= 400 then return done(nil, text:match('"error":%s*"(.-)"') or ("the server said " .. status)) end
    done(text)
  end)
end

function auth.send_code(email, done)
  auth.call("POST", "/code", "{\"email\":" .. quote(email) .. "}", function(text, err) done(text ~= nil, err) end)
end

function auth.verify(email, code, done)
  auth.call("POST", "/verify", "{\"email\":" .. quote(email) .. ",\"code\":" .. quote(code) .. "}", function(text, err)
    if not text then return done(false, err) end
    token = text:match('"token":%s*"(.-)"')
    save.set("auth_token", token)
    done(token ~= nil, token == nil and "no token in the answer" or nil)
  end)
end

function auth.me(done)
  auth.call("GET", "/me", nil, function(text, err)
    if not text then return done(nil, err) end
    done({ id = tonumber(text:match('"id":%s*(%d+)')), email = text:match('"email":%s*"(.-)"') })
  end)
end

function auth.logout(done)
  auth.call("POST", "/logout", nil, function()
    token = nil
    save.set("auth_token", nil)
    if done then done() end
  end)
end

function auth.delete(done)
  auth.call("DELETE", "/me", nil, function(text, err)
    if text then token = nil save.set("auth_token", nil) end
    if done then done(text ~= nil, err) end
  end)
end

function auth.signed_in() return token ~= nil end

return auth

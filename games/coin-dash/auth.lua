-- Signing in to this game's backend (backends/<game>/server.py). Copied in by: python tools/dev.py backend <game>
--
--   local auth = include("auth.lua")
--   auth.register(email, password, function(ok, err) end)
--   auth.login(email, password, function(ok, err) end)
--   auth.me(function(user, err) end)                 -- user = { id =, email = }
--   auth.logout(function() end)        auth.delete(function(ok, err) end)        auth.signed_in()
--   auth.call(method, path, body, function(text, err) end)   -- your own calls, sent signed in
--
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
    if status == 401 and path ~= "/login" then token = nil save.set("auth_token", nil) end   -- the session ended
    if status >= 400 then return done(nil, text:match('"error":%s*"(.-)"') or ("the server said " .. status)) end
    done(text)
  end)
end

local function sign_in_with(path)
  return function(email, password, done)
    auth.call("POST", path, "{\"email\":" .. quote(email) .. ",\"password\":" .. quote(password) .. "}", function(text, err)
      if not text then return done(false, err) end
      token = text:match('"token":%s*"(.-)"')
      save.set("auth_token", token)
      done(token ~= nil, token == nil and "no token in the answer" or nil)
    end)
  end
end

auth.register = sign_in_with("/register")
auth.login = sign_in_with("/login")

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

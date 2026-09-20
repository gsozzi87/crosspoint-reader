// Prueba de escritorio del cajón donde corren las apps de la tarjeta.
//
// Es la única forma de estar seguro de que `io`, `os`, `require` y `load` NO
// están cuando uno cree que no están: en el aparato eso se vería recién el día
// que una app maliciosa (o distraída) intente abrir un archivo.
//
// Compilar y correr:
//   g++ -std=c++17 -I lib/Lua/src -I src/lua test/lua_sandbox/test_sandbox.cpp \
//       src/lua/LuaSandbox.cpp lib/Lua/src/*.c -o /tmp/test_sandbox && /tmp/test_sandbox

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "LuaSandbox.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace {
int failures = 0;

void check(const bool ok, const char* what) {
  printf("%s  %s\n", ok ? "ok  " : "FALLA", what);
  if (!ok) ++failures;
}

// Corre un pedazo de Lua en un cajón nuevo. Devuelve "" si salió bien, o el
// mensaje de error. `steps` en 0 = sin guardia de instrucciones.
std::string run(const char* code, const int steps = 0, const size_t cap = 1024 * 1024) {
  lua_State* L = luasandbox::create(cap);
  if (!L) return "sin memoria";
  std::string err;
  if (luaL_loadbuffer(L, code, strlen(code), "=prueba") != LUA_OK) {
    err = lua_tostring(L, -1);
  } else {
    if (steps > 0) luasandbox::armStepLimit(L, steps);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) err = lua_tostring(L, -1);
    luasandbox::clearStepLimit(L);
  }
  luasandbox::destroy(L);
  return err;
}

bool ranClean(const char* code) { return run(code).empty(); }
bool failed(const char* code) { return !run(code).empty(); }

// --- Las apps de ejemplo -------------------------------------------------
//
// El `cp` de acá es un doble: devuelve valores plausibles y no dibuja nada. Lo
// que se prueba es que las apps de examples/Apps compilen y que sus callbacks
// corran sin error contra la API que promete docs/ws397/APPS_LUA.md. Si alguien
// agrega una función a `cp` en el aparato y no acá, una app que la use falla
// en esta prueba, que es justamente cuando conviene enterarse.

int stubZero(lua_State* L) {
  lua_pushinteger(L, 0);
  return 1;
}
int stubNil(lua_State* L) {
  lua_pushnil(L);
  return 1;
}
int stubNone(lua_State*) { return 0; }
int stubWidth(lua_State* L) {
  lua_pushinteger(L, 480);
  return 1;
}
int stubHeight(lua_State* L) {
  lua_pushinteger(L, 800);
  return 1;
}
int stubTextW(lua_State* L) {
  lua_pushinteger(L, 8 * static_cast<int>(strlen(luaL_checkstring(L, 1))));
  return 1;
}
int stubTextH(lua_State* L) {
  lua_pushinteger(L, 18);
  return 1;
}
int stubMs(lua_State* L) {
  lua_pushinteger(L, 12345);
  return 1;
}
int stubTrue(lua_State* L) {
  lua_pushboolean(L, 1);
  return 1;
}

// Con el reloj puesto y sin el reloj puesto: el aparato de verdad devuelve nil
// mientras no esté en hora, y una app que no lo contemple se rompe justo cuando
// alguien la abre recién sacada de la caja.
bool g_relojEnHora = true;

int stubTime(lua_State* L) {
  if (!g_relojEnHora) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, 0, 8);
  const auto campo = [L](const char* nombre, const lua_Integer valor) {
    lua_pushinteger(L, valor);
    lua_setfield(L, -2, nombre);
  };
  campo("year", 2026);
  campo("month", 9);
  campo("day", 14);
  campo("hour", 21);
  campo("min", 7);
  campo("sec", 42);
  campo("wday", 1);
  campo("epoch", 1789500462);
  return 1;
}

// --- El `cp` falso de las puertas (contrato v1) -----------------------------
//
// Lo de red, micrófono, visor y lector va como PRELUDE en Lua sobre unos pocos
// ayudantes en C para los archivos, porque así se lee como el contrato que
// imita: una cola de pedidos que `fake.step()` entrega de a uno llamando a
// on_heard / on_reply, lo mismo que hace LuaAppsActivity desde su loop().
// El escenario (test/lua_sandbox/scenarios/<app>.lua) maneja:
//
//   fake.heard = "texto"        la próxima cp.listen llama on_heard con eso
//   fake.reply[servicio] = f    f(args) -> ok, tabla; la próxima cp.call de ese
//                               servicio llama on_reply con eso
//   fake.download[fileId] = s   contenido que "baja" cp.download (si falta, un
//                               relleno); fake.downloadFails = true la hace fallar
//   fake.key(k), fake.tick()    on_key / on_tick, con la regla de busy()
//   fake.step()                 entrega UNA cosa pendiente (la más vieja)
//   fake.draw()                 corre on_draw y devuelve los textos dibujados
//   fake.advance(ms) / fake.ms  el reloj de cp.ms(), que no avanza solo
//   fake.opened                 lo que abrió cp.view / cp.open_book
//   fake.said                   los textos que cp.say mandó al parlante
//   fake.reload()               vacía la cola (el escenario llama on_open él)
//
// Los archivos van a un directorio temporal: <tmp>/data (la carpeta de la app)
// y <tmp>/books.

std::string g_fsRoot;  // el directorio temporal del escenario en curso

std::string fsPath(const char* sub, const char* name) { return g_fsRoot + "/" + sub + "/" + name; }

int fsRead(lua_State* L) {
  FILE* f = fopen(fsPath(luaL_checkstring(L, 1), luaL_checkstring(L, 2)).c_str(), "rb");
  if (!f) {
    lua_pushnil(L);
    return 1;
  }
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  lua_pushlstring(L, out.data(), out.size());
  return 1;
}

int fsWrite(lua_State* L) {
  size_t len = 0;
  const char* data = luaL_checklstring(L, 3, &len);
  const std::string path = fsPath(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
  const std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) {
    lua_pushboolean(L, 0);
    return 1;
  }
  const bool ok = fwrite(data, 1, len, f) == len;
  fclose(f);
  lua_pushboolean(L, ok && rename(tmp.c_str(), path.c_str()) == 0 ? 1 : 0);
  return 1;
}

int fsRemove(lua_State* L) {
  lua_pushboolean(L, unlink(fsPath(luaL_checkstring(L, 1), luaL_checkstring(L, 2)).c_str()) == 0 ? 1 : 0);
  return 1;
}

int fsExists(lua_State* L) {
  struct stat st;
  lua_pushboolean(L, stat(fsPath(luaL_checkstring(L, 1), luaL_checkstring(L, 2)).c_str(), &st) == 0 ? 1 : 0);
  return 1;
}

int fsList(lua_State* L) {
  lua_newtable(L);
  DIR* d = opendir((g_fsRoot + "/" + luaL_checkstring(L, 1)).c_str());
  if (!d) return 1;
  std::vector<std::string> names;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const std::string n = e->d_name;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".tmp") == 0) continue;
    names.push_back(n);
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  lua_Integer i = 1;
  for (const std::string& n : names) {
    lua_pushstring(L, n.c_str());
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

const char* FAKE_PRELUDE = R"LUA(
fake = { heard = nil, reply = {}, download = {}, downloadFails = false, opened = {}, said = {}, drawn = {}, ms = 12345 }
local pending = {}          -- la cola de pedidos, en orden
local nextId = 0
local saved = nil           -- cp.save / cp.load dentro del escenario
local QUEUE_CAP = 4

local function validName(n)
  return type(n) == "string" and #n >= 1 and #n <= 48 and n:sub(1, 1) ~= "." and n:match("^[A-Za-z0-9._%-]+$") ~= nil
end
local function validToken(n)
  return type(n) == "string" and #n >= 1 and #n <= 64 and n:match("^[A-Za-z0-9._%-]+$") ~= nil
end

cp.ms = function() return fake.ms end
fake.advance = function(ms) fake.ms = fake.ms + ms end
cp.save = function(t) saved = tostring(t):sub(1, 4096); return true end
cp.load = function() return saved end
-- Los mismos tipos que exige el C del aparato: luaL_checkinteger acepta enteros,
-- floats con valor entero y strings numéricos; luaL_checklstring acepta strings
-- y números. Lo demás es "bad argument" en el aparato, así que también acá.
local function chkint(fn, i, v)
  local n = math.tointeger(v) or (type(v) == "string" and math.tointeger(tonumber(v)))
  if n == nil then error(string.format("bad argument #%d to '%s' (number has no integer representation or not a number: %s)", i, fn, tostring(v)), 3) end
  return n
end
local function chkstr(fn, i, v)
  if type(v) == "string" then return v end
  if type(v) == "number" then return tostring(v) end
  error(string.format("bad argument #%d to '%s' (string expected, got %s)", i, fn, type(v)), 3)
end
local rawrect, rawline, rawsel = cp.rect, cp.line, cp.selection
cp.text = function(x, y, s, tam, negrita)
  chkint("text", 1, x); chkint("text", 2, y); s = chkstr("text", 3, s)
  if tam ~= nil then chkint("text", 4, tam) end
  fake.drawn[#fake.drawn + 1] = s
end
-- Anota todo dibujo que se pase del vidrio. La lista queda en
-- `fake.fuera` para que la prueba la mire al terminar.
function fuera_de_pantalla(que, x, y, w, h)
  local W, H = cp.width(), cp.height()
  if x >= 0 and y >= 0 and x + w <= W and y + h <= H then return end
  fake.fuera = fake.fuera or {}
  fake.fuera[#fake.fuera + 1] =
    string.format("%s en (%d,%d) de %dx%d: se pasa de %dx%d", que, x, y, w, h, W, H)
end

cp.rect = function(x, y, w, h, lleno, grosor)
  chkint("rect", 1, x); chkint("rect", 2, y); chkint("rect", 3, w); chkint("rect", 4, h)
  if grosor ~= nil then chkint("rect", 6, grosor) end
  fuera_de_pantalla("rect", x, y, w, h)
  return rawrect(x, y, w, h, lleno, grosor)
end
cp.line = function(x1, y1, x2, y2, grosor)
  chkint("line", 1, x1); chkint("line", 2, y1); chkint("line", 3, x2); chkint("line", 4, y2)
  if grosor ~= nil then chkint("line", 5, grosor) end
  return rawline(x1, y1, x2, y2, grosor)
end
cp.selection = function(x, y, w, h)
  chkint("selection", 1, x); chkint("selection", 2, y); chkint("selection", 3, w); chkint("selection", 4, h)
  fuera_de_pantalla("selection", x, y, w, h)
  return rawsel(x, y, w, h)
end
local rawtextw = cp.textw
cp.textw = function(s, tam) s = chkstr("textw", 1, s); if tam ~= nil then chkint("textw", 2, tam) end; return rawtextw(s, tam) end
cp.image = function(x, y, w, h, bits, escala)
  chkint("image", 1, x); chkint("image", 2, y); chkint("image", 3, w); chkint("image", 4, h)
  assert(type(bits) == "string", "cp.image: bits tiene que ser un string")
  assert(w >= 1 and h >= 1 and w <= 256 and h <= 256, "cp.image: tamaño fuera de rango")
  assert(#bits >= ((w + 7) // 8) * h, "cp.image: faltan bytes")
  fake.images = fake.images or {}
  fake.images[#fake.images + 1] = { x = x, y = y, w = w, h = h, bytes = #bits, escala = escala or 1 }
  -- Que un dibujo se salga de la pantalla no se ve de escritorio y en el vidrio
  -- llena el log del aparato con "N pixeles fuera de pantalla" en CADA cuadro.
  -- El aparato ahora lo recorta, pero recortar es tapar el sintoma: la app
  -- igual esta pidiendo pintar donde no hay pantalla, asi que se anota.
  fuera_de_pantalla("image", x, y, w * (escala or 1), h * (escala or 1))
end

cp.busy = function() return #pending > 0 end

cp.listen = function(seg, pregunta)
  if #pending >= QUEUE_CAP then return false end
  for _, p in ipairs(pending) do if p.kind == "listen" then return false end end
  pending[#pending + 1] = { kind = "listen", seg = seg, pregunta = pregunta }
  return true
end

cp.call = function(service, args)
  assert(validToken(service), "cp.call: servicio inválido")
  assert(args == nil or type(args) == "table", "cp.call: args tiene que ser una tabla")
  if #pending >= QUEUE_CAP then return nil end
  nextId = nextId + 1
  pending[#pending + 1] = { kind = "call", id = nextId, service = service, args = args or {} }
  return nextId
end

cp.download = function(fileId, nombre, destino)
  destino = destino or "app"
  if not validToken(fileId) or not validName(nombre) or (destino ~= "app" and destino ~= "books") then return nil end
  if #pending >= QUEUE_CAP then return nil end
  nextId = nextId + 1
  pending[#pending + 1] = { kind = "download", id = nextId, fileId = fileId, name = nombre, dest = destino }
  return nextId
end

-- cp.say encola como los demás; el host contesta on_reply(id, true, {}) cuando
-- el audio arrancó, y acá además anota el texto en fake.said.
cp.say = function(texto)
  if type(texto) ~= "string" or texto == "" or #texto > 512 then return nil end
  if #pending >= QUEUE_CAP then return nil end
  nextId = nextId + 1
  pending[#pending + 1] = { kind = "say", id = nextId, text = texto }
  return nextId
end

cp.files = function() return hostfs.list("data") end
cp.read = function(nombre, desde, largo)
  if not validName(nombre) then return nil end
  local s = hostfs.read("data", nombre)
  if not s then return nil end
  if desde then return s:sub(desde + 1, desde + (largo or 48 * 1024)) end
  if #s > 48 * 1024 then return nil end
  return s
end
cp.write = function(nombre, texto)
  if not validName(nombre) or type(texto) ~= "string" or #texto > 64 * 1024 then return false end
  return hostfs.write("data", nombre, texto)
end
cp.remove = function(nombre)
  if not validName(nombre) then return false end
  return hostfs.remove("data", nombre)
end
cp.size = function(nombre)
  if not validName(nombre) then return nil end
  local s = hostfs.read("data", nombre)
  return s and #s or nil
end
cp.view = function(nombre, titulo)
  if not validName(nombre) or not hostfs.exists("data", nombre) then return false end
  fake.opened[#fake.opened + 1] = { kind = "view", name = nombre, title = titulo or nombre }
  return true
end
cp.open_book = function(nombre)
  if not validName(nombre) or not hostfs.exists("books", nombre) then return false end
  fake.opened[#fake.opened + 1] = { kind = "book", name = nombre }
  return true
end

local function call(fn, ...)
  local f = rawget(_G, fn)
  if type(f) == "function" then return f(...) end
  return nil
end

-- Entrega UNA cosa pendiente, la más vieja. Devuelve true si entregó algo.
fake.step = function()
  local p = table.remove(pending, 1)
  if not p then return false end
  if p.kind == "listen" then
    local t = fake.heard
    fake.heard = nil
    call("on_heard", t)
  elseif p.kind == "call" then
    local f = fake.reply[p.service]
    if f then
      local ok, t = f(p.args)
      call("on_reply", p.id, ok and true or false, t or {})
    else
      call("on_reply", p.id, false, { error = "sin servicio falso: " .. p.service })
    end
  elseif p.kind == "say" then
    fake.said[#fake.said + 1] = p.text
    call("on_reply", p.id, true, {})
  elseif p.kind == "download" then
    if fake.downloadFails then
      call("on_reply", p.id, false, { error = "descarga fallida (1)" })
    else
      local body = fake.download[p.fileId] or ("<falso " .. p.fileId .. ">")
      local sub = p.dest == "books" and "books" or "data"
      assert(hostfs.write(sub, p.name, body), "no se pudo escribir la descarga falsa")
      call("on_reply", p.id, true, { bytes = #body })
    end
  end
  return true
end

-- Con algo en curso sólo pasa "back", como el host.
fake.key = function(k)
  if #pending > 0 and k ~= "back" then return nil end
  return call("on_key", k)
end
fake.tick = function() return call("on_tick") end
fake.draw = function()
  fake.drawn = {}
  call("on_draw")
  local out = {}
  for i, s in ipairs(fake.drawn) do out[i] = s end
  return out
end
fake.reload = function() pending = {} end
)LUA";

void installStubCp(lua_State* L) {
  static const luaL_Reg CP[] = {
      {"clear", stubNone}, {"text", stubNone},      {"textw", stubTextW}, {"texth", stubTextH},   {"rect", stubNone},
      {"line", stubNone},  {"selection", stubNone}, {"width", stubWidth}, {"height", stubHeight}, {"motion", stubNil},
      {"ms", stubMs},      {"beep", stubNone},      {"log", stubNone},    {"quit", stubNone},     {"save", stubTrue},
      {"load", stubNil},   {"time", stubTime},      {nullptr, nullptr},
  };
  luaL_newlib(L, CP);
  lua_setglobal(L, "cp");
  static const luaL_Reg FS[] = {
      {"read", fsRead}, {"write", fsWrite}, {"remove", fsRemove}, {"exists", fsExists}, {"list", fsList},
      {nullptr, nullptr},
  };
  luaL_newlib(L, FS);
  lua_setglobal(L, "hostfs");
  if (luaL_loadbuffer(L, FAKE_PRELUDE, strlen(FAKE_PRELUDE), "=fake") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
    printf("FALLA el prelude del cp falso: %s\n", lua_tostring(L, -1));
    exit(2);
  }
  (void)stubZero;
}

// Un directorio temporal limpio para los archivos de la app de turno.
void freshFsRoot() {
  if (!g_fsRoot.empty()) {
    const std::string cmd = "rm -rf '" + g_fsRoot + "'";
    if (system(cmd.c_str()) != 0) printf("(no se pudo borrar %s)\n", g_fsRoot.c_str());
  }
  char tmpl[] = "/tmp/lua_sandbox_XXXXXX";
  const char* dir = mkdtemp(tmpl);
  g_fsRoot = dir ? dir : "/tmp";
  mkdir((g_fsRoot + "/data").c_str(), 0700);
  mkdir((g_fsRoot + "/books").c_str(), 0700);
}

std::string slurp(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return {};
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}

// Carga la app y le pega a todos los callbacks, como hace LuaAppsActivity.
std::string runApp(const char* path) {
  const std::string source = slurp(path);
  if (source.empty()) return "no se pudo leer el archivo";
  freshFsRoot();
  lua_State* L = luasandbox::create(1024 * 1024);
  if (!L) return "sin memoria";
  installStubCp(L);
  std::string err;
  if (luaL_loadbuffer(L, source.data(), source.size(), path) != LUA_OK) {
    err = lua_tostring(L, -1);
  } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    err = lua_tostring(L, -1);
  } else {
    struct Call {
      const char* fn;
      const char* arg;
    };
    const Call calls[] = {{"on_open", nullptr}, {"on_key", "up"},     {"on_key", "down"},  {"on_key", "ok"},
                          {"on_key", "back"},   {"on_tick", nullptr}, {"on_draw", nullptr}};
    for (const Call& c : calls) {
      if (lua_getglobal(L, c.fn) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        continue;
      }
      int argc = 0;
      if (c.arg) {
        lua_pushstring(L, c.arg);
        argc = 1;
      }
      luasandbox::armStepLimit(L, 400000);
      const int rc = lua_pcall(L, argc, 1, 0);
      luasandbox::clearStepLimit(L);
      if (rc != LUA_OK) {
        err = std::string(c.fn) + ": " + lua_tostring(L, -1);
        lua_pop(L, 1);
        break;
      }
      lua_pop(L, 1);
    }
  }
  luasandbox::destroy(L);
  return err;
}

// Una partida entera, no un toque a cada callback. Es lo que encuentra los
// errores de verdad: un índice fuera de rango cuando el tablero se llena, un
// bucle que no sale cuando no queda casilla libre, un estado final que nadie
// contempló. Se dibuja después de cada tecla, como hace el aparato.
std::string playApp(const char* path, const char* const* teclas, const int cuantas, const int vueltas) {
  const std::string source = slurp(path);
  if (source.empty()) return "no se pudo leer el archivo";
  freshFsRoot();
  lua_State* L = luasandbox::create(1024 * 1024);
  if (!L) return "sin memoria";
  installStubCp(L);
  std::string err;
  const auto llamar = [&](const char* fn, const char* arg) {
    if (!err.empty()) return;
    if (lua_getglobal(L, fn) != LUA_TFUNCTION) {
      lua_pop(L, 1);
      return;
    }
    int argc = 0;
    if (arg) {
      lua_pushstring(L, arg);
      argc = 1;
    }
    luasandbox::armStepLimit(L, 400000);
    const int rc = lua_pcall(L, argc, 1, 0);
    luasandbox::clearStepLimit(L);
    if (rc != LUA_OK) {
      err = std::string(fn) + ": " + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
    }
    lua_pop(L, 1);
  };
  if (luaL_loadbuffer(L, source.data(), source.size(), path) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
    err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "no carga";
  } else {
    llamar("on_open", nullptr);
    for (int v = 0; v < vueltas && err.empty(); ++v) {
      for (int i = 0; i < cuantas && err.empty(); ++i) {
        llamar("on_key", teclas[i]);
        llamar("on_draw", nullptr);
      }
      llamar("on_tick", nullptr);
    }
  }
  luasandbox::destroy(L);
  return err;
}
// Un escenario: carga la app, corre el guion de test/lua_sandbox/scenarios/
// encima (con `fake` y `cp` a mano) y falla si el guion lanza. El guion llama
// a on_open él mismo, que es como sabe en qué estado arranca.
std::string runScenario(const char* appPath, const char* scenarioPath) {
  const std::string source = slurp(appPath);
  const std::string script = slurp(scenarioPath);
  if (source.empty() || script.empty()) return "no se pudo leer el archivo";
  freshFsRoot();
  lua_State* L = luasandbox::create(1024 * 1024);
  if (!L) return "sin memoria";
  installStubCp(L);
  std::string err;
  if (luaL_loadbuffer(L, source.data(), source.size(), appPath) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
    err = std::string("la app no carga: ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
  } else if (luaL_loadbuffer(L, script.data(), script.size(), scenarioPath) != LUA_OK) {
    err = std::string("el escenario no compila: ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
  } else {
    // Sin guardia de instrucciones: el guion entero es una sola llamada y
    // legítimamente larga. La app, adentro, sigue sin poder abrir archivos.
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
  }
  luasandbox::destroy(L);
  return err;
}

// Todas las apps de examples/Apps, ordenadas: la que agregue alguien mañana
// entra sola en la prueba.
std::vector<std::string> discoverApps() {
  std::vector<std::string> out;
  DIR* d = opendir("examples/Apps");
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".lua") == 0) out.push_back("examples/Apps/" + n);
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

bool fileExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}
}  // namespace

int main() {
  printf("-- lo que TIENE que estar --\n");
  check(ranClean("assert(math.floor(3.7) == 3)"), "math");
  check(ranClean("assert(('abc'):upper() == 'ABC')"), "string");
  check(ranClean("local t = {3,1,2}; table.sort(t); assert(t[1] == 1)"), "table");
  check(ranClean("assert(utf8.len('ñandú') == 5)"), "utf8");
  check(ranClean("local c = coroutine.create(function() coroutine.yield(1) end); coroutine.resume(c)"), "coroutine");
  check(ranClean("assert(select('#', 1, 2) == 2)"), "base: select");
  check(ranClean("assert(tostring(1) == '1' and tonumber('2') == 2)"), "base: tostring/tonumber");
  check(ranClean("assert(pcall(function() end))"), "base: pcall");

  printf("\n-- lo que NO tiene que estar --\n");
  check(failed("io.open('/etc/passwd')"), "io no existe");
  check(failed("os.execute('ls')"), "os no existe");
  check(failed("os.remove('/algo')"), "os.remove no existe");
  check(failed("require('socket')"), "require no existe");
  check(failed("load('return 1')()"), "load no existe");
  check(failed("loadstring('return 1')()"), "loadstring no existe");
  check(failed("dofile('/algo.lua')"), "dofile no existe");
  check(failed("loadfile('/algo.lua')"), "loadfile no existe");
  check(failed("string.dump(function() end)"), "string.dump no existe");
  check(failed("debug.getinfo(1)"), "debug no existe");
  check(failed("package.path = '/'"), "package no existe");

  printf("\n-- la guardia de instrucciones --\n");
  // Sin guardia esto no volvería nunca; con guardia tiene que dar error.
  const std::string spin = run("while true do end", 50000);
  check(!spin.empty(), "un bucle infinito termina en error y no cuelga");
  check(spin.find("demasiado") != std::string::npos, "y el error dice qué pasó");
  // La guardia NO puede cortar trabajo legítimo y corto.
  check(run("local s = 0; for i = 1, 1000 do s = s + i end; assert(s == 500500)", 400000).empty(),
        "un bucle normal pasa sin que la guardia lo toque");
  // Recursión sin fondo: tiene que dar error de Lua, no desbordar el stack de C.
  check(failed("local function f() return f() + 1 end; f()"), "una recursión sin fondo da error de Lua");

  // `pcall` se come el error de la guardia — está en la base y tiene que estar.
  // Hasta 1.5.90 eso alcanzaba para que la app NO terminara nunca, y como
  // `runBounded` espera al worker sin tope, el que quedaba colgado era el loop
  // de Arduino: ni recordatorios, ni reposo, ni forma de salir sin cortar la
  // corriente. Ahora pasarse del tope condena al intérprete (el asignador deja
  // de dar memoria), así que la llamada termina igual.
  const std::string swallowed = run("while true do pcall(function() while true do end end) end", 50000);
  check(!swallowed.empty(), "un bucle que se come el error con pcall termina igual");
  // Y con los pcall anidados, que es la vuelta de tuerca obvia.
  const std::string nested =
      run("pcall(function() while true do pcall(function() while true do end end) end end); error('vivo')", 50000);
  check(!nested.empty(), "ni con pcall anidados la app sigue para siempre");

  printf("\n-- el tope de memoria --\n");
  const std::string fat = run("local t = {}; for i = 1, 1e7 do t[i] = i end", 0, 128 * 1024);
  check(!fat.empty(), "una tabla que crece sin parar se topa contra el techo");
  check(fat.find("memory") != std::string::npos, "y el error lo dice");
  check(luasandbox::memUsed() == 0, "al cerrar no queda nada pedido");

  printf("\n-- el cp falso de las puertas --\n");
  {
    // El prelude tiene que cumplir el contrato que las apps van a suponer:
    // encolar, entregar de a uno, busy() mientras tanto, archivos que persisten.
    freshFsRoot();
    lua_State* L = luasandbox::create(1024 * 1024);
    installStubCp(L);
    const char* code =
        "local heard, replies = nil, {}\n"
        "function on_heard(t) heard = t end\n"
        "function on_reply(id, ok, t) replies[#replies+1] = {id=id, ok=ok, t=t} end\n"
        "assert(cp.listen(10, 'tema') == true)\n"
        "assert(cp.listen(10) == false, 'dos escuchas no')\n"
        "local id = cp.call('x.y', {a=1, b={'p','q'}})\n"
        "assert(id == 1 and cp.busy())\n"
        "fake.heard = 'hola'\n"
        "fake.reply['x.y'] = function(args) assert(args.b[2] == 'q'); return true, {ok=true, jobId='j1', n=7} end\n"
        "assert(fake.step() and heard == 'hola' and #replies == 0, 'de a uno')\n"
        "assert(fake.step() and replies[1].ok and replies[1].t.jobId == 'j1' and replies[1].t.n == 7)\n"
        "assert(not cp.busy() and not fake.step())\n"
        "assert(cp.write('a.txt', 'abc') and cp.read('a.txt') == 'abc' and cp.size('a.txt') == 3)\n"
        "assert(cp.read('a.txt', 1, 1) == 'b' and #cp.files() == 1)\n"
        "assert(cp.view('a.txt', 'T') and fake.opened[1].name == 'a.txt')\n"
        "assert(not cp.view('no.txt') and not cp.write('../x', 'a') and not cp.write('.oculto', 'a'))\n"
        "assert(cp.download('f1', 'l.epub', 'books') == 2); fake.download['f1'] = 'EPUB'\n"
        "assert(fake.step() and replies[2].t.bytes == 4 and cp.open_book('l.epub'))\n"
        "assert(cp.remove('a.txt') and #cp.files() == 0)\n"
        "assert(cp.say('') == nil and cp.say('hola') == 3 and cp.busy())\n"
        "assert(fake.step() and replies[3].ok and fake.said[1] == 'hola' and not cp.busy())\n"
        "cp.save('s'); assert(cp.load() == 's')\n"
        "local m = cp.ms(); fake.advance(5000); assert(cp.ms() == m + 5000)\n";
    std::string err;
    if (luaL_loadbuffer(L, code, strlen(code), "=cpfalso") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
      err = lua_tostring(L, -1);
    }
    luasandbox::destroy(L);
    check(err.empty(), (std::string("el cp falso cumple el contrato") + (err.empty() ? "" : ": " + err)).c_str());
  }

  printf("\n-- las apps de ejemplo y las de fábrica --\n");
  const std::vector<std::string> APPS = discoverApps();
  check(!APPS.empty(), "hay apps en examples/Apps");
  for (const std::string& file : APPS) {
    const std::string err = runApp(file.c_str());
    check(err.empty(), (file + (err.empty() ? "" : ": " + err)).c_str());
  }

  printf("\n-- las mismas apps con el aparato SIN hora --\n");
  g_relojEnHora = false;
  for (const std::string& file : APPS) {
    const std::string err = runApp(file.c_str());
    check(err.empty(), (file + " sin reloj" + (err.empty() ? "" : ": " + err)).c_str());
  }
  g_relojEnHora = true;

  printf("\n-- los escenarios (test/lua_sandbox/scenarios) --\n");
  for (const std::string& file : APPS) {
    const std::string stem = file.substr(strlen("examples/Apps/"), file.size() - strlen("examples/Apps/") - 4);
    const std::string scenario = "test/lua_sandbox/scenarios/" + stem + ".lua";
    if (!fileExists(scenario)) continue;
    const std::string err = runScenario(file.c_str(), scenario.c_str());
    check(err.empty(), (scenario + (err.empty() ? "" : ": " + err)).c_str());
  }

  printf("\n-- partidas enteras --\n");
  {
    // Ahorcado: recorrer el abecedario y probar cada letra. Con 26 letras se
    // termina la palabra o se pierde muchas veces; la app tiene que aguantar
    // las dos cosas y empezar de nuevo con OK.
    const char* teclas[] = {"down", "ok"};
    const std::string err = playApp("examples/Apps/ahorcado.lua", teclas, 2, 60);
    check(err.empty(), (std::string("ahorcado: 60 jugadas") + (err.empty() ? "" : ": " + err)).c_str());
  }
  {
    // Sudoku: mover y poner hasta llenar casillas varias veces. Acá es donde un
    // cursor que no sabe qué hacer sin casillas libres se cuelga.
    const char* teclas[] = {"down", "ok", "up", "ok"};
    const std::string err = playApp("examples/Apps/sudoku.lua", teclas, 4, 40);
    check(err.empty(), (std::string("sudoku: 40 vueltas") + (err.empty() ? "" : ": " + err)).c_str());
  }
  {
    // Lo que se probaba con el reloj (borrado del producto, ver CLAUDE.md) y
    // sigue siendo el caso que importa en tinta: muchos ticks seguidos sin que
    // cambie nada NO tienen que hacer nada raro ni pedir repintado.
    const char* teclas[] = {"ok"};
    const std::string err = playApp("examples/Apps/mascota.lua", teclas, 1, 50);
    check(err.empty(), (std::string("mascota: 50 vueltas") + (err.empty() ? "" : ": " + err)).c_str());
  }

  // El último directorio temporal de los archivos falsos no lo borra nadie más.
  if (!g_fsRoot.empty()) {
    const std::string cmd = "rm -rf '" + g_fsRoot + "'";
    if (system(cmd.c_str()) != 0) printf("(no se pudo borrar %s)\n", g_fsRoot.c_str());
  }
  printf("\n%s (%d fallas)\n", failures == 0 ? "TODO BIEN" : "HAY FALLAS", failures);
  return failures == 0 ? 0 : 1;
}

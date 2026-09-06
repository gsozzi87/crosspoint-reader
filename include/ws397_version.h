// Version of the ws397 build. release.ps1 rewrites WS397_BUILD on every
// release; only the eight files that include this header recompile, instead of
// the whole tree (a changed -D flag invalidates every object file).
#pragma once
#define WS397_BUILD 15
#define WS397_STR_(x) #x
#define WS397_STR(x) WS397_STR_(x)
#define WS397_VERSION "1.5." WS397_STR(WS397_BUILD) "-ws397"

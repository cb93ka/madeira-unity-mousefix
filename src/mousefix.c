/*
 * version.dll proxy for 64-bit Unity games running in Madeira (Wine on iOS).
 *
 * Madeira's "Absolute" pointer mode moves the Windows cursor to the finger.
 * A Unity game that locks the cursor for mouse-look keeps pulling it back to
 * the window centre (ClipCursor / SetCursorPos), so the finger's motion never
 * turns the camera. This proxy patches the game's imports so that, with
 * MOUSEFIX=1, cursor locking is ignored: the cursor stays under the finger,
 * Wine's raw-input deltas follow the finger, and mouse-look works in Absolute
 * mode while dialogs keep a real cursor.
 *
 * Everything the game does with the mouse is summarised once a second through
 * OutputDebugStringA, which Madeira copies into its log ("[mousefix]").
 *
 * Environment (set as env.NAME = value in madeira.cfg):
 *   MOUSEFIX=1           ignore cursor locking (default: log only)
 *   MOUSEFIX_JUMP=N      drop a raw-input step longer than N pixels, the jump
 *                        when a new touch lands far from the last one
 *                        (default 120, 0 keeps every step)
 *   MOUSEFIX_SCALE=F     multiply raw-input deltas (default 1.0)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ config */

static int g_fix;
static int g_jump = 120;
static float g_scale = 1.0f;

static void
read_config(void) {
  char v[32];
  if (GetEnvironmentVariableA("MOUSEFIX", v, sizeof(v)) && v[0] == '1')
    g_fix = 1;
  if (GetEnvironmentVariableA("MOUSEFIX_JUMP", v, sizeof(v)))
    g_jump = atoi(v);
  if (GetEnvironmentVariableA("MOUSEFIX_SCALE", v, sizeof(v)))
    g_scale = (float)atof(v);
}

static void
logf_(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if (n > (int)sizeof(buf) - 2)
    n = sizeof(buf) - 2;
  buf[n] = '\n';
  buf[n + 1] = 0;
  OutputDebugStringA(buf);
}

/* ------------------------------------------------------------- statistics */

static volatile LONG s_clip, s_clip_tiny, s_clip_null, s_setpos, s_getpos, s_show_hide;
static volatile LONG s_raw_calls, s_raw_moves, s_raw_abs, s_raw_dropped, s_rawbuf_calls, s_register;
static volatile LONG s_raw_sum_x, s_raw_sum_y;
static RECT s_last_clip;
static POINT s_last_setpos;
static DWORD s_last_report;

static void
report(void) {
  DWORD now = GetTickCount();
  if (now - s_last_report < 1000)
    return;
  s_last_report = now;
  POINT p = {0, 0};
  GetCursorPos(&p);
  logf_("[mousefix] fix=%d clip=%ld (tiny=%ld null=%ld last=%ld,%ld-%ld,%ld) setpos=%ld (last=%ld,%ld) getpos=%ld "
        "showcursor=%ld raw=%ld moves=%ld abs=%ld dropped=%ld sum=%ld,%ld rawbuf=%ld register=%ld cursor=%ld,%ld",
        g_fix, s_clip, s_clip_tiny, s_clip_null, s_last_clip.left, s_last_clip.top, s_last_clip.right,
        s_last_clip.bottom, s_setpos, s_last_setpos.x, s_last_setpos.y, s_getpos, s_show_hide, s_raw_calls,
        s_raw_moves, s_raw_abs, s_raw_dropped, s_raw_sum_x, s_raw_sum_y, s_rawbuf_calls, s_register, p.x, p.y);
  s_clip = s_clip_tiny = s_clip_null = s_setpos = s_getpos = s_show_hide = 0;
  s_raw_calls = s_raw_moves = s_raw_abs = s_raw_dropped = s_rawbuf_calls = s_register = 0;
  s_raw_sum_x = s_raw_sum_y = 0;
}

/* ------------------------------------------------------------------ hooks */

typedef BOOL(WINAPI *ClipCursor_t)(const RECT *);
typedef BOOL(WINAPI *SetCursorPos_t)(int, int);
typedef BOOL(WINAPI *GetCursorPos_t)(POINT *);
typedef int(WINAPI *ShowCursor_t)(BOOL);
typedef UINT(WINAPI *GetRawInputData_t)(HRAWINPUT, UINT, void *, UINT *, UINT);
typedef UINT(WINAPI *GetRawInputBuffer_t)(RAWINPUT *, UINT *, UINT);
typedef BOOL(WINAPI *RegisterRawInputDevices_t)(const RAWINPUTDEVICE *, UINT, UINT);
typedef FARPROC(WINAPI *GetProcAddress_t)(HMODULE, LPCSTR);

static ClipCursor_t real_ClipCursor;
static SetCursorPos_t real_SetCursorPos;
static GetCursorPos_t real_GetCursorPos;
static ShowCursor_t real_ShowCursor;
static GetRawInputData_t real_GetRawInputData;
static GetRawInputBuffer_t real_GetRawInputBuffer;
static RegisterRawInputDevices_t real_RegisterRawInputDevices;
static GetProcAddress_t real_GetProcAddress;

static BOOL WINAPI
hook_ClipCursor(const RECT *r) {
  InterlockedIncrement(&s_clip);
  if (!r) {
    InterlockedIncrement(&s_clip_null);
  } else {
    s_last_clip = *r;
    if (r->right - r->left <= 2 || r->bottom - r->top <= 2)
      InterlockedIncrement(&s_clip_tiny);
  }
  report();
  if (g_fix && r)
    return real_ClipCursor(NULL); /* keep the cursor free to follow the finger */
  return real_ClipCursor(r);
}

static BOOL WINAPI
hook_SetCursorPos(int x, int y) {
  InterlockedIncrement(&s_setpos);
  s_last_setpos.x = x;
  s_last_setpos.y = y;
  report();
  if (g_fix)
    return TRUE; /* pretend the recentre happened; the finger owns the cursor */
  return real_SetCursorPos(x, y);
}

static BOOL WINAPI
hook_GetCursorPos(POINT *p) {
  InterlockedIncrement(&s_getpos);
  return real_GetCursorPos(p);
}

static int WINAPI
hook_ShowCursor(BOOL show) {
  InterlockedIncrement(&s_show_hide);
  return real_ShowCursor(show);
}

static void
filter_mouse(RAWMOUSE *m) {
  InterlockedIncrement(&s_raw_moves);
  if (m->usFlags & MOUSE_MOVE_ABSOLUTE) {
    InterlockedIncrement(&s_raw_abs);
    return;
  }
  if (g_fix) {
    LONG dx = m->lLastX, dy = m->lLastY;
    if (g_jump > 0 && (dx > g_jump || dx < -g_jump || dy > g_jump || dy < -g_jump)) {
      InterlockedIncrement(&s_raw_dropped);
      m->lLastX = m->lLastY = 0;
      return;
    }
    if (g_scale != 1.0f) {
      m->lLastX = (LONG)(dx * g_scale);
      m->lLastY = (LONG)(dy * g_scale);
    }
  }
  InterlockedExchangeAdd(&s_raw_sum_x, m->lLastX);
  InterlockedExchangeAdd(&s_raw_sum_y, m->lLastY);
}

static UINT WINAPI
hook_GetRawInputData(HRAWINPUT h, UINT cmd, void *data, UINT *size, UINT header) {
  UINT r = real_GetRawInputData(h, cmd, data, size, header);
  InterlockedIncrement(&s_raw_calls);
  if (cmd == RID_INPUT && data && r != (UINT)-1 && r >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
    RAWINPUT *ri = (RAWINPUT *)data;
    if (ri->header.dwType == RIM_TYPEMOUSE)
      filter_mouse(&ri->data.mouse);
  }
  report();
  return r;
}

static UINT WINAPI
hook_GetRawInputBuffer(RAWINPUT *data, UINT *size, UINT header) {
  UINT r = real_GetRawInputBuffer(data, size, header);
  InterlockedIncrement(&s_rawbuf_calls);
  if (data && r != (UINT)-1) {
    RAWINPUT *ri = data;
    for (UINT i = 0; i < r; ++i) {
      if (ri->header.dwType == RIM_TYPEMOUSE)
        filter_mouse(&ri->data.mouse);
      ri = NEXTRAWINPUTBLOCK(ri);
    }
  }
  report();
  return r;
}

static BOOL WINAPI
hook_RegisterRawInputDevices(const RAWINPUTDEVICE *d, UINT n, UINT sz) {
  InterlockedIncrement(&s_register);
  for (UINT i = 0; i < n; ++i)
    logf_("[mousefix] RegisterRawInputDevices page=%u usage=%u flags=0x%lx", d[i].usUsagePage, d[i].usUsage,
          (unsigned long)d[i].dwFlags);
  return real_RegisterRawInputDevices(d, n, sz);
}

struct hook_entry {
  const char *name;
  void *hook;
  void **real;
};

static struct hook_entry g_hooks[] = {
    {"ClipCursor", (void *)hook_ClipCursor, (void **)&real_ClipCursor},
    {"SetCursorPos", (void *)hook_SetCursorPos, (void **)&real_SetCursorPos},
    {"GetCursorPos", (void *)hook_GetCursorPos, (void **)&real_GetCursorPos},
    {"ShowCursor", (void *)hook_ShowCursor, (void **)&real_ShowCursor},
    {"GetRawInputData", (void *)hook_GetRawInputData, (void **)&real_GetRawInputData},
    {"GetRawInputBuffer", (void *)hook_GetRawInputBuffer, (void **)&real_GetRawInputBuffer},
    {"RegisterRawInputDevices", (void *)hook_RegisterRawInputDevices, (void **)&real_RegisterRawInputDevices},
};

static FARPROC WINAPI
hook_GetProcAddress(HMODULE mod, LPCSTR name) {
  if ((ULONG_PTR)name > 0xFFFF) {
    for (size_t i = 0; i < sizeof(g_hooks) / sizeof(g_hooks[0]); ++i)
      if (!strcmp(name, g_hooks[i].name)) {
        logf_("[mousefix] GetProcAddress(%s) redirected", name);
        return (FARPROC)g_hooks[i].hook;
      }
  }
  return real_GetProcAddress(mod, name);
}

/* --------------------------------------------------------------- patching */

static int
patch_slot(void **slot, void *value) {
  DWORD old;
  if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
    return 0;
  *slot = value;
  VirtualProtect(slot, sizeof(void *), old, &old);
  return 1;
}

static int
patch_module(HMODULE mod, const char *label) {
  if (!mod)
    return 0;
  BYTE *base = (BYTE *)mod;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
  IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress)
    return 0;
  int patched = 0;
  for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; ++d) {
    const char *dll = (const char *)(base + d->Name);
    int is_user32 = !_stricmp(dll, "user32.dll");
    int is_kernel = !_stricmp(dll, "kernel32.dll");
    if (!is_user32 && !is_kernel)
      continue;
    IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
    IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
        continue;
      const char *fn = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
      if (is_kernel) {
        if (!strcmp(fn, "GetProcAddress") && patch_slot((void **)&slots->u1.Function, (void *)hook_GetProcAddress))
          ++patched;
        continue;
      }
      for (size_t i = 0; i < sizeof(g_hooks) / sizeof(g_hooks[0]); ++i)
        if (!strcmp(fn, g_hooks[i].name) && patch_slot((void **)&slots->u1.Function, g_hooks[i].hook))
          ++patched;
    }
  }
  logf_("[mousefix] patched %d imports in %s", patched, label);
  return patched;
}

static void
resolve_reals(void) {
  HMODULE u32 = GetModuleHandleA("user32.dll");
  if (!u32)
    u32 = LoadLibraryA("user32.dll");
  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  real_GetProcAddress = (GetProcAddress_t)GetProcAddress(k32, "GetProcAddress");
  for (size_t i = 0; i < sizeof(g_hooks) / sizeof(g_hooks[0]); ++i)
    *g_hooks[i].real = (void *)GetProcAddress(u32, g_hooks[i].name);
}

static DWORD WINAPI
late_patch(void *arg) {
  (void)arg;
  /* GameAssembly.dll (IL2CPP) loads after UnityPlayer starts; patch it when it appears. */
  for (int i = 0; i < 600; ++i) {
    HMODULE ga = GetModuleHandleA("GameAssembly.dll");
    if (ga) {
      patch_module(ga, "GameAssembly.dll");
      break;
    }
    Sleep(100);
  }
  return 0;
}

static void
install(void) {
  read_config();
  resolve_reals();
  logf_("[mousefix] loaded: fix=%d jump=%d scale=%.2f", g_fix, g_jump, g_scale);
  patch_module(GetModuleHandleA(NULL), "game exe");
  patch_module(GetModuleHandleA("UnityPlayer.dll"), "UnityPlayer.dll");
  HANDLE t = CreateThread(NULL, 0, late_patch, NULL, 0, NULL);
  if (t)
    CloseHandle(t);
}

/* ------------------------------------------------- version.dll forwarding */

static HMODULE g_real_version;

static FARPROC
real_version_proc(const char *name) {
  if (!g_real_version) {
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH - 16);
    strcpy(path + n, "\\version.dll");
    g_real_version = LoadLibraryA(path);
  }
  return g_real_version ? real_GetProcAddress ? real_GetProcAddress(g_real_version, name)
                                              : GetProcAddress(g_real_version, name)
                        : NULL;
}

#define FWD(ret, name, params, args)                                                                                   \
  ret WINAPI fwd_##name params {                                                                       \
    typedef ret(WINAPI * fn_t) params;                                                                                 \
    static fn_t fn;                                                                                                    \
    if (!fn)                                                                                                           \
      fn = (fn_t)real_version_proc(#name);                                                                             \
    return fn args;                                                                                                    \
  }

FWD(BOOL, GetFileVersionInfoA, (LPCSTR a, DWORD b, DWORD c, LPVOID d), (a, b, c, d))
FWD(BOOL, GetFileVersionInfoW, (LPCWSTR a, DWORD b, DWORD c, LPVOID d), (a, b, c, d))
FWD(BOOL, GetFileVersionInfoExA, (DWORD f, LPCSTR a, DWORD b, DWORD c, LPVOID d), (f, a, b, c, d))
FWD(BOOL, GetFileVersionInfoExW, (DWORD f, LPCWSTR a, DWORD b, DWORD c, LPVOID d), (f, a, b, c, d))
FWD(DWORD, GetFileVersionInfoSizeA, (LPCSTR a, LPDWORD b), (a, b))
FWD(DWORD, GetFileVersionInfoSizeW, (LPCWSTR a, LPDWORD b), (a, b))
FWD(DWORD, GetFileVersionInfoSizeExA, (DWORD f, LPCSTR a, LPDWORD b), (f, a, b))
FWD(DWORD, GetFileVersionInfoSizeExW, (DWORD f, LPCWSTR a, LPDWORD b), (f, a, b))
FWD(BOOL, VerQueryValueA, (LPCVOID a, LPCSTR b, LPVOID *c, PUINT d), (a, b, c, d))
FWD(BOOL, VerQueryValueW, (LPCVOID a, LPCWSTR b, LPVOID *c, PUINT d), (a, b, c, d))
FWD(DWORD, VerLanguageNameA, (DWORD a, LPSTR b, DWORD c), (a, b, c))
FWD(DWORD, VerLanguageNameW, (DWORD a, LPWSTR b, DWORD c), (a, b, c))
FWD(DWORD, VerFindFileA, (DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h),
    (a, b, c, d, e, f, g, h))
FWD(DWORD, VerFindFileW, (DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h),
    (a, b, c, d, e, f, g, h))
FWD(DWORD, VerInstallFileA, (DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPCSTR e, LPCSTR f, LPSTR g, PUINT h),
    (a, b, c, d, e, f, g, h))
FWD(DWORD, VerInstallFileW, (DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPCWSTR e, LPCWSTR f, LPWSTR g, PUINT h),
    (a, b, c, d, e, f, g, h))
FWD(BOOL, GetFileVersionInfoByHandle, (DWORD a, HANDLE b, LPVOID *c, LPDWORD d), (a, b, c, d))

BOOL WINAPI
DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);
    install();
  }
  return TRUE;
}

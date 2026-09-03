#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <strsafe.h>

#include <stddef.h>
#include <string.h>

#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_browser_capi.h"
#include "include/capi/cef_client_capi.h"
#include "include/capi/cef_life_span_handler_capi.h"
#include "include/capi/cef_load_handler_capi.h"
#include "include/capi/cef_render_handler_capi.h"
#include "include/cef_api_hash.h"

#include "cef_renderer.hpp"

typedef struct renderer_t {
  cef_client_t client;
  cef_life_span_handler_t life_span;
  cef_load_handler_t load;
  cef_render_handler_t render;
  CRITICAL_SECTION lock;
  cef_browser_t* browser;
  unsigned char* frame;
  size_t frame_size;
  int width;
  int height;
  wchar_t source[1024];
  LONG initialized;
  LONG initializing;
} renderer_t;

static renderer_t* g_renderer = NULL;
static CRITICAL_SECTION g_renderer_lock;
static LONG g_renderer_lock_ready = 0;
static LONG g_logged_first_paint = 0;
static const wchar_t kCefTestUrl[] = L"https://www.twitch.tv/michimochievee";
static const int kCefViewportWidth = 1024;
static const int kCefViewportHeight = 1024;

static void write_trace(const wchar_t* text) {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(NULL, path, _countof(path));
  if (!length || length >= _countof(path))
    return;

  wchar_t* name = wcsrchr(path, L'\\');
  if (!name || FAILED(StringCchCopyW(name + 1, _countof(path) - (size_t)(name + 1 - path), L"aisp.cef-renderer.log")))
    return;

  HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE)
    return;

  DWORD written = 0;
  WriteFile(file, text, (DWORD)(wcslen(text) * sizeof(wchar_t)), &written, NULL);
  CloseHandle(file);
}

static void CEF_CALLBACK never_add_ref(cef_base_ref_counted_t* self) {
  (void)self;
}

static int CEF_CALLBACK never_release(cef_base_ref_counted_t* self) {
  (void)self;
  // The renderer lives for the game process. CEF's shutdown happens during
  // process teardown, where releasing injected state is unsafe.
  return 0;
}

static int CEF_CALLBACK always_one_ref(cef_base_ref_counted_t* self) {
  (void)self;
  return 1;
}

static void initialize_base(cef_base_ref_counted_t* base, size_t size) {
  base->size = size;
  base->add_ref = never_add_ref;
  base->release = never_release;
  base->has_one_ref = always_one_ref;
  base->has_at_least_one_ref = always_one_ref;
}

static renderer_t* from_client(cef_client_t* self) {
  return (renderer_t*)self;
}

static renderer_t* from_life_span(cef_life_span_handler_t* self) {
  return (renderer_t*)((char*)self - offsetof(renderer_t, life_span));
}

static renderer_t* from_load(cef_load_handler_t* self) {
  return (renderer_t*)((char*)self - offsetof(renderer_t, load));
}

static renderer_t* from_render(cef_render_handler_t* self) {
  return (renderer_t*)((char*)self - offsetof(renderer_t, render));
}

static cef_life_span_handler_t* CEF_CALLBACK get_life_span_handler(cef_client_t* self) {
  renderer_t* renderer = from_client(self);
  renderer->life_span.base.add_ref(&renderer->life_span.base);
  return &renderer->life_span;
}

static cef_load_handler_t* CEF_CALLBACK get_load_handler(cef_client_t* self) {
  renderer_t* renderer = from_client(self);
  renderer->load.base.add_ref(&renderer->load.base);
  return &renderer->load;
}

static cef_render_handler_t* CEF_CALLBACK get_render_handler(cef_client_t* self) {
  renderer_t* renderer = from_client(self);
  renderer->render.base.add_ref(&renderer->render.base);
  return &renderer->render;
}

static void CEF_CALLBACK on_load_end(cef_load_handler_t* self, cef_browser_t* browser, cef_frame_t* frame, int http_status_code) {
  (void)self;
  (void)browser;
  (void)http_status_code;
  if (!frame || !frame->is_main(frame))
    return;

  static const wchar_t script[] =
      L"(() => {"
      L"if (window.__aispFullscreenVideo) return;"
      L"window.__aispFullscreenVideo = setInterval(() => {"
      L"const video = document.querySelector('video');"
      L"if (!video) return;"
      L"document.documentElement.style.cssText += ';overflow:hidden!important;background:#000!important';"
      L"document.body.style.cssText += ';overflow:hidden!important;background:#000!important';"
      // Protocol 1 pages render at 1024x1024, but the game copies only this
      // rectangle into its TV texture: (9,15)-(495,358).
      L"video.style.cssText += ';position:fixed!important;left:9px!important;top:15px!important;width:486px!important;height:343px!important;max-width:none!important;max-height:none!important;object-fit:cover!important;background:#000!important;z-index:2147483647!important';"
      L"const play = video.play(); if (play) play.catch(() => {});"
      L"}, 250);"
      L"})()";
  cef_string_t code = {};
  cef_string_t script_url = {};
  cef_string_from_wide(script, _countof(script) - 1, &code);
  cef_string_from_wide(L"aisp://fullscreen-video", _countof(L"aisp://fullscreen-video") - 1, &script_url);
  frame->execute_java_script(frame, &code, &script_url, 1);
  cef_string_clear(&code);
  cef_string_clear(&script_url);
  write_trace(L"CEF: fullscreen video script installed\r\n");
}

static void CEF_CALLBACK on_after_created(cef_life_span_handler_t* self, cef_browser_t* browser) {
  renderer_t* renderer = from_life_span(self);
  EnterCriticalSection(&renderer->lock);
  if (renderer->browser)
    renderer->browser->base.release(&renderer->browser->base);
  browser->base.add_ref(&browser->base);
  renderer->browser = browser;
  LeaveCriticalSection(&renderer->lock);
  write_trace(L"CEF: browser created\r\n");
}

static void CEF_CALLBACK get_view_rect(cef_render_handler_t* self, cef_browser_t* browser, cef_rect_t* rect) {
  (void)browser;
  renderer_t* renderer = from_render(self);
  EnterCriticalSection(&renderer->lock);
  rect->x = 0;
  rect->y = 0;
  rect->width = renderer->width;
  rect->height = renderer->height;
  LeaveCriticalSection(&renderer->lock);
}

static void CEF_CALLBACK on_paint(
    cef_render_handler_t* self,
    cef_browser_t* browser,
    cef_paint_element_type_t type,
    size_t dirty_rect_count,
    const cef_rect_t* dirty_rects,
    const void* buffer,
    int width,
    int height) {
  (void)browser;
  (void)dirty_rect_count;
  (void)dirty_rects;
  if (type != PET_VIEW || !buffer || width <= 0 || height <= 0)
    return;

  renderer_t* renderer = from_render(self);
  const size_t size = (size_t)width * (size_t)height * 4;
  EnterCriticalSection(&renderer->lock);
  unsigned char* frame = renderer->frame
      ? (unsigned char*)HeapReAlloc(GetProcessHeap(), 0, renderer->frame, size)
      : (unsigned char*)HeapAlloc(GetProcessHeap(), 0, size);
  if (frame) {
    renderer->frame = frame;
    renderer->frame_size = size;
    renderer->width = width;
    renderer->height = height;
    memcpy(renderer->frame, buffer, size);
    if (InterlockedCompareExchange(&g_logged_first_paint, 1, 0) == 0)
      write_trace(L"CEF: first frame painted\r\n");
  }
  LeaveCriticalSection(&renderer->lock);
}

static void create_browser(renderer_t* renderer) {
  wchar_t source[1024] = {};
  int width = 1;
  int height = 1;
  cef_browser_t* browser = NULL;

  EnterCriticalSection(&renderer->lock);
  StringCchCopyW(source, _countof(source), renderer->source);
  width = renderer->width;
  height = renderer->height;
  if (renderer->browser) {
    renderer->browser->base.add_ref(&renderer->browser->base);
    browser = renderer->browser;
  }
  LeaveCriticalSection(&renderer->lock);

  if (browser) {
    browser->base.release(&browser->base);
    return;
  }

  cef_window_info_t window_info = {};
  window_info.size = sizeof(window_info);
  // No native CEF window is created. Keeping this detached also prevents CEF
  // from applying monitor/DPI window changes to the legacy game window.
  window_info.parent_window = NULL;
  window_info.windowless_rendering_enabled = 1;

  cef_browser_settings_t settings = {};
  settings.size = sizeof(settings);
  settings.windowless_frame_rate = 60;

  cef_string_t url = {};
  cef_string_from_wide(source, wcslen(source), &url);
  if (!cef_browser_host_create_browser(&window_info, &renderer->client, &url, &settings, NULL, NULL))
    write_trace(L"CEF: browser creation request rejected\r\n");
  else
    write_trace(L"CEF: browser creation requested\r\n");
  cef_string_clear(&url);
}

static void set_cef_string(cef_string_t* target, const wchar_t* value) {
  cef_string_from_wide(value, wcslen(value), target);
}

static DWORD WINAPI initialize_cef(void* parameter) {
  renderer_t* renderer = (renderer_t*)parameter;
  wchar_t game_path[MAX_PATH] = {};
  GetModuleFileNameW(NULL, game_path, _countof(game_path));
  wchar_t* slash = wcsrchr(game_path, L'\\');
  if (!slash)
    return 0;
  *slash = L'\0';

  wchar_t runtime_path[MAX_PATH] = {};
  StringCchPrintfW(runtime_path, _countof(runtime_path), L"%s\\aisp.cef", game_path);
  SetDllDirectoryW(runtime_path);

  cef_api_hash(CEF_API_VERSION, 0);
  cef_main_args_t arguments = {};
  arguments.instance = GetModuleHandleW(NULL);
  cef_settings_t settings = {};
  settings.size = sizeof(settings);
  settings.no_sandbox = 1;
  // Keep Chromium's UI work off the legacy game's window/message loop. Browser
  // creation is asynchronous and may be requested from this thread.
  settings.multi_threaded_message_loop = 1;
  settings.windowless_rendering_enabled = 1;

  wchar_t subprocess[MAX_PATH] = {};
  wchar_t locales[MAX_PATH] = {};
  wchar_t cache[MAX_PATH] = {};
  StringCchPrintfW(subprocess, _countof(subprocess), L"%s\\aisp.cef-subprocess.exe", runtime_path);
  StringCchPrintfW(locales, _countof(locales), L"%s\\locales", runtime_path);
  StringCchPrintfW(cache, _countof(cache), L"%s\\aisp.cef-cache", game_path);
  set_cef_string(&settings.browser_subprocess_path, subprocess);
  set_cef_string(&settings.resources_dir_path, runtime_path);
  set_cef_string(&settings.locales_dir_path, locales);
  set_cef_string(&settings.cache_path, cache);

  write_trace(L"CEF: initializing\r\n");
  if (cef_initialize(&arguments, &settings, NULL, NULL)) {
    InterlockedExchange(&renderer->initialized, 1);
    write_trace(L"CEF: initialized\r\n");
    create_browser(renderer);
  } else {
    write_trace(L"CEF: initialization failed\r\n");
  }

  cef_string_clear(&settings.browser_subprocess_path);
  cef_string_clear(&settings.resources_dir_path);
  cef_string_clear(&settings.locales_dir_path);
  cef_string_clear(&settings.cache_path);
  return 0;
}

extern "C" void WINAPI StartCefRenderer(LPCWSTR source, int width, int height) {
  // The local /p URL is protocol 1 in the original client. Its browser surface
  // is always 1024x1024 regardless of the ATL host's transient window size.
  (void)width;
  (void)height;

  if (InterlockedCompareExchange(&g_renderer_lock_ready, 1, 0) == 0)
    InitializeCriticalSection(&g_renderer_lock);
  EnterCriticalSection(&g_renderer_lock);
  if (!g_renderer) {
    g_renderer = (renderer_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*g_renderer));
    if (g_renderer) {
      InitializeCriticalSection(&g_renderer->lock);
      initialize_base(&g_renderer->client.base, sizeof(g_renderer->client));
      initialize_base(&g_renderer->life_span.base, sizeof(g_renderer->life_span));
      initialize_base(&g_renderer->load.base, sizeof(g_renderer->load));
      initialize_base(&g_renderer->render.base, sizeof(g_renderer->render));
      g_renderer->client.get_life_span_handler = get_life_span_handler;
      g_renderer->client.get_load_handler = get_load_handler;
      g_renderer->client.get_render_handler = get_render_handler;
      g_renderer->life_span.on_after_created = on_after_created;
      g_renderer->load.on_load_end = on_load_end;
      g_renderer->render.get_view_rect = get_view_rect;
      g_renderer->render.on_paint = on_paint;
    }
  }

  renderer_t* renderer = g_renderer;
  if (renderer) {
    EnterCriticalSection(&renderer->lock);
    // The test renderer intentionally ignores the URL supplied by the legacy
    // player and always paints the known Chromium-only test destination.
    (void)source;
    StringCchCopyW(renderer->source, _countof(renderer->source), kCefTestUrl);
    renderer->width = kCefViewportWidth;
    renderer->height = kCefViewportHeight;
    LeaveCriticalSection(&renderer->lock);
  }
  LeaveCriticalSection(&g_renderer_lock);

  if (!renderer)
    return;
  if (!InterlockedCompareExchange(&renderer->initialized, 0, 0) && InterlockedCompareExchange(&renderer->initializing, 1, 0) == 0) {
    // CEF requires cef_initialize to run on the application UI thread. This
    // function is invoked from the game's ATL control creation on that thread;
    // creating a worker thread here prevents CEF from creating its browser.
    initialize_cef(renderer);
  }
}

extern "C" bool WINAPI DrawCefFrame(HDC destination, const RECT* destination_rect) {
  if (!destination || !destination_rect || !g_renderer)
    return false;

  renderer_t* renderer = g_renderer;
  EnterCriticalSection(&renderer->lock);
  const int width = destination_rect->right - destination_rect->left;
  const int height = destination_rect->bottom - destination_rect->top;
  if (!renderer->frame || width <= 0 || height <= 0) {
    LeaveCriticalSection(&renderer->lock);
    return false;
  }

  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = renderer->width;
  info.bmiHeader.biHeight = -renderer->height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  const int result = StretchDIBits(destination, destination_rect->left, destination_rect->top, width, height, 0, 0, renderer->width, renderer->height, renderer->frame, &info, DIB_RGB_COLORS, SRCCOPY);
  LeaveCriticalSection(&renderer->lock);
  return result != GDI_ERROR;
}

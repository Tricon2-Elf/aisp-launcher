// The synthesized document; see document.h.
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <oleidl.h>
#include <mshtml.h>
#include <strsafe.h>

#include <cstddef>
#include <cstring>
#include <string>

#include "document.h"
#include "browser.h"

namespace aisp
{
namespace
{
// An E_NOTIMPL stub with the signature of the slot it fills, so the callee-pops calling
// convention stays balanced for slots the hook does not implement.
template <typename F>
struct Stub;
template <typename R, typename... A>
struct Stub<R(STDMETHODCALLTYPE*)(A...)>
{
    static R STDMETHODCALLTYPE fn(A...) { return static_cast<R>(E_NOTIMPL); }
};
#include "fake_vtables.inc"

const GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kIidDispatch = {0x00020400, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kIidHtmlDocument = {0x626FC520, 0xA41E, 0x11CF, {0xA7, 0x31, 0x00, 0xA0, 0xC9, 0x08, 0x26, 0x37}};
const GUID kIidHtmlDocument2 = {0x332C4425, 0x26CB, 0x11D0, {0xB4, 0x83, 0x00, 0xC0, 0x4F, 0xD9, 0x01, 0x19}};
const GUID kIidHtmlDocument3 = {0x3050F485, 0x98B5, 0x11CF, {0xBB, 0x82, 0x00, 0xAA, 0x00, 0xBD, 0xCE, 0x0B}};
const GUID kIidHtmlWindow2 = {0x332C4427, 0x26CB, 0x11D0, {0xB4, 0x83, 0x00, 0xC0, 0x4F, 0xD9, 0x01, 0x19}};
const GUID kIidHtmlElement = {0x3050F1FF, 0x98B5, 0x11CF, {0xBB, 0x82, 0x00, 0xAA, 0x00, 0xBD, 0xCE, 0x0B}};
const GUID kIidViewObject = {0x0000010D, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kIidViewObject2 = {0x00000127, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

struct Document;

struct Window
{
    IHTMLWindow2Vtbl* lpVtbl;
    LONG refs;
    Document* document; // null once the document is gone
};

struct Document
{
    IHTMLDocument2Vtbl* doc2; // also IUnknown, IDispatch, IHTMLDocument
    IHTMLDocument3Vtbl* doc3;
    IViewObject2Vtbl* view;   // also IViewObject
    LONG refs;
    ScreenStream* stream;     // null once the stream let go
    Window* window;
    Document* next;
};

struct Element
{
    IHTMLElementVtbl* lpVtbl;
    LONG refs;
    std::wstring id;
    std::wstring inner; // Electron's innerHTML at the time of getElementById
    long left = 0, top = 0, width = 0, height = 0; // its offsetLeft/Top/Width/Height then
};

IHTMLDocument2Vtbl g_document2Vtbl;
IHTMLDocument3Vtbl g_document3Vtbl;
IViewObject2Vtbl g_viewVtbl;
IHTMLWindow2Vtbl g_windowVtbl;
IHTMLElementVtbl g_elementVtbl;
bool g_vtablesReady = false;

CRITICAL_SECTION g_documentsLock;
bool g_documentsLockReady = false;
Document* g_documents = nullptr;

Document* DocumentOf2(IHTMLDocument2* self) { return reinterpret_cast<Document*>(reinterpret_cast<BYTE*>(self) - offsetof(Document, doc2)); }
Document* DocumentOf3(IHTMLDocument3* self) { return reinterpret_cast<Document*>(reinterpret_cast<BYTE*>(self) - offsetof(Document, doc3)); }
Document* DocumentOfView(IViewObject2* self) { return reinterpret_cast<Document*>(reinterpret_cast<BYTE*>(self) - offsetof(Document, view)); }

BSTR Bstr(const wchar_t* text) { return SysAllocString(text ? text : L""); }

// --- Document ------------------------------------------------------------------------------------

HRESULT DocumentQuery(Document* doc, REFIID iid, void** out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (IsEqualGUID(iid, kIidUnknown) || IsEqualGUID(iid, kIidDispatch) || IsEqualGUID(iid, kIidHtmlDocument) || IsEqualGUID(iid, kIidHtmlDocument2))
        *out = &doc->doc2;
    else if (IsEqualGUID(iid, kIidHtmlDocument3))
        *out = &doc->doc3;
    else if (IsEqualGUID(iid, kIidViewObject) || IsEqualGUID(iid, kIidViewObject2))
        *out = &doc->view;
    else
        return E_NOINTERFACE;
    InterlockedIncrement(&doc->refs);
    return S_OK;
}

ULONG DocumentAddRef(Document* doc) { return static_cast<ULONG>(InterlockedIncrement(&doc->refs)); }

void ReleaseWindow(Window* window);

ULONG DocumentRelease(Document* doc)
{
    const LONG refs = InterlockedDecrement(&doc->refs);
    if (refs == 0)
    {
        EnterCriticalSection(&g_documentsLock);
        for (Document** link = &g_documents; *link; link = &(*link)->next)
        {
            if (*link == doc)
            {
                *link = doc->next;
                break;
            }
        }
        LeaveCriticalSection(&g_documentsLock);
        if (doc->window)
        {
            doc->window->document = nullptr;
            ReleaseWindow(doc->window);
        }
        delete doc;
    }
    return static_cast<ULONG>(refs < 0 ? 0 : refs);
}

HRESULT STDMETHODCALLTYPE Doc2_QueryInterface(IHTMLDocument2* self, REFIID iid, void** out) { return DocumentQuery(DocumentOf2(self), iid, out); }
ULONG STDMETHODCALLTYPE Doc2_AddRef(IHTMLDocument2* self) { return DocumentAddRef(DocumentOf2(self)); }
ULONG STDMETHODCALLTYPE Doc2_Release(IHTMLDocument2* self) { return DocumentRelease(DocumentOf2(self)); }
HRESULT STDMETHODCALLTYPE Doc2_GetTypeInfoCount(IHTMLDocument2*, UINT* count)
{
    if (count)
        *count = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Doc2_get_Script(IHTMLDocument2* self, IDispatch** out)
{
    if (!out)
        return E_POINTER;
    Document* doc = DocumentOf2(self);
    InterlockedIncrement(&doc->window->refs);
    *out = reinterpret_cast<IDispatch*>(doc->window);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Doc2_get_parentWindow(IHTMLDocument2* self, IHTMLWindow2** out)
{
    if (!out)
        return E_POINTER;
    Document* doc = DocumentOf2(self);
    InterlockedIncrement(&doc->window->refs);
    *out = reinterpret_cast<IHTMLWindow2*>(doc->window);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Doc2_get_readyState(IHTMLDocument2*, BSTR* out)
{
    if (!out)
        return E_POINTER;
    *out = Bstr(L"complete");
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Doc2_get_title(IHTMLDocument2* self, BSTR* out)
{
    if (!out)
        return E_POINTER;
    Document* doc = DocumentOf2(self);
    wchar_t title[1024] = {};
    if (doc->stream)
    {
        EnterCriticalSection(&doc->stream->lock);
        StringCchCopyW(title, 1024, doc->stream->electronTitle);
        LeaveCriticalSection(&doc->stream->lock);
    }
    *out = Bstr(title);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Doc2_get_URL(IHTMLDocument2* self, BSTR* out)
{
    if (!out)
        return E_POINTER;
    Document* doc = DocumentOf2(self);
    wchar_t url[4096] = {};
    if (doc->stream)
    {
        EnterCriticalSection(&doc->stream->lock);
        StringCchCopyW(url, 4096, doc->stream->pageUrl);
        LeaveCriticalSection(&doc->stream->lock);
    }
    *out = Bstr(url);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Doc3_QueryInterface(IHTMLDocument3* self, REFIID iid, void** out) { return DocumentQuery(DocumentOf3(self), iid, out); }
ULONG STDMETHODCALLTYPE Doc3_AddRef(IHTMLDocument3* self) { return DocumentAddRef(DocumentOf3(self)); }
ULONG STDMETHODCALLTYPE Doc3_Release(IHTMLDocument3* self) { return DocumentRelease(DocumentOf3(self)); }

Element* NewElement(const wchar_t* id, const wchar_t* inner);

// The client's reads: statusForm and the retX elements its getter scripts create. One round trip
// to Electron for the element's innerHTML now, which the returned element then hands back from
// get_innerHTML. An id the page does not have is null, as it would be. While Electron is not up
// yet, or its page is still loading (the host says so at once rather than holding the call until
// the load ends), statusForm says the page is loading, which the client waits on; anything else
// is null.
HRESULT STDMETHODCALLTYPE Doc3_getElementById(IHTMLDocument3* self, BSTR id, IHTMLElement** out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (!id)
        return S_OK;
    Document* doc = DocumentOf3(self);
    ScreenStream* stream = doc->stream;
    if (!stream)
        return S_OK;

    char idUtf8[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, id, -1, idUtf8, sizeof(idUtf8), nullptr, nullptr);
    std::string idJson = "\"";
    for (const char* c = idUtf8; *c; ++c)
    {
        if (*c == '"' || *c == '\\')
            idJson += '\\';
        idJson += *c;
    }
    idJson += '"';
    // One round trip brings the element's offsets along with its innerHTML: the client's live
    // player (mode 2) lays its viewport out from the container's offsets and the navi bar's
    // height before it draws at all, and reads them right after getElementById.
    const std::string script = "(function(){var e=document.getElementById(" + idJson + ");return e?e.offsetLeft+','+e.offsetTop+','+e.offsetWidth+','+e.offsetHeight+';'+e.innerHTML:null})()";
    char valueUtf8[16384] = {};
    LARGE_INTEGER frequency = {}, before = {}, after = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&before);
    bool loading = false;
    bool have = CallPrimary(stream, script.c_str(), valueUtf8, sizeof(valueUtf8), 250, &loading);
    QueryPerformanceCounter(&after);
    if (g_logStats)
        CountPrimaryRead(stream, id, (after.QuadPart - before.QuadPart) * 1000.0 / frequency.QuadPart, have || loading);
    if (!have && std::wcscmp(id, L"statusForm") == 0 && (loading || !stream->primaryActive))
    {
        StringCchCopyA(valueUtf8, sizeof(valueUtf8), "0,0,0,0;value=load (statusForm)");
        have = true;
    }
    if (!have)
        return S_OK;
    long box[4] = {};
    const char* html = valueUtf8;
    if (const char* semicolon = std::strchr(valueUtf8, ';'))
    {
        if (sscanf(valueUtf8, "%ld,%ld,%ld,%ld", &box[0], &box[1], &box[2], &box[3]) == 4)
            html = semicolon + 1;
    }
    wchar_t value[16384] = {};
    MultiByteToWideChar(CP_UTF8, 0, html, -1, value, 16384);
    Element* element = NewElement(id, value);
    element->left = box[0];
    element->top = box[1];
    element->width = box[2];
    element->height = box[3];
    *out = reinterpret_cast<IHTMLElement*>(element);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE View_QueryInterface(IViewObject2* self, REFIID iid, void** out) { return DocumentQuery(DocumentOfView(self), iid, out); }
ULONG STDMETHODCALLTYPE View_AddRef(IViewObject2* self) { return DocumentAddRef(DocumentOfView(self)); }
ULONG STDMETHODCALLTYPE View_Release(IViewObject2* self) { return DocumentRelease(DocumentOfView(self)); }

// What the client's OleDraw reaches when the hook's import hook let it through: the crop is
// black until Electron has a frame, exactly what the hook paints when a page is not there yet.
HRESULT STDMETHODCALLTYPE View_Draw(IViewObject2*, DWORD, LONG, void*, DVTARGETDEVICE*, HDC, HDC hdcDraw, LPCRECTL bounds, LPCRECTL, BOOL(STDMETHODCALLTYPE*)(ULONG_PTR), ULONG_PTR)
{
    if (hdcDraw && bounds)
    {
        RECT rect = {bounds->left, bounds->top, bounds->right, bounds->bottom};
        FillRect(hdcDraw, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE View_GetExtent(IViewObject2* self, DWORD, LONG, DVTARGETDEVICE*, LPSIZEL size)
{
    if (!size)
        return E_POINTER;
    Document* doc = DocumentOfView(self);
    size->cx = doc->stream ? doc->stream->pageViewWidth : 0;
    size->cy = doc->stream ? doc->stream->pageViewHeight : 0;
    return S_OK;
}

// --- Window ------------------------------------------------------------------------------------

void ReleaseWindow(Window* window)
{
    if (InterlockedDecrement(&window->refs) == 0)
        delete window;
}

HRESULT STDMETHODCALLTYPE Win_QueryInterface(IHTMLWindow2* self, REFIID iid, void** out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (!(IsEqualGUID(iid, kIidUnknown) || IsEqualGUID(iid, kIidDispatch) || IsEqualGUID(iid, kIidHtmlWindow2)))
        return E_NOINTERFACE;
    InterlockedIncrement(&reinterpret_cast<Window*>(self)->refs);
    *out = self;
    return S_OK;
}
ULONG STDMETHODCALLTYPE Win_AddRef(IHTMLWindow2* self) { return static_cast<ULONG>(InterlockedIncrement(&reinterpret_cast<Window*>(self)->refs)); }
ULONG STDMETHODCALLTYPE Win_Release(IHTMLWindow2* self)
{
    Window* window = reinterpret_cast<Window*>(self);
    const LONG refs = window->refs - 1;
    ReleaseWindow(window);
    return static_cast<ULONG>(refs < 0 ? 0 : refs);
}
HRESULT STDMETHODCALLTYPE Win_GetTypeInfoCount(IHTMLWindow2*, UINT* count)
{
    if (count)
        *count = 0;
    return S_OK;
}

// The client's scripts (ext_* setters, the retX getter functions and their cleanup) run in
// Electron's page. One way: the client never reads a value back through the return.
HRESULT STDMETHODCALLTYPE Win_execScript(IHTMLWindow2* self, BSTR code, BSTR, VARIANT* ret)
{
    Window* window = reinterpret_cast<Window*>(self);
    if (ret)
        VariantInit(ret);
    if (window->document && window->document->stream && code)
        ForwardScriptToPrimary(window->document->stream, code);
    return S_OK;
}

// The live player scrolls the window by the navi bar's height once its viewport is laid out;
// the page does the same scroll in Electron.
void WindowScroll(IHTMLWindow2* self, const wchar_t* method, long x, long y)
{
    Window* window = reinterpret_cast<Window*>(self);
    if (!window->document || !window->document->stream)
        return;
    wchar_t script[96] = {};
    StringCchPrintfW(script, 96, L"window.%s(%ld,%ld)", method, x, y);
    ForwardScriptToPrimary(window->document->stream, script);
}
HRESULT STDMETHODCALLTYPE Win_scrollBy(IHTMLWindow2* self, long x, long y)
{
    WindowScroll(self, L"scrollBy", x, y);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE Win_scrollTo(IHTMLWindow2* self, long x, long y)
{
    WindowScroll(self, L"scrollTo", x, y);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Win_get_document(IHTMLWindow2* self, IHTMLDocument2** out)
{
    if (!out)
        return E_POINTER;
    Window* window = reinterpret_cast<Window*>(self);
    if (!window->document)
    {
        *out = nullptr;
        return E_FAIL;
    }
    DocumentAddRef(window->document);
    *out = reinterpret_cast<IHTMLDocument2*>(&window->document->doc2);
    return S_OK;
}

// --- Element ------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE El_QueryInterface(IHTMLElement* self, REFIID iid, void** out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (!(IsEqualGUID(iid, kIidUnknown) || IsEqualGUID(iid, kIidDispatch) || IsEqualGUID(iid, kIidHtmlElement)))
        return E_NOINTERFACE;
    InterlockedIncrement(&reinterpret_cast<Element*>(self)->refs);
    *out = self;
    return S_OK;
}
ULONG STDMETHODCALLTYPE El_AddRef(IHTMLElement* self) { return static_cast<ULONG>(InterlockedIncrement(&reinterpret_cast<Element*>(self)->refs)); }
ULONG STDMETHODCALLTYPE El_Release(IHTMLElement* self)
{
    Element* element = reinterpret_cast<Element*>(self);
    const LONG refs = InterlockedDecrement(&element->refs);
    if (refs == 0)
        delete element;
    return static_cast<ULONG>(refs < 0 ? 0 : refs);
}
HRESULT STDMETHODCALLTYPE El_GetTypeInfoCount(IHTMLElement*, UINT* count)
{
    if (count)
        *count = 0;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE El_get_innerHTML(IHTMLElement* self, BSTR* out)
{
    if (!out)
        return E_POINTER;
    *out = Bstr(reinterpret_cast<Element*>(self)->inner.c_str());
    return S_OK;
}
HRESULT STDMETHODCALLTYPE El_get_id(IHTMLElement* self, BSTR* out)
{
    if (!out)
        return E_POINTER;
    *out = Bstr(reinterpret_cast<Element*>(self)->id.c_str());
    return S_OK;
}
HRESULT STDMETHODCALLTYPE El_get_tagName(IHTMLElement*, BSTR* out)
{
    if (!out)
        return E_POINTER;
    *out = Bstr(L"DIV");
    return S_OK;
}
HRESULT STDMETHODCALLTYPE El_get_offsetLeft(IHTMLElement* self, long* out)
{
    if (!out)
        return E_POINTER;
    *out = reinterpret_cast<Element*>(self)->left;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE El_get_offsetTop(IHTMLElement* self, long* out)
{
    if (!out)
        return E_POINTER;
    *out = reinterpret_cast<Element*>(self)->top;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE El_get_offsetWidth(IHTMLElement* self, long* out)
{
    if (!out)
        return E_POINTER;
    *out = reinterpret_cast<Element*>(self)->width;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE El_get_offsetHeight(IHTMLElement* self, long* out)
{
    if (!out)
        return E_POINTER;
    *out = reinterpret_cast<Element*>(self)->height;
    return S_OK;
}

Element* NewElement(const wchar_t* id, const wchar_t* inner)
{
    Element* element = new Element();
    element->lpVtbl = &g_elementVtbl;
    element->refs = 1;
    element->id = id ? id : L"";
    element->inner = inner ? inner : L"";
    return element;
}

void InitVtables()
{
    if (g_vtablesReady)
        return;
    FillStubs(g_document2Vtbl);
    g_document2Vtbl.QueryInterface = Doc2_QueryInterface;
    g_document2Vtbl.AddRef = Doc2_AddRef;
    g_document2Vtbl.Release = Doc2_Release;
    g_document2Vtbl.GetTypeInfoCount = Doc2_GetTypeInfoCount;
    g_document2Vtbl.get_Script = Doc2_get_Script;
    g_document2Vtbl.get_parentWindow = Doc2_get_parentWindow;
    g_document2Vtbl.get_readyState = Doc2_get_readyState;
    g_document2Vtbl.get_title = Doc2_get_title;
    g_document2Vtbl.get_URL = Doc2_get_URL;

    FillStubs(g_document3Vtbl);
    g_document3Vtbl.QueryInterface = Doc3_QueryInterface;
    g_document3Vtbl.AddRef = Doc3_AddRef;
    g_document3Vtbl.Release = Doc3_Release;
    g_document3Vtbl.GetTypeInfoCount = reinterpret_cast<decltype(g_document3Vtbl.GetTypeInfoCount)>(Doc2_GetTypeInfoCount);
    g_document3Vtbl.getElementById = Doc3_getElementById;

    FillStubs(g_viewVtbl);
    g_viewVtbl.QueryInterface = View_QueryInterface;
    g_viewVtbl.AddRef = View_AddRef;
    g_viewVtbl.Release = View_Release;
    g_viewVtbl.Draw = View_Draw;
    g_viewVtbl.GetExtent = View_GetExtent;

    FillStubs(g_windowVtbl);
    g_windowVtbl.QueryInterface = Win_QueryInterface;
    g_windowVtbl.AddRef = Win_AddRef;
    g_windowVtbl.Release = Win_Release;
    g_windowVtbl.GetTypeInfoCount = Win_GetTypeInfoCount;
    g_windowVtbl.execScript = Win_execScript;
    g_windowVtbl.get_document = Win_get_document;
    g_windowVtbl.scrollBy = Win_scrollBy;
    g_windowVtbl.scrollTo = Win_scrollTo;

    FillStubs(g_elementVtbl);
    g_elementVtbl.QueryInterface = El_QueryInterface;
    g_elementVtbl.AddRef = El_AddRef;
    g_elementVtbl.Release = El_Release;
    g_elementVtbl.GetTypeInfoCount = El_GetTypeInfoCount;
    g_elementVtbl.get_innerHTML = El_get_innerHTML;
    g_elementVtbl.get_innerText = El_get_innerHTML;
    g_elementVtbl.get_id = El_get_id;
    g_elementVtbl.get_tagName = El_get_tagName;
    g_elementVtbl.get_offsetLeft = El_get_offsetLeft;
    g_elementVtbl.get_offsetTop = El_get_offsetTop;
    g_elementVtbl.get_offsetWidth = El_get_offsetWidth;
    g_elementVtbl.get_offsetHeight = El_get_offsetHeight;

    if (!g_documentsLockReady)
    {
        InitializeCriticalSection(&g_documentsLock);
        g_documentsLockReady = true;
    }
    g_vtablesReady = true;
}
} // namespace

IUnknown* SynthesizedDocument(ScreenStream* stream)
{
    if (!stream)
        return nullptr;
    InitVtables();
    Document* doc = static_cast<Document*>(stream->syntheticDocument);
    if (!doc)
    {
        doc = new Document();
        doc->doc2 = &g_document2Vtbl;
        doc->doc3 = &g_document3Vtbl;
        doc->view = &g_viewVtbl;
        doc->refs = 1; // the stream's own
        doc->stream = stream;
        doc->window = new Window();
        doc->window->lpVtbl = &g_windowVtbl;
        doc->window->refs = 1; // the document's
        doc->window->document = doc;
        EnterCriticalSection(&g_documentsLock);
        doc->next = g_documents;
        g_documents = doc;
        LeaveCriticalSection(&g_documentsLock);
        stream->syntheticDocument = doc;
    }
    DocumentAddRef(doc);
    return reinterpret_cast<IUnknown*>(&doc->doc2);
}

ScreenStream* StreamOfSynthesizedDocument(IUnknown* object)
{
    if (!object || !g_documentsLockReady)
        return nullptr;
    ScreenStream* found = nullptr;
    EnterCriticalSection(&g_documentsLock);
    for (Document* doc = g_documents; doc && !found; doc = doc->next)
    {
        const void* p = object;
        if (p == &doc->doc2 || p == &doc->doc3 || p == &doc->view)
            found = doc->stream;
    }
    LeaveCriticalSection(&g_documentsLock);
    return found;
}

void ReleaseSynthesizedDocument(ScreenStream* stream)
{
    Document* doc = stream ? static_cast<Document*>(stream->syntheticDocument) : nullptr;
    if (!doc)
        return;
    stream->syntheticDocument = nullptr;
    doc->stream = nullptr;
    DocumentRelease(doc);
}
} // namespace aisp

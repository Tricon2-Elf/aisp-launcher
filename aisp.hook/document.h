// The synthesized document: when Electron is the primary browser, the WebBrowser control the
// client hosts is never navigated, and what the client gets from IWebBrowser2::get_Document is
// an object of the hook's own. It implements the little the client uses of a page:
//
//   IHTMLDocument2   get_Script and get_parentWindow (the window below), get_readyState
//                    ("complete"), get_title, get_URL; the rest answers E_NOTIMPL
//   IHTMLDocument3   getElementById: an element of the hook's own, or null when Electron's
//                    page has no such id
//   IHTMLWindow2     execScript: the client's script runs in Electron's page, one way;
//                    scrollBy / scrollTo, the same way (the live player's viewport scroll)
//   IHTMLElement     get_innerHTML / get_innerText and get_offsetLeft/Top/Width/Height: one
//                    round trip to Electron at getElementById brings them all (the live player
//                    lays its viewport out from the container's offsets before it draws);
//                    get_id, get_tagName
//   IViewObject2     Draw: the client's OleDraw lands in the hook's import hook first and paints
//                    Electron's frame; what falls through to this Draw fills the rectangle black
//
// Every other slot of those interfaces is an E_NOTIMPL stub of the right signature (generated,
// fake_vtables.inc). No mshtml, no Gecko: on Windows and Wine alike the page lives in Electron
// and IE is a host window that shows nothing.
#pragma once

#include "screen.h"

namespace aisp
{
// A document for the stream, one reference for the caller. The same object is returned for the
// stream's lifetime (AddRef'd); it is released by the client through Release.
IUnknown* SynthesizedDocument(ScreenStream* stream);
// The stream behind a synthesized document, or nullptr for any other object. No AddRef.
ScreenStream* StreamOfSynthesizedDocument(IUnknown* object);
// Drops the stream's document (the stream is going away); clients still holding it keep a
// working but streamless object.
void ReleaseSynthesizedDocument(ScreenStream* stream);
} // namespace aisp

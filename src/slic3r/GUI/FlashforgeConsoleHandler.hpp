#ifndef slic3r_FlashforgeConsoleHandler_hpp_
#define slic3r_FlashforgeConsoleHandler_hpp_

#include <memory>
#include <string>
#include <wx/string.h>
#include <nlohmann/json_fwd.hpp>

namespace Slic3r {
namespace GUI {

class PrinterWebView;
class PrinterWebViewHandler;

// The Device tab for a Flashforge printer is our own page (resources/web/flashforge/index.html)
// rather than the Bambu-shaped MonitorPanel, which has no vocabulary for a four-slot material
// station and shows Bambu branding for a machine that is not one.
//
// The page and the slicer talk over the wxWebView script-message channel, not HTTP: the page posts
//     {"id": <n>, "method": "status", "params": {}}
// through window.wx.postMessage, and this handler answers by evaluating
//     window.orcaFlashforge.receive({"id": <n>, "method": "status", "ok": true, "result": {...}})
// back into the document. `id` 0 marks an unsolicited push, which is how the poller delivers every
// later snapshot. Nothing here opens a socket the page could reach directly, so there is no token,
// no CORS and no dependency on the MCP server.
//
// The page's second method is `command`:
//     {"id": <n>, "method": "command", "params": {"name": "light", "on": true}}
// answered with {"id": <n>, "method": "command", "ok": true} or ok:false plus the printer's own
// message. Every command runs on a worker thread - a control request is the same blocking HTTP
// call the poller makes - and a successful one wakes the poller so the page settles on what the
// printer reports rather than on what the click hoped for.

/// file:// URL of the bundled console page.
wxString flashforge_console_url();

/// Maps one `command` from the page onto the concrete printer call the worker thread makes. Pure:
/// no GUI, no network, no printer, so the whole control surface is testable without one.
///
/// `params` is the page's `{"name": ..., ...}`. `snapshot` is the last status the poller cached,
/// and is where the fields a control is *not* changing come from: `printerCtl_cmd` and
/// `circulateCtl_cmd` each carry every field they own, so sending one field alone would silently
/// reset the others.
///
/// `operation` comes back as one of
///     {"kind":"light",       "on": <bool>}
///     {"kind":"job",         "action": "pause"|"resume"|"stop"}
///     {"kind":"temperature", "bed": <number|null>, "chamber": <number|null>,
///                            "nozzles": [<number|null> x 4]}   // null = leave alone, 0 = off
///     {"kind":"control",     "cmd": <printer command>, "args": {...}}
///
/// Returns false with `error` set for an unknown or out-of-range request, or for one whose
/// untouched fields cannot be filled in because no status has arrived yet.
bool build_console_operation(const nlohmann::json& params,
                             const nlohmann::json& snapshot,
                             nlohmann::json&       operation,
                             std::string&          error);

/// The script-message handler that answers `status` and then keeps pushing. Polls on its own worker
/// thread (the local API must never be called from the GUI thread) and delivers through CallAfter.
std::unique_ptr<PrinterWebViewHandler> make_flashforge_console_handler(PrinterWebView& owner);

} // namespace GUI
} // namespace Slic3r

#endif /* slic3r_FlashforgeConsoleHandler_hpp_ */

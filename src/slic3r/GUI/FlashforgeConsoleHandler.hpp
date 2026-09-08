#ifndef slic3r_FlashforgeConsoleHandler_hpp_
#define slic3r_FlashforgeConsoleHandler_hpp_

#include <memory>
#include <wx/string.h>

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

/// file:// URL of the bundled console page.
wxString flashforge_console_url();

/// The script-message handler that answers `status` and then keeps pushing. Polls on its own worker
/// thread (the local API must never be called from the GUI thread) and delivers through CallAfter.
std::unique_ptr<PrinterWebViewHandler> make_flashforge_console_handler(PrinterWebView& owner);

} // namespace GUI
} // namespace Slic3r

#endif /* slic3r_FlashforgeConsoleHandler_hpp_ */

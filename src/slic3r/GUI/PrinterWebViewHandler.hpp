#ifndef slic3r_PrinterWebViewHandler_hpp_
#define slic3r_PrinterWebViewHandler_hpp_

#include <memory>
#include <wx/webview.h>
#include <wx/string.h>

class wxWebView;

namespace Slic3r {
namespace GUI {

class PrinterWebView;

class PrinterWebViewHandler {
public:
    explicit PrinterWebViewHandler(PrinterWebView& owner);
    virtual ~PrinterWebViewHandler();

    virtual void on_loaded(wxWebViewEvent &evt);
    virtual void on_script_message(wxWebViewEvent &evt);
    /// The page has been taken off the tab bar. A handler that keeps background work going (polling
    /// a printer, say) stops it here rather than running for the life of the app.
    virtual void on_suspended();

protected:
    PrinterWebView& owner() const;
    wxWebView*      browser() const;

private:
    PrinterWebView& m_owner;
};

std::unique_ptr<PrinterWebViewHandler> create_printer_webview_handler(PrinterWebView& owner);

} // GUI
} // Slic3r

#endif /* slic3r_PrinterWebViewHandler_hpp_ */
#ifndef slic3r_GUI_SendDialogRouting_hpp_
#define slic3r_GUI_SendDialogRouting_hpp_

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace GUI {

// Which dialog the Print button opens for the selected printer.
enum class SendDialogKind
{
    // SelectMachineDialog: Bambu's device dialog, reached through a printer agent.
    MachineSelect,
    // The printer's own PrintHostSendDialog (or the subclass registered for its host type).
    PrintHost,
};

// Pure. `use_bbl_network` is PresetBundle::use_bbl_network(), `use_printer_agents` the app
// preference of the same name, `host_type` the selected printer's `host_type` option.
SendDialogKind choose_send_dialog(bool use_bbl_network, bool use_printer_agents, PrintHostType host_type);

// The same three inputs, read from the running app, so every Print entry point asks the question the
// same way. `choose_send_dialog` above stays pure and is what the tests exercise.
SendDialogKind choose_send_dialog_for_current_printer();

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_SendDialogRouting_hpp_

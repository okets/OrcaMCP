#include "SendDialogRouting.hpp"

#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace Slic3r {
namespace GUI {

// Whether this host type's own send dialog maps filaments better than SelectMachineDialog does.
//
// SelectMachineDialog models every multi-nozzle printer as a two-nozzle IDEX: it splits the plate's
// filaments into a left and a right bucket by `filament_map`, and each bucket only sees AMS units
// bound to that nozzle. A Flashforge with a material station publishes one unit bound to the main
// nozzle, so anything the project assigns to the left nozzle has nothing to map against and Send is
// refused -- whatever is actually loaded. FlashforgePrintHostSendDialog has no nozzle count in it at
// all: it builds one mapping card per project filament and matches against the real slots.
//
// Only Flashforge for now. The other agent-backed hosts are not known to hit this, and quietly
// moving their Send flow is a change nobody asked for.
static bool host_has_own_send_dialog(PrintHostType host_type)
{
    switch (host_type) {
    case htFlashforge: return true;
    default: return false;
    }
}

SendDialogKind choose_send_dialog(bool use_bbl_network, bool use_printer_agents, PrintHostType host_type)
{
    if (use_bbl_network)
        return SendDialogKind::MachineSelect;
    if (use_printer_agents && !host_has_own_send_dialog(host_type))
        return SendDialogKind::MachineSelect;
    return SendDialogKind::PrintHost;
}

SendDialogKind choose_send_dialog_for_current_printer()
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    // A physical printer's connection settings are merged into the edited printer preset, which is
    // where Plater::priv::send_gcode_legacy reads the host type from as well.
    const auto* host_opt = bundle.printers.get_edited_preset().config.option<ConfigOptionEnum<PrintHostType>>("host_type");
    return choose_send_dialog(bundle.use_bbl_network(),
                              wxGetApp().app_config->get_bool("use_printer_agents"),
                              host_opt != nullptr ? host_opt->value : htPrusaLink);
}

} // namespace GUI
} // namespace Slic3r

#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/SendDialogRouting.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

// Why this exists: on 2026-09-20 the Print button sent a Flashforge Creator 5 Pro to
// SelectMachineDialog, which models every multi-nozzle printer as a two-nozzle IDEX. The C5P is a
// four-tool machine, so its filaments were split into a left and a right bucket, the material
// station (which registers on the main nozzle) was invisible to the left bucket, and every plate
// came back "Not all filaments used in slicing are mapped to the printer". The printer agents
// preference is what routed it there: it is meant to give third-party printers the Device tab, not
// to take away their own send dialog.

TEST_CASE("Bambu network always keeps the machine-select dialog", "[SendDialogRouting]")
{
    for (bool agents : {false, true})
        for (PrintHostType host : {htPrusaLink, htFlashforge, htMoonraker})
            CHECK(choose_send_dialog(/*bbl_network=*/true, agents, host) == SendDialogKind::MachineSelect);
}

TEST_CASE("a Flashforge keeps its own send dialog even with printer agents on", "[SendDialogRouting]")
{
    CHECK(choose_send_dialog(/*bbl_network=*/false, /*agents=*/true, htFlashforge) == SendDialogKind::PrintHost);
}

TEST_CASE("printer agents still route other hosts to the machine-select dialog", "[SendDialogRouting]")
{
    for (PrintHostType host : {htPrusaLink, htMoonraker, htOctoPrint, htCrealityPrint, htElegooLink})
        CHECK(choose_send_dialog(/*bbl_network=*/false, /*agents=*/true, host) == SendDialogKind::MachineSelect);
}

TEST_CASE("without printer agents every non-Bambu printer uses its print-host dialog", "[SendDialogRouting]")
{
    for (PrintHostType host : {htPrusaLink, htFlashforge, htMoonraker, htOctoPrint})
        CHECK(choose_send_dialog(/*bbl_network=*/false, /*agents=*/false, host) == SendDialogKind::PrintHost);
}

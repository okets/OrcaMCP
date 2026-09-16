// The seam between a polling agent's synthesized push_status payload and the parser that consumes
// it. Both the Flashforge and the Moonraker agent print from storage that is always present, and
// both used to say so by poking DevStorage after the fact -- which never survived, because
// DevStorage::ParseV1_0 reads a *missing* `sdcard` key as positive proof of no card. The Send print
// job dialog then refused with "Storage needs to be inserted before printing." These cases pin the
// contract that replaced it: the claim rides in the payload, where the parser cannot overwrite it.

// Match the include environment that libslic3r_gui TUs get from pchheader.hpp -- see the same
// preamble in test_dev_mapping.cpp for why the order matters.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/Utils/FlashforgeApi.hpp"

using namespace Slic3r;

TEST_CASE("A Flashforge status payload parses as storage present", "[DevStorage]")
{
    FlashforgeApi::PrinterStatus status;
    status.state = "ready";

    const nlohmann::json payload = FlashforgeApi::flashforge_status_to_bambu_payload(status);

    DevStorage storage(nullptr);
    DevStorage::ParseV1_0(payload["print"], &storage);
    CHECK(storage.get_sdcard_state() == DevStorage::HAS_SDCARD_NORMAL);
}

TEST_CASE("The parser treats a missing sdcard key as no card", "[DevStorage]")
{
    // This is the behaviour that made the out-of-band poke useless: a good state set before the
    // payload arrives is reset by the very next poll. Pinned so the agents keep sending the key
    // rather than reaching for DevStorage again.
    DevStorage storage(nullptr);
    storage.set_sdcard_state(DevStorage::HAS_SDCARD_NORMAL);
    REQUIRE(storage.get_sdcard_state() == DevStorage::HAS_SDCARD_NORMAL);

    DevStorage::ParseV1_0(nlohmann::json::parse(R"({"gcode_state":"IDLE"})"), &storage);
    CHECK(storage.get_sdcard_state() == DevStorage::NO_SDCARD);
}

TEST_CASE("An sdcard key in the payload decides the state either way", "[DevStorage]")
{
    // The shape the Moonraker agent emits for its always-available virtual_sdcard.
    DevStorage present(nullptr);
    DevStorage::ParseV1_0(nlohmann::json::parse(R"({"sdcard":true})"), &present);
    CHECK(present.get_sdcard_state() == DevStorage::HAS_SDCARD_NORMAL);

    DevStorage absent(nullptr);
    absent.set_sdcard_state(DevStorage::HAS_SDCARD_NORMAL);
    DevStorage::ParseV1_0(nlohmann::json::parse(R"({"sdcard":false})"), &absent);
    CHECK(absent.get_sdcard_state() == DevStorage::NO_SDCARD);
}

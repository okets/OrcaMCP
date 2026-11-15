#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/NativeBambuPrintSnapshot.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPBambuPrint.hpp"
using namespace Slic3r::GUI::OrcaMCP;
static BambuPrintFacts ready() {
    BambuPrintFacts f;
    f.slice_valid = f.connected = f.idle = f.storage_ready = f.ams_ready = f.credentials_ready = true;
    f.age_s = 1; f.nozzle_mm = f.profile_nozzle_mm = 0.4;
    f.printer_type = "C12"; f.profile_model = "Bambu Lab P1S"; f.slice_id = "slice-a";
    return f;
}
TEST_CASE("Bambu start refuses every missing native readiness fact", "[McpBambuPrint]") {
    CHECK_FALSE(bambu_print_refusal(ready(), "slice-a"));
    for (auto field : {&BambuPrintFacts::slice_valid, &BambuPrintFacts::connected, &BambuPrintFacts::idle,
        &BambuPrintFacts::storage_ready, &BambuPrintFacts::ams_ready, &BambuPrintFacts::credentials_ready}) {
        auto f = ready(); f.*field = false; CHECK(bambu_print_refusal(f, "slice-a"));
    }
    for (auto field : {&BambuPrintFacts::modal, &BambuPrintFacts::busy}) {
        auto f = ready(); f.*field = true; CHECK(bambu_print_refusal(f, "slice-a"));
    }
    auto f = ready(); f.age_s = 16; CHECK(bambu_print_refusal(f, "slice-a"));
    f = ready(); f.age_s = -1; CHECK(bambu_print_refusal(f, "slice-a"));
    f = ready(); f.nozzle_mm = 0.6; CHECK(bambu_print_refusal(f, "slice-a"));
    f = ready(); f.profile_nozzle_mm = 0.6; CHECK(bambu_print_refusal(f, "slice-a"));
    f = ready(); f.printer_type = "C11"; CHECK(bambu_print_refusal(f, "slice-a"));
    f = ready(); f.profile_model = "Other"; CHECK(bambu_print_refusal(f, "slice-a"));
    CHECK(bambu_print_refusal(ready(), "old-slice"));
}
TEST_CASE("Bambu mapping requires every used project filament and exact native material identity", "[McpBambuPrint]") {
    std::vector<BambuFilament> filaments{{1, "PLA", "GFA01", "AABBCC"}, {3, "PLA", "GFA02", "FFFFFF"}};
    std::vector<BambuMapping> maps{{1, 0, 1, true, true, false, "PLA", "GFA01", "AABBCC"}, {3, 0, 3, true, true, false, "PLA", "GFA02", "FFFFFF"}};
    CHECK_FALSE(bambu_mapping_refusal(filaments, maps));
    auto bad = maps; bad.pop_back(); CHECK(bambu_mapping_refusal(filaments, bad));
    bad = maps; bad.push_back(maps[0]); CHECK(bambu_mapping_refusal(filaments, bad));
    bad = maps; bad[0].project_filament = 2; CHECK(bambu_mapping_refusal(filaments, bad));
    bad = maps; bad[0].slot_id = 4; CHECK(bambu_mapping_refusal(filaments, bad));
    bad = maps; bad[0].ams_id = 4; CHECK(bambu_mapping_refusal(filaments, bad));
    bad = maps; bad[0].type = "PETG"; CHECK(bambu_mapping_refusal(filaments, bad));
    bad = maps; bad[0].profile_id = "other"; CHECK(bambu_mapping_refusal(filaments, bad));
    bad = maps; bad[0].profile_id.clear(); CHECK(bambu_mapping_refusal(filaments, bad));
    bad = maps; bad[0].present = false; CHECK(bambu_mapping_refusal(filaments, bad));
    bad = maps; bad[0].ready = false; CHECK(bambu_mapping_refusal(filaments, bad));
    bad = maps; bad[0].reading = true; CHECK(bambu_mapping_refusal(filaments, bad));
}
TEST_CASE("Bambu submission IDs remain consumed without any retry dispatch", "[McpBambuPrint]") {
    BambuSubmissionLedger ledger;
    CHECK(ledger.reserve("a", "first"));
    CHECK_FALSE(ledger.reserve("a", "first"));
    CHECK_FALSE(ledger.reserve("a", "different"));
    REQUIRE(ledger.find("a")); CHECK(*ledger.find("a") == "first");
    CHECK(ledger.reserve("b", "second")); CHECK_FALSE(ledger.find("absent"));
}

TEST_CASE("Bambu mapping preserves unused project positions and native AMS numbering", "[McpBambuPrint]") {
    std::vector<BambuFilament> filaments{{1, "PLA", "GFA01", "112233FF"}, {3, "PLA", "GFA02", "FFFFFF00"}};
    std::vector<BambuMapping> maps{{1, 0, 1, true, true, false, "PLA", "GFA01", "334455FF"}, {3, 2, 3, true, true, false, "PLA", "GFA02", "556677FF"}};
    filaments[0].nozzle_id = 0;
    auto arrays = bambu_mapping_arrays(4, filaments, maps);
    CHECK(arrays["ams_mapping"] == nlohmann::json::array({1, -1, 11, -1}));
    CHECK(arrays["ams_mapping2"][0]["slot_id"] == 1);
    CHECK(arrays["ams_mapping2"][1]["ams_id"] == 255);
    CHECK(arrays["ams_mapping2"][1]["slot_id"] == 255);
    CHECK(arrays["ams_mapping2"][2]["ams_id"] == 2);
    CHECK(arrays["ams_mapping_info"][0]["filamentId"] == "GFA01");
    CHECK(arrays["ams_mapping_info"][0]["nozzleId"] == 0);
    CHECK(arrays["ams_mapping_info"][0]["sourceColor"] == "112233FF");
    CHECK(arrays["ams_mapping_info"][0]["targetColor"] == "334455FF");
    CHECK(arrays["ams_mapping_info"][3]["ams"] == -1);
}

TEST_CASE("Bambu dry run dispatches nothing and a request can dispatch only once", "[McpBambuPrint]") {
    BambuSubmissionLedger ledger;
    int submitted = 0;
    auto native = [&] { ++submitted; };
    CHECK_FALSE(bambu_dispatch_once(true, "a", "slice", ledger, native));
    CHECK(submitted == 0); CHECK_FALSE(ledger.find("a"));
    CHECK(bambu_dispatch_once(false, "a", "slice", ledger, native));
    CHECK(submitted == 1);
    CHECK_FALSE(bambu_dispatch_once(false, "a", "slice", ledger, native));
    CHECK_FALSE(bambu_dispatch_once(false, "a", "different", ledger, native));
    CHECK(submitted == 1);
    CHECK_THROWS(bambu_dispatch_once(false, "failed", "slice", ledger, [] { throw std::runtime_error("uncertain"); }));
    CHECK_FALSE(bambu_dispatch_once(false, "failed", "slice", ledger, native));
    CHECK(submitted == 1);
}

TEST_CASE("A reconnect delta cannot establish native Bambu snapshot readiness", "[McpBambuPrint]") {
    nlohmann::json raw{{"command", "push_status"}, {"gcode_state", "IDLE"}, {"ams_status", 0},
        {"ams", {{"ams_exist_bits", "1"}, {"tray_exist_bits", "2"}, {"tray_reading_bits", "0"}, {"ams", nlohmann::json::array()}}}};
    CHECK(Slic3r::native_bambu_print_snapshot_complete(raw)); // legacy full LAN packet has no msg field
    raw["msg"] = 0;
    CHECK(Slic3r::native_bambu_print_snapshot_complete(raw));
    raw["msg"] = 1; // a delta can contain every key: it is still not a fresh full snapshot
    CHECK_FALSE(Slic3r::native_bambu_print_snapshot_complete(raw));
    raw["msg"] = 2;
    CHECK_FALSE(Slic3r::native_bambu_print_snapshot_complete(raw));
    raw["msg"] = false;
    CHECK_FALSE(Slic3r::native_bambu_print_snapshot_complete(raw));
    raw["msg"] = 0;
    raw["ams"].erase("tray_reading_bits");
    CHECK_FALSE(Slic3r::native_bambu_print_snapshot_complete(raw));
}

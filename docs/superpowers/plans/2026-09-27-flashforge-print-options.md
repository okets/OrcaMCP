# Flashforge print options (flow calibration, leveling, time-lapse) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every way OrcaMCP starts a print on a Flashforge Creator 5 / 5 Pro can ask for the three switches the printer's own start screen offers (Flow Calibration, Leveling, Timelapse), and an agent gets the facts it needs to decide when calibration is worth its minutes.

**Architecture:** One value type, `FlashforgeApi::PrintOptions`, carries the three switches from every sender (the Flashforge send dialog, `send_to_printer`, `print_printer_file`, the printer agent) to the two requests that start a print (`uploadGcode`'s headers, `printGcode`'s body). The MCP tools take each switch from the agent's explicit boolean or, when it is omitted, from a print-time gate (leveling and flow calibration on for a print estimated at 4 h or more), and report what they sent and why. Every start the printer accepts is recorded per printer in memory, and `get_printer_status` reports the last one, so an agent can skip calibration for a quick repeat.

**Tech Stack:** C++17, nlohmann::json, wxWidgets (send dialog), Catch2 (`tests/slic3rutils`), the OrcaMCP tool registry and its golden file.

**Spec:** No separate spec document. The decisions below were made by the user on 2026-09-27 and are the spec.

## Decisions (the user, 2026-09-27)

- Send **flow calibration, leveling and time-lapse**. Not first-layer inspection: the Creator 5 Pro's start screen does not offer it, so `uploadGcode` keeps sending `firstLayerInspection: false`.
- **Send dialog:** all three start off; the last choice confirmed with Send is remembered.
  *Superseded by the user on 2026-09-29, after the live check: "it should remember the last changes made in the ui
  regardless of the print actually being sent." Each box, the material station's too, is saved the moment it is
  toggled.*
- **MCP:** the agent judges. An explicit boolean always wins. Omitted, leveling and flow calibration follow a **print-time gate: on when the print is estimated at 4 hours or more**, off otherwise and off when there is no estimate. Omitted time-lapse is off.
- The agent needs the facts to judge: the print's estimated time (in the response) and when this app last started a print on that printer and with which options (`get_printer_status`).

## Global Constraints

- **Start only after the parallel session's work (release prompt 06b) is merged into `mcp`.** Then branch `flashforge-print-options` off the latest local `mcp`. One C++ build at a time, in the main checkout, no worktrees (`build/arm64` is 31 GB).
- **Never call `send_to_printer` or `print_printer_file` unless the user is present and has said yes.** On a Flashforge both start a real print. Only Task 5 calls them.
- Before any live MCP call: `lsof -nP -iTCP:13618 -sTCP:LISTEN`. The user's release app and a dev build can both hold the port.
- Test live behavior through the `mcp__orca-slicer__*` tools, never curl (CLAUDE.md).
- The printer's check code is a credential: never echo, log or commit it.
- Any change to a `register_tool` text or schema: regenerate `scripts/orcamcp_tools.json` in the same commit (CLAUDE.md, "Adding New Tools", step 4).
- Pushing to `mcp` starts an hour-long Build all. Push only with the user's word, run `gh run list -R okets/OrcaMCP` first, and put `[skip ci]` on docs-only commits. Always pass `-R okets/OrcaMCP`.
- No tag, no release.
- Docs in American spelling; code comments follow the file they are in.
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

Build and test commands used throughout:

```bash
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "<tags>"
```

## Review Focus

1. **A stored file the printer lists without a usable `printingTime`** (older firmware, a plain name, a string, 0, or not listed at all): `print_printer_file` still starts the print, and the gate reads "unknown" (off). It must not fail. Pinned in Task 1 (`parse_gcode_printing_time` tests).
2. **A print estimated at exactly 4 h, or with no estimate, or an estimate of 0:** 4 h runs calibration; no estimate or 0 runs neither and says `print_time_unknown`. Pinned in Task 3.
3. **A parameter that is not a boolean** (`"flow_calibration": "true"`, `1`, `null`): refused with the parameter's name, nothing sent. Pinned in Task 3.
4. **An upload with `start_print: false`, or a start the printer refused:** must not show up as `last_print_started_here`. The record is written only after the request succeeded and only for a start. Pinned by Task 5, step C (an upload-only send is safe to run).
5. **A user whose config already holds `flashforge_leveling_before_print = 1`:** older builds started with leveling on and saved it on every Send, so most configs hold "1". The dialog keeps showing leveling on, as decided ("previous choice remembered"). Pinned in Task 2.

---

## Before you start

- [ ] Confirm 06b is merged and nothing else is building: `git -C /Users/hanan/Projects/OrcaMCP status` is clean and `git log mcp -1` shows 06b's merge.
- [ ] Branch: `git checkout -b flashforge-print-options mcp`. This plan was written into the working tree
  untracked while 06b ran, so it comes along to the new branch.
- [ ] Commit the plan on the branch:

```bash
git add docs/superpowers/plans/2026-09-27-flashforge-print-options.md
git commit -m "docs: plan for Flashforge print options [skip ci]"
```

---

### Task 1: One set of print options, from every sender to both start requests

Today the three switches travel as loose `extended_info` strings built in three places (the dialog, `send_to_printer`, the printer agent), and flow calibration is hard-coded `false` in both start requests. This task gives them one type and one place where the keys are spelled, and sends flow calibration and time-lapse on both requests. Behavior visible to users does not change yet, apart from `printGcode` now also sending `timeLapseVideo: false`; Task 5 checks the firmware accepts it.

**Files:**
- Modify: `src/slic3r/Utils/FlashforgeApi.hpp` (new `PrintOptions`, `make_upload_extended_info`, `read_upload_print_options`, `parse_gcode_printing_time`; `make_print_gcode_payload` takes `PrintOptions`)
- Modify: `src/slic3r/Utils/FlashforgeApi.cpp`
- Modify: `src/slic3r/Utils/Flashforge.hpp` (`print_gcode_file` signature, new `stored_file_printing_time`, private `fetch_gcode_list`)
- Modify: `src/slic3r/Utils/Flashforge.cpp` (`upload_local_api` headers, `print_gcode_file`, `list_gcode_files`)
- Modify: `src/slic3r/Utils/FlashforgePrinterAgent.cpp` (`build_upload_extended_info`)
- Modify: `src/slic3r/GUI/PrintHostDialogs.cpp` (`FlashforgePrintHostSendDialog::extendedInfo`)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp` (the two call sites, behavior unchanged)
- Modify: `src/slic3r/GUI/Plater.hpp` (the key list in the comment above `send_gcode_direct`)
- Test: `tests/slic3rutils/test_flashforge_api.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces:

```cpp
namespace Slic3r { namespace FlashforgeApi {
struct PrintOptions { bool leveling{false}; bool flow_calibration{false}; bool time_lapse{false}; };
bool operator==(const PrintOptions& a, const PrintOptions& b);
std::map<std::string, std::string> make_upload_extended_info(const PrintOptions& options, bool use_material_station, const nlohmann::json& mappings);
PrintOptions read_upload_print_options(const std::map<std::string, std::string>& extended_info);
nlohmann::json make_print_gcode_payload(const std::string& serial, const std::string& check_code, const std::string& file_name, const PrintOptions& options, const nlohmann::json& material_mappings);
std::optional<long> parse_gcode_printing_time(const nlohmann::json& response, const std::string& file_name);
}}
// class Slic3r::Flashforge
bool print_gcode_file(const std::string& file_name, const FlashforgeApi::PrintOptions& options, const nlohmann::json& material_mappings, wxString& msg) const;
bool stored_file_printing_time(const std::string& file_name, std::optional<long>& seconds, wxString& msg) const;
```

- [ ] **Step 1: Write the failing tests**

In `tests/slic3rutils/test_flashforge_api.cpp`, the `"control payload shapes"` case calls `make_print_gcode_payload` with a `bool` (line 137). Replace these five lines:

```cpp
    auto g = make_print_gcode_payload("SN1", "CC1", "a.gcode", true, nlohmann::json::array({{{"toolId", 0}, {"slotId", 1}}}));
    CHECK(g["fileName"] == "a.gcode");
    CHECK(g["levelingBeforePrint"] == true);
    CHECK(g["useMatlStation"] == true);
    CHECK(g["gcodeToolCnt"] == 1);
```

with nothing (the new case below covers them), and append these cases at the end of the file:

```cpp
// ---------------------------------------------------------------------------
// Print options: the switches the printer's start screen offers, on both the
// upload and printGcode, and the stored file's own estimate for the MCP gate.
// ---------------------------------------------------------------------------

TEST_CASE("printGcode carries all three print options", "[flashforge]") {
    PrintOptions options;
    options.leveling         = true;
    options.flow_calibration = true;
    options.time_lapse       = true;
    const auto g = make_print_gcode_payload("SN1", "CC1", "a.gcode", options, nlohmann::json::array({{{"toolId", 0}, {"slotId", 1}}}));
    CHECK(g["fileName"] == "a.gcode");
    CHECK(g["levelingBeforePrint"] == true);
    CHECK(g["flowCalibration"] == true);
    CHECK(g["timeLapseVideo"] == true);
    CHECK(g["useMatlStation"] == true);
    CHECK(g["gcodeToolCnt"] == 1);

    const auto off = make_print_gcode_payload("SN1", "CC1", "a.gcode", PrintOptions{}, nlohmann::json::array());
    CHECK(off["levelingBeforePrint"] == false);
    CHECK(off["flowCalibration"] == false);
    CHECK(off["timeLapseVideo"] == false);
    CHECK(off["useMatlStation"] == false);
}

TEST_CASE("upload extended_info round-trips the print options", "[flashforge]") {
    PrintOptions options;
    options.flow_calibration = true;
    const nlohmann::json mappings = nlohmann::json::array({{{"toolId", 0}, {"slotId", 2}}, {{"toolId", 1}, {"slotId", 3}}});

    const auto info = make_upload_extended_info(options, true, mappings);
    CHECK(info.at("levelingBeforePrint") == "0");
    CHECK(info.at("flowCalibration") == "1");
    CHECK(info.at("timeLapseVideo") == "0");
    CHECK(info.at("useMatlStation") == "1");
    CHECK(info.at("gcodeToolCnt") == "2");
    CHECK(nlohmann::json::parse(info.at("materialMappings")) == mappings);
    CHECK(read_upload_print_options(info) == options);

    const auto bare = make_upload_extended_info(PrintOptions{}, false, nlohmann::json::array());
    CHECK(bare.at("useMatlStation") == "0");
    CHECK(bare.at("gcodeToolCnt") == "0");
    CHECK(bare.at("materialMappings") == "[]");
}

TEST_CASE("an extended_info without the option keys reads as all off", "[flashforge]") {
    CHECK(read_upload_print_options({}) == PrintOptions{});
    CHECK(read_upload_print_options({{"flowCalibration", "yes"}}) == PrintOptions{}); // only "1" is on
}

TEST_CASE("parse_gcode_printing_time finds a stored file's time", "[flashforge]") {
    const auto response = nlohmann::json::parse(R"({"code":0,
        "gcodeList":["a.gcode","b.gcode","c.gcode","d.gcode"],
        "gcodeListDetail":[{"gcodeFileName":"a.gcode","printingTime":893},
                           {"gcodeFileName":"b.gcode","printingTime":"7200"},
                           {"gcodeFileName":"c.gcode","printingTime":0},
                           {"gcodeFileName":"d.gcode"}]})");
    CHECK(parse_gcode_printing_time(response, "a.gcode") == 893L);
    CHECK(parse_gcode_printing_time(response, "b.gcode") == 7200L);           // the API types integers loosely
    CHECK_FALSE(parse_gcode_printing_time(response, "c.gcode").has_value()); // 0: the printer does not know
    CHECK_FALSE(parse_gcode_printing_time(response, "d.gcode").has_value());
    CHECK_FALSE(parse_gcode_printing_time(response, "e.gcode").has_value()); // not stored
}

TEST_CASE("parse_gcode_printing_time reads a gcodeList object too", "[flashforge]") {
    CHECK(parse_gcode_printing_time(nlohmann::json::parse(R"({"gcodeList":[{"gcodeFileName":"a.gcode","printingTime":60}]})"), "a.gcode") == 60L);
    CHECK_FALSE(parse_gcode_printing_time(nlohmann::json::parse(R"({"gcodeList":["a.gcode"]})"), "a.gcode").has_value());
    CHECK_FALSE(parse_gcode_printing_time(nlohmann::json(), "a.gcode").has_value());
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run the build command. Expected: compile errors in `test_flashforge_api.cpp`, "no type named 'PrintOptions' in namespace 'Slic3r::FlashforgeApi'".

- [ ] **Step 3: Add the API**

In `src/slic3r/Utils/FlashforgeApi.hpp`, add `#include <map>` to the includes, and add above the `make_credentials_payload` declaration:

```cpp
// The switches the printer's own start screen offers (Creator 5 Pro, firmware 1.9.9: Flow
// Calibration, Leveling, Timelapse). The API's fourth, `firstLayerInspection`, is not on that
// screen and is always sent off.
struct PrintOptions
{
    bool leveling{false};
    bool flow_calibration{false};
    bool time_lapse{false};
};
bool operator==(const PrintOptions& a, const PrintOptions& b);

// The PrintHostUpload::extended_info of a Flashforge upload. Every sender builds it here
// (FlashforgePrintHostSendDialog, send_to_printer, FlashforgePrinterAgent) and
// Flashforge::upload_local_api turns it into the request's headers, so the keys are spelled once.
// `mappings` is the {toolId, slotId, ...} array the printer is sent; empty without the station.
std::map<std::string, std::string> make_upload_extended_info(const PrintOptions& options, bool use_material_station, const nlohmann::json& mappings);

// The options an extended_info carries. Only "1" is on; a missing key is off.
PrintOptions read_upload_print_options(const std::map<std::string, std::string>& extended_info);

// A stored file's `printingTime` in seconds, from a gcodeList response: its gcodeListDetail entry,
// else a {"gcodeFileName": ...} object in gcodeList. nullopt when the printer does not give a
// positive one (older firmware, a plain name, 0) or the file is not listed.
std::optional<long> parse_gcode_printing_time(const nlohmann::json& response, const std::string& file_name);
```

and change the `make_print_gcode_payload` declaration to:

```cpp
nlohmann::json make_print_gcode_payload(const std::string& serial, const std::string& check_code, const std::string& file_name, const PrintOptions& options, const nlohmann::json& material_mappings);
```

In `src/slic3r/Utils/FlashforgeApi.cpp`, add inside the anonymous namespace at the top, after `append_gcode_names`:

```cpp
constexpr const char* kLevelingKey        = "levelingBeforePrint";
constexpr const char* kFlowCalibrationKey = "flowCalibration";
constexpr const char* kTimeLapseKey       = "timeLapseVideo";

// `printingTime` of the entry named `file_name` in `list`, when that entry gives a positive one.
std::optional<long> printing_time_in(const nlohmann::json& list, const std::string& file_name)
{
    if (!list.is_array())
        return std::nullopt;
    for (const auto& entry : list) {
        if (!entry.is_object() || get_string(entry, "gcodeFileName") != file_name)
            continue;
        int seconds = 0;
        if (entry.contains("printingTime") && try_parse_json_int(entry["printingTime"], seconds) && seconds > 0)
            return long(seconds);
        return std::nullopt;
    }
    return std::nullopt;
}
```

After `parse_gcode_list`, add:

```cpp
std::optional<long> parse_gcode_printing_time(const nlohmann::json& response, const std::string& file_name)
{
    if (!response.is_object())
        return std::nullopt;
    if (const auto seconds = printing_time_in(response.value("gcodeListDetail", nlohmann::json()), file_name))
        return seconds;
    return printing_time_in(response.value("gcodeList", nlohmann::json()), file_name);
}
```

Replace `make_print_gcode_payload` at the end of the file with:

```cpp
bool operator==(const PrintOptions& a, const PrintOptions& b)
{
    return a.leveling == b.leveling && a.flow_calibration == b.flow_calibration && a.time_lapse == b.time_lapse;
}

std::map<std::string, std::string> make_upload_extended_info(const PrintOptions& options, bool use_material_station, const nlohmann::json& mappings)
{
    const auto   flag       = [](bool on) { return std::string(on ? "1" : "0"); };
    const size_t tool_count = mappings.is_array() ? mappings.size() : 0;
    return {{kLevelingKey, flag(options.leveling)},
            {kFlowCalibrationKey, flag(options.flow_calibration)},
            {kTimeLapseKey, flag(options.time_lapse)},
            {"useMatlStation", flag(use_material_station)},
            {"gcodeToolCnt", std::to_string(tool_count)},
            {"materialMappings", mappings.is_array() ? mappings.dump() : std::string("[]")}};
}

PrintOptions read_upload_print_options(const std::map<std::string, std::string>& extended_info)
{
    const auto on = [&](const char* key) {
        const auto it = extended_info.find(key);
        return it != extended_info.end() && it->second == "1";
    };
    PrintOptions options;
    options.leveling         = on(kLevelingKey);
    options.flow_calibration = on(kFlowCalibrationKey);
    options.time_lapse       = on(kTimeLapseKey);
    return options;
}

nlohmann::json make_print_gcode_payload(const std::string& serial, const std::string& check_code, const std::string& file_name, const PrintOptions& options, const nlohmann::json& material_mappings)
{
    const bool has_mappings = material_mappings.is_array() && !material_mappings.empty();

    nlohmann::json payload = make_credentials_payload(serial, check_code);
    payload["fileName"]          = file_name;
    payload[kLevelingKey]        = options.leveling;
    payload[kFlowCalibrationKey] = options.flow_calibration;
    // Not in the printGcode body docs/printers/flashforge-lan-api.md documents; the upload takes it.
    // The live check in docs/superpowers/plans/2026-09-27-flashforge-print-options.md decides whether
    // this request honours it.
    payload[kTimeLapseKey]       = options.time_lapse;
    payload["useMatlStation"]    = has_mappings;
    payload["gcodeToolCnt"]      = material_mappings.is_array() ? material_mappings.size() : 0;
    payload["materialMappings"]  = material_mappings;
    return payload;
}
```

- [ ] **Step 4: Use it in the transport**

In `src/slic3r/Utils/Flashforge.hpp`, replace the `print_gcode_file` declaration and add one after it:

```cpp
    bool print_gcode_file(const std::string& file_name, const FlashforgeApi::PrintOptions& options, const nlohmann::json& material_mappings, wxString& msg) const;
    // A stored file's estimated print time, from the printer's file list. `seconds` is nullopt when
    // the printer does not report one; false with `msg` when the list could not be read.
    bool stored_file_printing_time(const std::string& file_name, std::optional<long>& seconds, wxString& msg) const;
```

and in its `private:` section, after `require_local_api_credentials`:

```cpp
    // The gcodeList response, parsed. False with `msg` when it could not be fetched or read.
    bool fetch_gcode_list(nlohmann::json& response, wxString& msg) const;
```

In `src/slic3r/Utils/Flashforge.cpp`, replace `list_gcode_files` and `print_gcode_file` with:

```cpp
bool Flashforge::fetch_gcode_list(json& response, wxString& msg) const
{
    if (!require_local_api_credentials(msg))
        return false;

    std::string body;
    if (!request_local_api_json("gcodeList", FlashforgeApi::make_credentials_payload(m_serial_number, m_check_code).dump(), body, msg))
        return false;

    response = json::parse(body, nullptr, false, true);
    if (response.is_discarded()) {
        msg = _(L("Flashforge returned an invalid JSON response."));
        return false;
    }
    return true;
}

bool Flashforge::list_gcode_files(std::vector<std::string>& files, wxString& msg) const
{
    files.clear();

    json response;
    if (!fetch_gcode_list(response, msg))
        return false;

    // Observed shape: {"code":0,"gcodeList":[{"gcodeFileName":"a.gcode", ...}, ...]}. A plain array of
    // strings and a top-level `gcodeListDetail` fallback are also accepted since the exact response
    // shape returned by different firmware versions is not fully documented -- which is exactly why
    // the parsing lives in FlashforgeApi, where it is pure, tested, and skips what it cannot read.
    files = FlashforgeApi::parse_gcode_list(response);
    return true;
}

bool Flashforge::stored_file_printing_time(const std::string& file_name, std::optional<long>& seconds, wxString& msg) const
{
    seconds.reset();
    json response;
    if (!fetch_gcode_list(response, msg))
        return false;
    seconds = FlashforgeApi::parse_gcode_printing_time(response, file_name);
    return true;
}

bool Flashforge::print_gcode_file(const std::string& file_name, const FlashforgeApi::PrintOptions& options, const nlohmann::json& material_mappings, wxString& msg) const
{
    if (!require_local_api_credentials(msg))
        return false;

    std::string body;
    return request_local_api_json("printGcode", FlashforgeApi::make_print_gcode_payload(m_serial_number, m_check_code, file_name, options, material_mappings).dump(), body, msg);
}
```

In `upload_local_api`, replace the two locals

```cpp
    auto        leveling_before_print = upload_data.extended_info["levelingBeforePrint"] == "1";
    auto        time_lapse_video      = upload_data.extended_info["timeLapseVideo"] == "1";
```

with

```cpp
    const FlashforgeApi::PrintOptions options = FlashforgeApi::read_upload_print_options(upload_data.extended_info);
```

and the four header lines with

```cpp
            .header("levelingBeforePrint", options.leveling ? "true" : "false")
            .header("flowCalibration", options.flow_calibration ? "true" : "false")
            .header("firstLayerInspection", "false") // not on the Creator 5 Pro's start screen
            .header("timeLapseVideo", options.time_lapse ? "true" : "false")
```

- [ ] **Step 5: Build every sender's extended_info with the one builder**

`src/slic3r/Utils/FlashforgePrinterAgent.cpp`, `build_upload_extended_info`: replace the comment and the `info` initializer with

```cpp
    // Built by FlashforgeApi::make_upload_extended_info, as the send dialog and send_to_printer
    // build theirs, so every upload path speaks one dialect.
    FlashforgeApi::PrintOptions options;
    options.leveling         = params.task_bed_leveling;
    options.flow_calibration = params.task_flow_cali;
    options.time_lapse       = params.task_record_timelapse;
    std::map<std::string, std::string> info = FlashforgeApi::make_upload_extended_info(options, false, nlohmann::json::array());
```

and replace the three lines at the end (`info["useMatlStation"] = "1";` through `info["materialMappings"] = mappings.dump();`) plus `return info;` with

```cpp
    return FlashforgeApi::make_upload_extended_info(options, true, mappings);
```

`src/slic3r/GUI/PrintHostDialogs.cpp`, `FlashforgePrintHostSendDialog::extendedInfo`: delete `int mapped_count = 0;` and the `++mapped_count;` after `mappings.push_back(...)`, and replace the final `return {...};` with

```cpp
    FlashforgeApi::PrintOptions options;
    options.leveling   = m_leveling_before_print;
    options.time_lapse = m_time_lapse_video;
    return FlashforgeApi::make_upload_extended_info(options, m_use_material_station, mappings);
```

`src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp`, `send_to_printer`: replace the comment and the `extended_info = {...};` literal with

```cpp
                Slic3r::FlashforgeApi::PrintOptions options;
                options.leveling = leveling;
                extended_info = Slic3r::FlashforgeApi::make_upload_extended_info(options, use_material_station, mappings_payload);
```

`print_printer_file`: replace `if (!ff->print_gcode_file(file_name, leveling, mappings_payload, msg))` with

```cpp
            Slic3r::FlashforgeApi::PrintOptions options;
            options.leveling = leveling;
            if (!ff->print_gcode_file(file_name, options, mappings_payload, msg))
```

`src/slic3r/GUI/Plater.hpp`, the comment above `send_gcode_direct`: change `levelingBeforePrint/timeLapseVideo/useMatlStation/gcodeToolCnt/materialMappings` to `the keys FlashforgeApi::make_upload_extended_info builds`.

- [ ] **Step 6: Run the tests**

Run the build command, then the tests with `"[flashforge]"`. Expected: all pass, the five new cases included.

- [ ] **Step 7: Commit**

```bash
git add src/slic3r/Utils/FlashforgeApi.hpp src/slic3r/Utils/FlashforgeApi.cpp src/slic3r/Utils/Flashforge.hpp src/slic3r/Utils/Flashforge.cpp src/slic3r/Utils/FlashforgePrinterAgent.cpp src/slic3r/GUI/PrintHostDialogs.cpp src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp src/slic3r/GUI/Plater.hpp tests/slic3rutils/test_flashforge_api.cpp
git commit -m "flashforge: one set of print options from every sender, flow calibration and time-lapse on both start requests

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: The send dialog offers flow calibration; every option starts off and is remembered

**Files:**
- Modify: `src/slic3r/GUI/PrintHostDialogs.hpp`
- Modify: `src/slic3r/GUI/PrintHostDialogs.cpp`
- Create: `tests/slic3rutils/test_flashforge_send_options.cpp`
- Modify: `tests/slic3rutils/CMakeLists.txt`

**Interfaces:**
- Consumes: `FlashforgeApi::PrintOptions`, `FlashforgeApi::make_upload_extended_info` (Task 1).
- Produces:

```cpp
namespace Slic3r { namespace GUI {
FlashforgeApi::PrintOptions remembered_flashforge_print_options(const AppConfig& config);
void remember_flashforge_print_options(AppConfig& config, const FlashforgeApi::PrintOptions& options);
}}
```

- [ ] **Step 1: Write the failing tests**

Create `tests/slic3rutils/test_flashforge_send_options.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/PrintHostDialogs.hpp"

// What the Flashforge send dialog starts with: every print option off until a send was confirmed
// with it on, then whatever was confirmed last.

using Slic3r::AppConfig;
using Slic3r::FlashforgeApi::PrintOptions;
using Slic3r::GUI::remember_flashforge_print_options;
using Slic3r::GUI::remembered_flashforge_print_options;

TEST_CASE("The Flashforge send dialog starts with every print option off", "[flashforge][send_dialog]")
{
    const AppConfig config;
    CHECK(remembered_flashforge_print_options(config) == PrintOptions{});
}

TEST_CASE("The Flashforge send dialog remembers the last confirmed options", "[flashforge][send_dialog]")
{
    AppConfig    config;
    PrintOptions chosen;
    chosen.flow_calibration = true;
    chosen.time_lapse       = true;
    remember_flashforge_print_options(config, chosen);
    CHECK(remembered_flashforge_print_options(config) == chosen);

    remember_flashforge_print_options(config, PrintOptions{}); // turning them off is remembered too
    CHECK(remembered_flashforge_print_options(config) == PrintOptions{});
}

TEST_CASE("A leveling choice saved by an older build is kept", "[flashforge][send_dialog]")
{
    // Older builds started with leveling on and saved it on every send, so most configs hold "1".
    AppConfig config;
    config.set("recent", "flashforge_leveling_before_print", "1");
    CHECK(remembered_flashforge_print_options(config).leveling);
    CHECK_FALSE(remembered_flashforge_print_options(config).flow_calibration);
}
```

In `tests/slic3rutils/CMakeLists.txt`, add `test_flashforge_send_options.cpp` on the line after `test_flashforge_local_api.cpp`.

- [ ] **Step 2: Run the tests to see them fail**

Run the build command. Expected: compile error, "no member named 'remembered_flashforge_print_options' in namespace 'Slic3r::GUI'".

- [ ] **Step 3: Implement**

`src/slic3r/GUI/PrintHostDialogs.hpp`:
- Inside `namespace Slic3r {`, before `namespace GUI {`, add `class AppConfig;`.
- Before `class FlashforgePrintHostSendDialog`, add:

```cpp
// The Flashforge send dialog's print options as last confirmed with Send, from the app config's
// "recent" section. Each is off until a send was confirmed with it on.
FlashforgeApi::PrintOptions remembered_flashforge_print_options(const AppConfig& config);
// Saves them for the next dialog. Called only when a send is confirmed.
void remember_flashforge_print_options(AppConfig& config, const FlashforgeApi::PrintOptions& options);
```

- In the class, replace `::CheckBox* m_checkbox_leveling {nullptr};` with

```cpp
    ::CheckBox*                      m_checkbox_flow_calibration {nullptr};
    ::CheckBox*                      m_checkbox_leveling {nullptr};
```

- Replace `bool m_leveling_before_print {true};` and `bool m_time_lapse_video {false};` with

```cpp
    FlashforgeApi::PrintOptions      m_print_options;
```

- Delete the `CONFIG_KEY_LEVELING` and `CONFIG_KEY_TIMELAPSE` members; keep `CONFIG_KEY_IFS`.

`src/slic3r/GUI/PrintHostDialogs.cpp`, before `FlashforgePrintHostSendDialog::FlashforgePrintHostSendDialog`:

```cpp
namespace {
constexpr const char* kRecentSection            = "recent";
constexpr const char* kLevelingConfigKey        = "flashforge_leveling_before_print";
constexpr const char* kFlowCalibrationConfigKey = "flashforge_flow_calibration";
constexpr const char* kTimeLapseConfigKey       = "flashforge_timelapse_video";
} // namespace

FlashforgeApi::PrintOptions remembered_flashforge_print_options(const AppConfig& config)
{
    const auto on = [&](const char* key) { return config.get(kRecentSection, key) == "1"; };
    FlashforgeApi::PrintOptions options;
    options.leveling         = on(kLevelingConfigKey);
    options.flow_calibration = on(kFlowCalibrationConfigKey);
    options.time_lapse       = on(kTimeLapseConfigKey);
    return options;
}

void remember_flashforge_print_options(AppConfig& config, const FlashforgeApi::PrintOptions& options)
{
    const auto flag = [](bool on) { return std::string(on ? "1" : "0"); };
    config.set(kRecentSection, kLevelingConfigKey, flag(options.leveling));
    config.set(kRecentSection, kFlowCalibrationConfigKey, flag(options.flow_calibration));
    config.set(kRecentSection, kTimeLapseConfigKey, flag(options.time_lapse));
}
```

In `init()`, replace the two blocks that read `CONFIG_KEY_LEVELING` and `CONFIG_KEY_TIMELAPSE` with

```cpp
    m_print_options = remembered_flashforge_print_options(*app_config);
```

Replace the leveling and time-lapse `add_option_checkbox` calls with these three, in the order the printer's start screen lists them:

```cpp
    add_option_checkbox(options_group, m_flashforge_options_sizer, _L("Calibrate the flow before printing"), m_print_options.flow_calibration,
                        [this](bool checked) { m_print_options.flow_calibration = checked; }, &m_checkbox_flow_calibration);
    add_option_checkbox(options_group, m_flashforge_options_sizer, _L("Level the bed before printing"), m_print_options.leveling,
                        [this](bool checked) { m_print_options.leveling = checked; }, &m_checkbox_leveling);
    add_option_checkbox(options_group, m_flashforge_options_sizer, _L("Ask the printer to record a time-lapse"), m_print_options.time_lapse,
                        [this](bool checked) { m_print_options.time_lapse = checked; }, &m_checkbox_timelapse);
```

In the no-credentials block, change the comment's first words from "Both of these ride" to "All three ride", and the loop to

```cpp
        for (::CheckBox* box : {m_checkbox_flow_calibration, m_checkbox_leveling, m_checkbox_timelapse}) {
```

In `EndModal`, replace the leveling and time-lapse `set` lines with

```cpp
        remember_flashforge_print_options(*app_config, m_print_options);
```

In `extendedInfo`, replace the four lines Task 1 wrote at its end (the three `options` lines and the `return`) with

```cpp
    return FlashforgeApi::make_upload_extended_info(m_print_options, m_use_material_station, mappings);
```

- [ ] **Step 4: Run the tests**

Build, then run `"[send_dialog]"` and `"[flashforge]"`. Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/PrintHostDialogs.hpp src/slic3r/GUI/PrintHostDialogs.cpp tests/slic3rutils/test_flashforge_send_options.cpp tests/slic3rutils/CMakeLists.txt
git commit -m "flashforge: the send dialog offers flow calibration, and every print option starts off until chosen

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

The dialog itself is checked with the user in Task 5.

---

### Task 3: The MCP print tools decide the options, and say what they sent and why

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.hpp`
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.cpp`
- Modify: `src/slic3r/CMakeLists.txt` (two lines after `GUI/OrcaMCP/OrcaMCPPrinterUtils.cpp`)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp` (`send_to_printer`, `print_printer_file`)
- Create: `tests/slic3rutils/test_mcp_print_options.cpp`
- Modify: `tests/slic3rutils/CMakeLists.txt`
- Modify: `scripts/orcamcp_tools.json` (regenerated)
- Modify: `docs/tools/reference.md`, `docs/printers/flashforge-creator-5.md`, `CLAUDE.md`

**Interfaces:**
- Consumes: `FlashforgeApi::PrintOptions`, `make_upload_extended_info`, `Flashforge::print_gcode_file`, `Flashforge::stored_file_printing_time` (Task 1).
- Produces (namespace `Slic3r::GUI::OrcaMCP`):

```cpp
constexpr double kCalibrationGateSeconds = 4 * 3600.0;
std::string calibration_gate_text(); // "4 h"
struct PrintOptionRequest { std::optional<bool> leveling, flow_calibration, time_lapse; };
bool read_print_option_request(const nlohmann::json& params, PrintOptionRequest& out, std::string& error);
bool needs_print_time(const PrintOptionRequest& request);
struct PrintOptionChoice { bool on{false}; std::string decided_by; };
struct PrintOptionChoices { PrintOptionChoice leveling, flow_calibration, time_lapse; };
PrintOptionChoices choose_print_options(const PrintOptionRequest& request, std::optional<double> estimated_print_s);
FlashforgeApi::PrintOptions to_print_options(const PrintOptionChoices& choices);
nlohmann::json print_options_json(const PrintOptionChoices& choices, std::optional<double> estimated_print_s);
```

- [ ] **Step 1: Write the failing tests**

Create `tests/slic3rutils/test_mcp_print_options.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.hpp"

#include <string>
#include <utility>
#include <vector>

// How send_to_printer and print_printer_file decide leveling, flow calibration and time-lapse:
// the agent's explicit choice, else the print-time gate.

using namespace Slic3r::GUI::OrcaMCP;

TEST_CASE("An explicit choice wins over the print-time gate", "[orcamcp][print_options]")
{
    PrintOptionRequest request;
    request.leveling         = false;
    request.flow_calibration = true;
    request.time_lapse       = true;
    const auto choices = choose_print_options(request, 3 * 24 * 3600.0); // a three-day print

    CHECK_FALSE(choices.leveling.on);
    CHECK(choices.leveling.decided_by == "caller");
    CHECK(choices.flow_calibration.on);
    CHECK(choices.flow_calibration.decided_by == "caller");
    CHECK(choices.time_lapse.on);
    CHECK(choices.time_lapse.decided_by == "caller");
}

TEST_CASE("Left to the tool, calibration runs from four hours", "[orcamcp][print_options]")
{
    const auto at_gate = choose_print_options(PrintOptionRequest{}, kCalibrationGateSeconds);
    CHECK(at_gate.leveling.on);
    CHECK(at_gate.flow_calibration.on);
    CHECK(at_gate.leveling.decided_by == "print_time_gate");

    const auto under = choose_print_options(PrintOptionRequest{}, kCalibrationGateSeconds - 1);
    CHECK_FALSE(under.leveling.on);
    CHECK_FALSE(under.flow_calibration.on);
    CHECK(under.flow_calibration.decided_by == "print_time_gate");
}

TEST_CASE("Without an estimate, calibration stays off and says why", "[orcamcp][print_options]")
{
    for (const std::optional<double> estimate : {std::optional<double>{}, std::optional<double>{0.0}}) {
        const auto choices = choose_print_options(PrintOptionRequest{}, estimate);
        CHECK_FALSE(choices.leveling.on);
        CHECK(choices.leveling.decided_by == "print_time_unknown");
        CHECK_FALSE(choices.flow_calibration.on);
        CHECK(choices.flow_calibration.decided_by == "print_time_unknown");
    }
}

TEST_CASE("Time-lapse is off unless asked for, whatever the print's length", "[orcamcp][print_options]")
{
    const auto choices = choose_print_options(PrintOptionRequest{}, 3 * 24 * 3600.0);
    CHECK_FALSE(choices.time_lapse.on);
    CHECK(choices.time_lapse.decided_by == "default");
}

TEST_CASE("read_print_option_request reads the three booleans", "[orcamcp][print_options]")
{
    PrintOptionRequest request;
    std::string        error;
    REQUIRE(read_print_option_request(nlohmann::json{{"flow_calibration", true}, {"time_lapse", false}}, request, error));
    CHECK_FALSE(request.leveling.has_value());
    CHECK(request.flow_calibration == true);
    CHECK(request.time_lapse == false);
}

TEST_CASE("read_print_option_request refuses a value that is not a boolean", "[orcamcp][print_options]")
{
    const std::vector<std::pair<std::string, nlohmann::json>> bad = {
        {"leveling_before_print", "true"}, {"flow_calibration", 1}, {"time_lapse", nullptr}};
    for (const auto& [name, value] : bad) {
        PrintOptionRequest request;
        std::string        error;
        CHECK_FALSE(read_print_option_request(nlohmann::json{{name, value}}, request, error));
        CHECK(error == name + " must be a boolean");
    }
}

TEST_CASE("needs_print_time only when calibration is left to the gate", "[orcamcp][print_options]")
{
    CHECK(needs_print_time(PrintOptionRequest{}));

    PrintOptionRequest one;
    one.leveling = true;
    CHECK(needs_print_time(one));

    PrintOptionRequest both;
    both.leveling         = true;
    both.flow_calibration = false;
    CHECK_FALSE(needs_print_time(both));
}

TEST_CASE("print_options_json says what was sent and why", "[orcamcp][print_options]")
{
    PrintOptionRequest request;
    request.flow_calibration = false;
    const nlohmann::json j = print_options_json(choose_print_options(request, 5 * 3600.0), 5 * 3600.0);

    CHECK(j["leveling"] == nlohmann::json{{"on", true}, {"decided_by", "print_time_gate"}});
    CHECK(j["flow_calibration"] == nlohmann::json{{"on", false}, {"decided_by", "caller"}});
    CHECK(j["time_lapse"] == nlohmann::json{{"on", false}, {"decided_by", "default"}});
    CHECK(j["estimated_print_s"] == 18000);
    CHECK(j["gate_s"] == 14400);

    CHECK(print_options_json(choose_print_options(PrintOptionRequest{}, std::nullopt), std::nullopt)["estimated_print_s"].is_null());
}

TEST_CASE("to_print_options carries the choices to the printer", "[orcamcp][print_options]")
{
    PrintOptionRequest request;
    request.time_lapse = true;
    const auto options = to_print_options(choose_print_options(request, kCalibrationGateSeconds));
    CHECK(options.leveling);
    CHECK(options.flow_calibration);
    CHECK(options.time_lapse);
}

TEST_CASE("The tool descriptions state the gate from the constant", "[orcamcp][print_options]")
{
    CHECK(calibration_gate_text() == "4 h");
}
```

In `tests/slic3rutils/CMakeLists.txt`, add `test_mcp_print_options.cpp` on the line after `test_material_mapping.cpp`.

- [ ] **Step 2: Run the tests to see them fail**

Run the build command. Expected: "'slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.hpp' file not found".

- [ ] **Step 3: Implement the decision**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.hpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.hpp
#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/FlashforgeApi.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// How send_to_printer and print_printer_file decide what a Flashforge does before and during the
// print they start. The agent decides: an explicit boolean always wins. Left to the tool, leveling
// and flow calibration run for a print estimated at kCalibrationGateSeconds or longer, where the
// minutes they add are small beside what a failed print wastes, and time-lapse stays off. Pure and
// any thread; tests/slic3rutils/test_mcp_print_options.cpp pins it.

constexpr double kCalibrationGateSeconds = 4 * 3600.0;

// The gate as the tool descriptions state it ("4 h"), so they cannot drift from the constant.
std::string calibration_gate_text();

// The tool parameters as the caller gave them; unset when omitted.
struct PrintOptionRequest
{
    std::optional<bool> leveling;         // leveling_before_print
    std::optional<bool> flow_calibration; // flow_calibration
    std::optional<bool> time_lapse;       // time_lapse
};

// Reads the three parameters from a tool's `params`. False with `error` ("<name> must be a
// boolean") when one is present and is not a boolean.
bool read_print_option_request(const nlohmann::json& params, PrintOptionRequest& out, std::string& error);

// Whether choose_print_options reads the estimate: some calibration was left to the gate.
bool needs_print_time(const PrintOptionRequest& request);

// One option as decided, and by what: "caller", "print_time_gate", "print_time_unknown" (left to
// the gate with no estimate, so off) or "default" (time-lapse left to the tool, so off).
struct PrintOptionChoice
{
    bool        on{false};
    std::string decided_by;
};

struct PrintOptionChoices
{
    PrintOptionChoice leveling;
    PrintOptionChoice flow_calibration;
    PrintOptionChoice time_lapse;
};

// `estimated_print_s` is the print's estimated time; unset or not positive when unknown.
PrintOptionChoices choose_print_options(const PrintOptionRequest& request, std::optional<double> estimated_print_s);

FlashforgeApi::PrintOptions to_print_options(const PrintOptionChoices& choices);

// The response's `print_options`: each option's {on, decided_by}, the estimate the gate read
// (`estimated_print_s`, whole seconds, null when unknown) and the gate (`gate_s`).
nlohmann::json print_options_json(const PrintOptionChoices& choices, std::optional<double> estimated_print_s);

}}} // namespace Slic3r::GUI::OrcaMCP
```

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.cpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.cpp
#include "OrcaMCPPrintOptions.hpp"

#include <cmath>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

constexpr const char* kLevelingParam        = "leveling_before_print";
constexpr const char* kFlowCalibrationParam = "flow_calibration";
constexpr const char* kTimeLapseParam       = "time_lapse";

bool read_optional_bool(const nlohmann::json& params, const char* name, std::optional<bool>& out, std::string& error)
{
    out.reset();
    if (!params.is_object() || !params.contains(name))
        return true;
    const nlohmann::json& value = params.at(name);
    if (!value.is_boolean()) {
        error = std::string(name) + " must be a boolean";
        return false;
    }
    out = value.get<bool>();
    return true;
}

std::optional<double> known_estimate(std::optional<double> estimated_print_s)
{
    if (estimated_print_s && *estimated_print_s > 0)
        return estimated_print_s;
    return std::nullopt;
}

PrintOptionChoice gated(std::optional<bool> requested, std::optional<double> estimate)
{
    if (requested)
        return {*requested, "caller"};
    if (!estimate)
        return {false, "print_time_unknown"};
    return {*estimate >= kCalibrationGateSeconds, "print_time_gate"};
}

nlohmann::json choice_json(const PrintOptionChoice& choice)
{
    return {{"on", choice.on}, {"decided_by", choice.decided_by}};
}

} // namespace

std::string calibration_gate_text()
{
    return std::to_string(std::lround(kCalibrationGateSeconds / 3600.0)) + " h";
}

bool read_print_option_request(const nlohmann::json& params, PrintOptionRequest& out, std::string& error)
{
    return read_optional_bool(params, kLevelingParam, out.leveling, error) &&
           read_optional_bool(params, kFlowCalibrationParam, out.flow_calibration, error) &&
           read_optional_bool(params, kTimeLapseParam, out.time_lapse, error);
}

bool needs_print_time(const PrintOptionRequest& request)
{
    return !request.leveling || !request.flow_calibration;
}

PrintOptionChoices choose_print_options(const PrintOptionRequest& request, std::optional<double> estimated_print_s)
{
    const std::optional<double> estimate = known_estimate(estimated_print_s);
    PrintOptionChoices          choices;
    choices.leveling         = gated(request.leveling, estimate);
    choices.flow_calibration = gated(request.flow_calibration, estimate);
    choices.time_lapse       = request.time_lapse ? PrintOptionChoice{*request.time_lapse, "caller"}
                                                  : PrintOptionChoice{false, "default"};
    return choices;
}

FlashforgeApi::PrintOptions to_print_options(const PrintOptionChoices& choices)
{
    FlashforgeApi::PrintOptions options;
    options.leveling         = choices.leveling.on;
    options.flow_calibration = choices.flow_calibration.on;
    options.time_lapse       = choices.time_lapse.on;
    return options;
}

nlohmann::json print_options_json(const PrintOptionChoices& choices, std::optional<double> estimated_print_s)
{
    const std::optional<double> estimate = known_estimate(estimated_print_s);
    return {{"leveling", choice_json(choices.leveling)},
            {"flow_calibration", choice_json(choices.flow_calibration)},
            {"time_lapse", choice_json(choices.time_lapse)},
            {"estimated_print_s", estimate ? nlohmann::json(std::lround(*estimate)) : nlohmann::json(nullptr)},
            {"gate_s", std::lround(kCalibrationGateSeconds)}};
}

}}} // namespace Slic3r::GUI::OrcaMCP
```

In `src/slic3r/CMakeLists.txt`, after the line `GUI/OrcaMCP/OrcaMCPPrinterUtils.cpp`, add:

```
    GUI/OrcaMCP/OrcaMCPPrintOptions.hpp
    GUI/OrcaMCP/OrcaMCPPrintOptions.cpp
```

- [ ] **Step 4: Run the tests**

Build, then run `"[print_options]"`. Expected: all pass.

- [ ] **Step 5: Use it in `send_to_printer`**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp`, add `#include "OrcaMCPPrintOptions.hpp"` with the other OrcaMCP includes.

Schema: replace the `leveling_before_print` property and add two after it:

```cpp
                {"leveling_before_print", {
                    {"type", "boolean"},
                    {"description", "Level the bed before printing. Omitted: on when the plate's estimated time is " +
                                    calibration_gate_text() + " or more. Direct sends to a Flashforge host with local-API "
                                    "credentials only; ignored otherwise."}
                }},
                {"flow_calibration", {
                    {"type", "boolean"},
                    {"description", "Calibrate the flow before printing. Omitted: on when the plate's estimated time is " +
                                    calibration_gate_text() + " or more. Direct sends to a Flashforge host with local-API "
                                    "credentials only; ignored otherwise."}
                }},
                {"time_lapse", {
                    {"type", "boolean"},
                    {"description", "Ask the printer to record a time-lapse (default false). Direct sends to a Flashforge "
                                    "host with local-API credentials only; ignored otherwise."}
                }},
```

Description: append to the existing text (the literal concatenation becomes `std::string(...) + ...`):

```cpp
        " On a Flashforge the printer can level the bed and calibrate the flow before it starts; each adds "
        "minutes, and the choice is yours. Pass leveling_before_print and flow_calibration true before a long "
        "print or after a filament, nozzle or bed change, false for a short print or a repeat soon after the "
        "last one on the same filaments (get_printer_status's last_print_started_here says when this app last "
        "started a print there, and with what). Omitted, each runs when the plate's estimated time is " +
        calibration_gate_text() + " or more. The response's print_options says what was sent and why."
```

Handler:
- Change the validation loop to `for (const char* flag : {"start_print", "use_material_station"})`, and after it add

```cpp
            PrintOptionRequest option_request;
            std::string        option_error;
            if (!read_print_option_request(params, option_request, option_error))
                return error_response(option_error);
```

- Delete `const bool leveling = params.value("leveling_before_print", false);`.
- Next to `int plate_idx = 0;` declare `std::optional<double> estimated_print_s;`, and in the first main-thread lambda, after `plate_idx = ...;`, add the lines below (if `PrintEstimatedStatistics` is not yet visible in this file, add `#include "libslic3r/GCode/GCodeProcessor.hpp"`)

```cpp
                if (const GCodeProcessorResult* result = plate->get_slice_result())
                    estimated_print_s = result->print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time;
```

- Before `auto* ff = dynamic_cast<Slic3r::Flashforge*>(host.get());` declare `std::optional<PrintOptionChoices> print_options;`, and replace the three lines Task 1 put in the Flashforge branch with

```cpp
                print_options = choose_print_options(option_request, estimated_print_s);
                extended_info = Slic3r::FlashforgeApi::make_upload_extended_info(to_print_options(*print_options),
                                                                                 use_material_station, mappings_payload);
```

- Before `if (!info_messages.empty())`, add

```cpp
            if (print_options)
                response["print_options"] = print_options_json(*print_options, estimated_print_s);
```

- [ ] **Step 6: Use it in `print_printer_file`**

Schema: replace `leveling_before_print` and add two after it:

```cpp
                {"leveling_before_print", {
                    {"type", "boolean"},
                    {"description", "Level the bed before printing. Omitted: on when the file's estimated time, from "
                                    "the printer's file list, is " + calibration_gate_text() + " or more."}
                }},
                {"flow_calibration", {
                    {"type", "boolean"},
                    {"description", "Calibrate the flow before printing. Omitted: on when the file's estimated time, "
                                    "from the printer's file list, is " + calibration_gate_text() + " or more."}
                }},
                {"time_lapse", {
                    {"type", "boolean"},
                    {"description", "Ask the printer to record a time-lapse (default false)."}
                }},
```

Description: append

```cpp
        " Leveling and flow calibration each add minutes before the print; decide them as for send_to_printer. "
        "Omitted, each runs when the file's estimated time, from the printer's file list, is " +
        calibration_gate_text() + " or more, and not when the printer does not report one. The response's "
        "print_options says what was sent and why."
```

Handler: replace the `leveling_before_print` check and `const bool leveling = ...;` with

```cpp
            PrintOptionRequest option_request;
            std::string        option_error;
            if (!read_print_option_request(params, option_request, option_error))
                return error_response(option_error);
```

After `resolve_flashforge(...)` succeeds, add

```cpp
            // The gate needs the file's own estimate, and only the printer's file list has it. A list that
            // cannot be read leaves the estimate unknown; the print request below reports a dead printer.
            std::optional<double> estimated_print_s;
            if (needs_print_time(option_request)) {
                std::optional<long> printing_time_s;
                wxString            list_msg;
                if (ff->stored_file_printing_time(file_name, printing_time_s, list_msg) && printing_time_s)
                    estimated_print_s = double(*printing_time_s);
            }
            const PrintOptionChoices print_options = choose_print_options(option_request, estimated_print_s);
```

Replace the three lines Task 1 wrote there (the two `options` lines and the `if` line) with

```cpp
            if (!ff->print_gcode_file(file_name, to_print_options(print_options), mappings_payload, msg))
```

and the success return with

```cpp
            return {{"status", "success"},
                    {"file_name", file_name},
                    {"material_mappings", mappings_report},
                    {"print_options", print_options_json(print_options, estimated_print_s)}};
```

- [ ] **Step 7: Regenerate the golden file and run the tool-list tests**

```bash
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests
ORCAMCP_UPDATE_TOOLS_GOLDEN=1 build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][tools]"
build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[orcamcp][tools]"
python3 -m unittest discover -s scripts/tests -t scripts
```

Expected: the second run and the Python tests pass. `git diff scripts/orcamcp_tools.json` shows only the two tools' descriptions and properties.

- [ ] **Step 8: Docs**

`docs/tools/reference.md`: the `### send_to_printer` section is out of date (it says the tool opens a dialog). Replace it with the text below, and add the `### print_printer_file` section after it.

````markdown
### send_to_printer
Upload the sliced plate to the configured print host and, by default, **start printing it**.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `direct` | boolean | No | Upload straight to the printer (default true). False opens OrcaSlicer's send dialog for the user |
| `start_print` | boolean | No | Start printing once uploaded (default true) |
| `leveling_before_print` | boolean | No | Level the bed first. Omitted: on when the plate's estimated time is 4 h or more |
| `flow_calibration` | boolean | No | Calibrate the flow first. Omitted: on when the plate's estimated time is 4 h or more |
| `time_lapse` | boolean | No | Record a time-lapse (default false) |
| `use_material_station` | boolean | No | Feed from the material station (default: when the printer has one) |
| `material_mappings` | array | No | Explicit `{tool_id, slot_id}` pairs; omitted, the project's filaments are matched to loaded slots |
| `file_name` | string | No | Name to store the upload under |
| `all_plates` | boolean | No | Dialog sends only: send every plate |

The print options, the station and the mappings apply to a Flashforge host with local-API
credentials (serial number and check code); other hosts ignore them.

Leveling and flow calibration each add minutes before the print starts, so the agent decides:
true before a long print or after a filament, nozzle or bed change, false for a short print or a
repeat soon after the last one on the same filaments. `get_printer_status`'s
`last_print_started_here` says when this app last started a print on the printer, and with what.

**Returns (Flashforge):**
```json
{
  "status": "queued",
  "host_type": "flashforge",
  "file_name": "vase.gcode.3mf",
  "start_print": true,
  "material_mappings": [{"tool_id": 0, "slot_id": 1, "color_delta_e": 2.1}],
  "print_options": {
    "leveling": {"on": true, "decided_by": "print_time_gate"},
    "flow_calibration": {"on": false, "decided_by": "caller"},
    "time_lapse": {"on": false, "decided_by": "default"},
    "estimated_print_s": 52200,
    "gate_s": 14400
  },
  "note": "Upload progress is shown in OrcaSlicer; poll get_printer_status."
}
```
`decided_by` is `caller` (passed explicitly), `print_time_gate` (omitted; on from `gate_s`),
`print_time_unknown` (omitted with no estimate, so off) or `default` (time-lapse omitted, so off).

---

### print_printer_file
Start printing a file already stored on the Flashforge printer (see `list_printer_files`).

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file_name` | string | Yes | The file, as `list_printer_files` names it |
| `leveling_before_print` | boolean | No | Level the bed first. Omitted: on when the file's estimated time is 4 h or more |
| `flow_calibration` | boolean | No | Calibrate the flow first. Omitted: on when the file's estimated time is 4 h or more |
| `time_lapse` | boolean | No | Record a time-lapse (default false) |
| `material_mappings` | array | No | Explicit `{tool_id, slot_id}` pairs |
| `auto_map` | boolean | No | Match the project's filaments to loaded slots when no mapping is given (default true) |

The file's estimated time comes from the printer's file list (`printingTime`). When the printer
does not report one, omitted calibration stays off (`print_time_unknown`). Returns `file_name`,
`material_mappings` and `print_options`, shaped as in `send_to_printer`.
````

`docs/printers/flashforge-creator-5.md`, under `## Printing to it`, after its first paragraph:

```markdown
### Calibration and time-lapse

The printer's own start screen offers flow calibration, bed leveling and a time-lapse, and so
does OrcaSlicer. In the send dialog all three start off, and the next dialog remembers what you
chose. An assistant decides per print: calibration adds minutes before the first layer, so it is
worth it before a long print or after a filament change, and not for a short repeat. Left to
OrcaMCP, leveling and flow calibration run for prints estimated at 4 hours or more.
```

`CLAUDE.md`, the **Printers** row of the tool table: change `` `send_to_printer` `` to
`` `send_to_printer` (`leveling_before_print`, `flow_calibration`, `time_lapse`: the agent's call; omitted, leveling and flow calibration run from 4 h estimated; `print_options` says what was sent and why) `` and `` `print_printer_file` `` to `` `print_printer_file` (the same options, gated on the file's `printingTime`) ``.

- [ ] **Step 9: Run everything touched, then commit**

Run `"[flashforge]"`, `"[print_options]"`, `"[orcamcp][tools]"` and the Python tests. Expected: all pass.

```bash
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.cpp src/slic3r/CMakeLists.txt src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp tests/slic3rutils/test_mcp_print_options.cpp tests/slic3rutils/CMakeLists.txt scripts/orcamcp_tools.json docs/tools/reference.md docs/printers/flashforge-creator-5.md CLAUDE.md
git commit -m "mcp: send_to_printer and print_printer_file take leveling, flow calibration and time-lapse, leave calibration to a print-time gate when omitted, and say what they sent

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: The last print this app started, recorded and reported

The agent's other fact: "is this a repeat soon after the last print?" The printer does not say when it last calibrated, so the app records every start the printer accepts (from the dialog, both MCP tools and the printer agent), per printer, in memory. The record sits next to the existing last-status cache, which keeps the same kind of value, so both become one template.

**Files:**
- Modify: `src/slic3r/Utils/FlashforgeLocalApi.hpp`, `src/slic3r/Utils/FlashforgeLocalApi.cpp`
- Modify: `src/slic3r/Utils/Flashforge.hpp`, `src/slic3r/Utils/Flashforge.cpp`
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.hpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.cpp`
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp`
- Test: `tests/slic3rutils/test_flashforge_local_api.cpp`, `tests/slic3rutils/test_mcp_print_options.cpp`
- Modify: `scripts/orcamcp_tools.json` (regenerated), `docs/tools/reference.md`, `CLAUDE.md`

**Interfaces:**
- Consumes: `FlashforgeApi::PrintOptions` (Task 1); `OrcaMCPPrintOptions.hpp` (Task 3).
- Produces:

```cpp
namespace Slic3r { namespace FlashforgeLocalApi {
template <class T> class LatestPerHost { /* Clock; struct Aged { T value; long age_s; }; put(host, T, now); get(host, now) -> std::optional<Aged> */ };
using StatusCache  = LatestPerHost<FlashforgeApi::PrinterStatus>;
using CachedStatus = StatusCache::Aged;          // was a struct with `status`; the field is now `value`
struct PrintStart { std::string file_name; FlashforgeApi::PrintOptions options; nlohmann::json material_mappings; };
using PrintStartLog      = LatestPerHost<PrintStart>;
using RecordedPrintStart = PrintStartLog::Aged;
PrintStartLog& print_start_log();
}}
// class Slic3r::Flashforge
std::optional<FlashforgeLocalApi::RecordedPrintStart> last_print_start() const;
// namespace Slic3r::GUI::OrcaMCP
nlohmann::json print_start_json(const std::optional<FlashforgeLocalApi::RecordedPrintStart>& recorded);
```

- [ ] **Step 1: Write the failing tests**

In `tests/slic3rutils/test_flashforge_local_api.cpp`, in the `"StatusCache returns a host's last status with its age"` case, change `cached->status.slots` to `cached->value.slots` (two places). After that case, add:

```cpp
TEST_CASE("PrintStartLog keeps each printer's last accepted start with its age", "[flashforge]")
{
    using Clock = PrintStartLog::Clock;
    PrintStartLog           log;
    const Clock::time_point t0 = Clock::now();

    CHECK_FALSE(log.get("10.0.0.100", t0).has_value());

    PrintStart first;
    first.file_name = "cube.gcode.3mf";
    PrintStart second;
    second.file_name                = "vase.gcode.3mf";
    second.options.flow_calibration = true;
    second.material_mappings        = nlohmann::json::array({{{"toolId", 0}, {"slotId", 2}, {"materialName", "PLA"}}});

    log.put("10.0.0.100", first, t0);
    log.put("10.0.0.100", second, t0 + std::chrono::seconds(60)); // the newer one wins

    const auto recorded = log.get("10.0.0.100", t0 + std::chrono::seconds(2460));
    REQUIRE(recorded.has_value());
    CHECK(recorded->age_s == 2400);
    CHECK(recorded->value.file_name == "vase.gcode.3mf");
    CHECK(recorded->value.options.flow_calibration);
    CHECK(recorded->value.material_mappings.size() == 1);

    CHECK_FALSE(log.get("10.0.0.101", t0).has_value()); // another printer's start is not this one's
}
```

In `tests/slic3rutils/test_mcp_print_options.cpp`, add:

```cpp
TEST_CASE("last_print_started_here says what the last start asked for", "[orcamcp][print_options]")
{
    CHECK(print_start_json(std::nullopt).is_null());

    Slic3r::FlashforgeLocalApi::RecordedPrintStart recorded;
    recorded.value.file_name         = "vase.gcode.3mf";
    recorded.value.options.leveling  = true;
    recorded.value.material_mappings = nlohmann::json::array({{{"toolId", 0}, {"slotId", 2}, {"materialName", "PLA"},
                                                               {"toolMaterialColor", "#FFFFFF"}, {"slotMaterialColor", "#FF0000"}}});
    recorded.age_s = 2400;

    const nlohmann::json j = print_start_json(recorded);
    CHECK(j["file_name"] == "vase.gcode.3mf");
    CHECK(j["age_s"] == 2400);
    CHECK(j["leveling"] == true);
    CHECK(j["flow_calibration"] == false);
    CHECK(j["time_lapse"] == false);
    CHECK(j["material_mappings"] ==
          nlohmann::json::array({{{"tool_id", 0}, {"slot_id", 2}, {"material", "PLA"}, {"color", "#FF0000"}}}));
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run the build command. Expected: compile errors, "no member named 'value'" on `CachedStatus` and "unknown type name 'PrintStartLog'".

- [ ] **Step 3: One template for both records**

In `src/slic3r/Utils/FlashforgeLocalApi.hpp`, replace everything from the `// ── The last status each printer answered with` banner down to `StatusCache& status_cache();` with:

```cpp
// ── The latest value per printer ───────────────────────────────────────────────────────────────

// The latest value put for each host, and how old it is. Thread-safe: the agent's poll, the Device
// page's poll and MCP calls all record into the same ones.
template <class T>
class LatestPerHost
{
public:
    using Clock = std::chrono::steady_clock;

    struct Aged
    {
        T    value;
        long age_s{0};
    };

    void put(const std::string& host, T value, Clock::time_point now = Clock::now())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries[host] = Entry{std::move(value), now};
    }

    std::optional<Aged> get(const std::string& host, Clock::time_point now = Clock::now()) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_entries.find(host);
        if (it == m_entries.end())
            return std::nullopt;
        const long age_s = long(std::chrono::duration_cast<std::chrono::seconds>(now - it->second.at).count());
        return Aged{it->second.value, age_s};
    }

private:
    struct Entry
    {
        T                 value;
        Clock::time_point at;
    };
    mutable std::mutex           m_mutex;
    std::map<std::string, Entry> m_entries;
};

// The last status each host answered with. Every successful Flashforge::fetch_status records into
// it, so when a live read fails the caller can still say what the printer last reported and how
// long ago.
using StatusCache  = LatestPerHost<FlashforgeApi::PrinterStatus>;
using CachedStatus = StatusCache::Aged;

// The process-wide cache every Flashforge host records into.
StatusCache& status_cache();

// A print the printer accepted from this app: an upload with printNow, or printGcode.
struct PrintStart
{
    std::string                 file_name;
    FlashforgeApi::PrintOptions options;
    nlohmann::json              material_mappings = nlohmann::json::array(); // as sent: {toolId, slotId, materialName, ...}
};

// The last print each host accepted from this process. Flashforge::upload_local_api (with printNow)
// and Flashforge::print_gcode_file record into it, so the send dialog, send_to_printer,
// print_printer_file and the printer agent are all here. A print started on the printer's screen,
// from another computer or before this app launched is not. Memory only.
using PrintStartLog      = LatestPerHost<PrintStart>;
using RecordedPrintStart = PrintStartLog::Aged;

// The process-wide log every Flashforge host records into.
PrintStartLog& print_start_log();
```

In `src/slic3r/Utils/FlashforgeLocalApi.cpp`, delete `StatusCache::put` and `StatusCache::get`, keep `status_cache()`, and add after it:

```cpp
PrintStartLog& print_start_log()
{
    static PrintStartLog log;
    return log;
}
```

In `src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp`, change `cached.status.` to `cached.value.` in `cached_station_json` (two places) and `match_from_cached_status` (one place).

- [ ] **Step 4: Record every accepted start**

`src/slic3r/Utils/Flashforge.hpp`, public, after `last_known_status`:

```cpp
    // The last print this app started on this printer, and its age; nullopt when none since the app
    // started. Never touches the network.
    std::optional<FlashforgeLocalApi::RecordedPrintStart> last_print_start() const;
```

private, after `fetch_gcode_list`:

```cpp
    // Called only once the printer accepted a start.
    void record_print_start(const std::string& file_name, const FlashforgeApi::PrintOptions& options, const nlohmann::json& material_mappings) const;
```

`src/slic3r/Utils/Flashforge.cpp`, after `last_known_status`:

```cpp
std::optional<FlashforgeLocalApi::RecordedPrintStart> Flashforge::last_print_start() const
{
    return FlashforgeLocalApi::print_start_log().get(m_local_api_host);
}

void Flashforge::record_print_start(const std::string& file_name, const FlashforgeApi::PrintOptions& options, const json& material_mappings) const
{
    FlashforgeLocalApi::PrintStart start;
    start.file_name         = file_name;
    start.options           = options;
    start.material_mappings = material_mappings.is_array() ? material_mappings : json::array();
    FlashforgeLocalApi::print_start_log().put(m_local_api_host, std::move(start));
}
```

In `print_gcode_file`, replace the `return request_local_api_json(...)` line with

```cpp
    if (!request_local_api_json("printGcode", FlashforgeApi::make_print_gcode_payload(m_serial_number, m_check_code, file_name, options, material_mappings).dump(), body, msg))
        return false;
    record_print_start(file_name, options, material_mappings);
    return true;
```

At the end of `upload_local_api`, replace

```cpp
    if (!ok && !failure.cancelled)
        error_fn(error_msg);
    return ok;
```

with

```cpp
    if (!ok && !failure.cancelled)
        error_fn(error_msg);
    if (ok && upload_data.post_action == PrintHostPostUploadAction::StartPrint)
        record_print_start(filename, options, json::parse(material_map_json, nullptr, false));
    return ok;
```

- [ ] **Step 5: Report it in `get_printer_status`**

In `OrcaMCPPrintOptions.hpp`, add `#include "slic3r/Utils/FlashforgeLocalApi.hpp"` and, at the end of the namespace:

```cpp
// get_printer_status's `last_print_started_here`: the last print this app started on the printer,
// the options it asked for and the slots it fed from; null when none since the app started.
nlohmann::json print_start_json(const std::optional<FlashforgeLocalApi::RecordedPrintStart>& recorded);
```

In `OrcaMCPPrintOptions.cpp`:

```cpp
nlohmann::json print_start_json(const std::optional<FlashforgeLocalApi::RecordedPrintStart>& recorded)
{
    if (!recorded)
        return nullptr;

    const FlashforgeLocalApi::PrintStart& start = recorded->value;
    nlohmann::json                        slots = nlohmann::json::array();
    for (const auto& mapping : start.material_mappings)
        if (mapping.is_object())
            slots.push_back({{"tool_id", mapping.value("toolId", -1)},
                             {"slot_id", mapping.value("slotId", -1)},
                             {"material", mapping.value("materialName", std::string())},
                             {"color", mapping.value("slotMaterialColor", std::string())}});

    return {{"file_name", start.file_name},
            {"age_s", recorded->age_s},
            {"leveling", start.options.leveling},
            {"flow_calibration", start.options.flow_calibration},
            {"time_lapse", start.options.time_lapse},
            {"material_mappings", slots}};
}
```

In `OrcaMCPPrinterTools.cpp`, `get_printer_status`: add to the success response

```cpp
                    {"last_print_started_here", print_start_json(ff->last_print_start())},
```

and append to its description:

```cpp
        " last_print_started_here is the last print this app started on the printer: age_s, file_name, the "
        "leveling, flow_calibration and time_lapse it asked for, and the slots it fed from; null when none "
        "since the app started. Prints started on the printer's screen or from another computer are not in "
        "it. Use it to judge whether a new print needs calibrating again."
```

- [ ] **Step 6: Run the tests, regenerate the golden file**

Build, run `"[flashforge]"` and `"[print_options]"`, then the golden regeneration and checks from Task 3, Step 7. Expected: all pass; the golden diff touches only `get_printer_status`.

- [ ] **Step 7: Docs**

`docs/tools/reference.md`, `### get_printer_status`: add `"last_print_started_here": {"file_name": "vase.gcode.3mf", "age_s": 2400, "leveling": true, "flow_calibration": true, "time_lapse": false, "material_mappings": [{"tool_id": 0, "slot_id": 2, "material": "PLA", "color": "#FF0000"}]}` to the Returns example, and after the `obico.configured` line:

```markdown
`last_print_started_here` is the last print this app started on the printer, from any of its
paths (the send dialog, `send_to_printer`, `print_printer_file`), with the options it asked for
and the slots it fed from. It is null when the app has started none since it launched, and it
never includes a print started on the printer's screen or from another computer. An agent uses
it to decide whether a new print needs calibrating again.
```

`CLAUDE.md`, the **Printers** row: change `` `get_printer_status` (a failure names host:port ... ) `` to start `` `get_printer_status` (`last_print_started_here`: the last print this app started there, its options and slots; a failure names host:port ... ) ``, keeping the rest of the parenthesis.

- [ ] **Step 8: Commit**

```bash
git add src/slic3r/Utils/FlashforgeLocalApi.hpp src/slic3r/Utils/FlashforgeLocalApi.cpp src/slic3r/Utils/Flashforge.hpp src/slic3r/Utils/Flashforge.cpp src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPPrintOptions.cpp src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp tests/slic3rutils/test_flashforge_local_api.cpp tests/slic3rutils/test_mcp_print_options.cpp scripts/orcamcp_tools.json docs/tools/reference.md CLAUDE.md
git commit -m "mcp: get_printer_status says when this app last started a print on the printer, and with which options

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Check on the real printer, with the user

Unit tests prove what is sent, not what the printer does with it. This task needs the user at the printer: steps A and B start real prints (short ones). Ask before each one.

**Files:**
- Modify: `docs/printers/flashforge-lan-api.md` (section 4, `printGcode`; section 7's findings)
- Possibly modify: `src/slic3r/Utils/FlashforgeApi.cpp`, `tests/slic3rutils/test_flashforge_api.cpp`, `src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp`, `scripts/orcamcp_tools.json`, `docs/tools/reference.md` (only if step B says `printGcode` does not take a time-lapse)

- [ ] **Step 1: Build and launch the dev app**

```bash
cmake --build build/arm64 --config RelWithDebInfo --target all --
lsof -nP -iTCP:13618 -sTCP:LISTEN
```

If anything but the dev build holds 13618 (the release app in `/Applications` included), ask the user to quit it. Launch the dev build with `ORCAMCP_SKIP_CLOUD_LOGIN=1` (`open --env`, see the "unattended app launch" memory). Confirm with `mcp__orca-slicer__get_server_info`.

- [ ] **Step 2: A, the send dialog (a real print; ask first)**

Load a small model (a 20 mm cube), `slice_all`, `wait_for_slice`. Ask the user to click Print. They check: three boxes in the printer's order (Calibrate the flow, Level the bed, Record a time-lapse). Leveling may show ticked, the old default remembered (Review Focus 5); the other two are off. The user ticks all three and presses Send, then watches the printer: does it run flow calibration, then leveling, before the first layer? Note how many minutes each takes.
Then `get_printer_status`: `last_print_started_here` names the file, `age_s` is small, all three options are `true`. The user opens the dialog again: all three are ticked. They press Cancel, and cancel the print on the printer or let it finish.

- [ ] **Step 3: B, a stored file, every option explicit (a real print; ask first)**

`list_printer_files`, then `print_printer_file` with the shortest stored file (the screen showed `cube_PLA_14m53s.gcode.3mf`) and `leveling_before_print: true`, `flow_calibration: true`, `time_lapse: true`. Expected: `status: success`, and `print_options` with all three `caller` / on. The user checks that flow calibration and leveling run, and, once the print ends, whether a time-lapse was recorded (the printer's time-lapse list; see the Flashforge wiki's "Monitoring & Timelapse" page).

- [ ] **Step 4: C, upload only, options left to the gate (no print)**

With the cube still sliced: `send_to_printer` with `start_print: false` and `file_name: "orcamcp-options-check.gcode"`. Expected: `print_options.leveling` and `print_options.flow_calibration` are `{"on": false, "decided_by": "print_time_gate"}`, and `estimated_print_s` is under `gate_s`. Then `get_printer_status`: `last_print_started_here` still names step B's file, so an upload-only send was not recorded (Review Focus 4). The API cannot delete files, so tell the user to remove `orcamcp-options-check.gcode` on the printer's screen.

- [ ] **Step 5: D, a bad parameter (no print)**

`print_printer_file` with `file_name` set to step B's file and `flow_calibration: "yes"`. Expected: the error `flow_calibration must be a boolean`, and nothing starts.

- [ ] **Step 6: Act on step B's time-lapse result**

**If a time-lapse was recorded:** in `docs/printers/flashforge-lan-api.md`, section 4, replace the `printGcode` paragraph with

```markdown
`printGcode` starts a stored file and takes a file name, `levelingBeforePrint`,
`flowCalibration`, `timeLapseVideo` (all booleans), and an optional material-mapping array
pairing project tools to material-station slots. The three switches are the ones the printer's
start screen offers; all three were seen to take effect on firmware 1.9.9.
```

Put the day of step B in parentheses after "1.9.9", and use the firmware version `get_printer_status` reports if it is no longer 1.9.9.

and delete the "Not in the printGcode body..." comment in `make_print_gcode_payload`.

**If `printGcode` refused the request, or no time-lapse was recorded:** the printer's API cannot ask for one on a stored file. Then:
- In `make_print_gcode_payload`, delete the `timeLapseVideo` line and its comment, and in its test delete the two `timeLapseVideo` checks.
- In `print_printer_file`'s handler, right after `read_print_option_request`, add

```cpp
            if (option_request.time_lapse.value_or(false))
                return error_response("The printer's API cannot record a time-lapse for a file already on the printer. "
                                      "Start it from the printer's screen, or send the plate again with send_to_printer "
                                      "and time_lapse: true.");
```

- Change the `time_lapse` property's description in `print_printer_file` to `"Not supported for a stored file: true is refused. Use send_to_printer's time_lapse, or the printer's screen."`, regenerate the golden file (Task 3, Step 7), and update `print_printer_file`'s `time_lapse` row in `docs/tools/reference.md` the same way.
- In `docs/printers/flashforge-lan-api.md`, section 4, record that `printGcode` takes `levelingBeforePrint` and `flowCalibration` but not a time-lapse (firmware, date).

Either way, in section 7's findings paragraph, add what steps A and B showed, with the date and firmware version, and the minutes flow calibration and leveling took. If those minutes make 4 h look wrong, tell the user before changing `kCalibrationGateSeconds`.

- [ ] **Step 7: Commit, and hand over**

```bash
git add -A docs/printers/flashforge-lan-api.md src/slic3r/Utils/FlashforgeApi.cpp tests/slic3rutils/test_flashforge_api.cpp src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp scripts/orcamcp_tools.json docs/tools/reference.md
git commit -m "flashforge: what the Creator 5 Pro does with each print option, checked on the printer

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Run the whole suite once (`slic3rutils_tests` with no tag, then the Python tests), then use superpowers:finishing-a-development-branch. Merge into `mcp` only with the user's word, the way the release README describes (review, rebase onto `mcp`, fast-forward). Do not push without their word.

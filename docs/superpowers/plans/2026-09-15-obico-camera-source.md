# Obico Camera Source for the Flashforge Console — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a Flashforge physical printer name an Obico server, and make the Device console show every camera Obico advertises (played from the agent's full-rate re-served streams) plus Obico's watch state, falling back to today's direct camera when no Obico is configured.

**Architecture:** Two new preset keys flow from the physical-printer dialog through the existing `PollSession.identity` into every snapshot the console page receives. The page opens Obico's token-authenticated websocket itself (native `WebSocket` in the web view), keeps the latest printer document, and a pure chooser picks the camera source in order: advertised stream → Obico snapshot → printer's own stream → unavailable. C++ does no Obico networking; one small pure module (`ObicoLink`) turns config into the page's link object and the MCP's redacted status, and is unit-tested with Catch2.

**Tech Stack:** C++17 (libslic3r config, wxWidgets dialog, nlohmann::json), Catch2 tests in `tests/slic3rutils`, vanilla JS/CSS in `resources/web/flashforge/index.html`, Playwright (MCP plugin) plus a throwaway Python fake-Obico server for page verification.

**Spec:** `docs/superpowers/specs/2026-09-15-obico-camera-source-design.md`. Agent side (already deployed): `~/Projects/flashforge-obico`.

## Global Constraints

- The Obico token is a credential: it is stored like `printhost_apikey`, never logged, never returned by any MCP tool, never placed in a test fixture as a real value. The page receives it only to open the socket.
- With both new keys empty, behaviour must be byte-for-byte today's: no socket, no strip, no watch line, camera straight from `p.camera_stream_url`.
- Follow the file's conventions: `snake_case` functions, doc comments that say *why*, the page's "create the `<img>` once, only touch `src`" rule.
- Build/test command: `cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests && build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[obico]"`. The full app: `--target OrcaSlicer`.
- Commit after every task, message ending with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. Branch: `sync-upstream-2.5` (the user's working branch for this fork).

---

## File structure

```
src/libslic3r/PrintConfig.cpp                 + two option defs (after flashforge_serial_number)
src/libslic3r/Preset.cpp                      + both keys in the two printer-option lists
src/slic3r/Utils/ObicoLink.hpp / .cpp          NEW: pure config → link json / status json
src/slic3r/CMakeLists.txt                     + ObicoLink.{hpp,cpp}
src/slic3r/GUI/PhysicalPrinterDialog.cpp      + fields, show/hide, key list
src/slic3r/GUI/FlashforgeConsoleHandler.cpp   + identity["obico"]
src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.hpp/.cpp   save_print_host_preset: two optional params
src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp        add_physical_printer params; get_printer_status "obico"
resources/web/flashforge/index.html           obicoLink module, camera chooser, strip, watch line, CSS
tests/slic3rutils/test_obico_link.cpp         NEW
tests/slic3rutils/CMakeLists.txt              + test_obico_link.cpp
docs/printers/flashforge-creator-5.md, docs/printers/flashforge-lan-api.md, docs/tools/reference.md, CLAUDE.md
```

---

### Task 1: `ObicoLink` pure module with tests

**Files:**
- Create: `src/slic3r/Utils/ObicoLink.hpp`, `src/slic3r/Utils/ObicoLink.cpp`, `tests/slic3rutils/test_obico_link.cpp`
- Modify: `src/slic3r/CMakeLists.txt:740` (after `Utils/FlashforgeApi.hpp`), `tests/slic3rutils/CMakeLists.txt:23` (after `test_flashforge_live.cpp`), `src/libslic3r/PrintConfig.cpp:1020-1025` (after the `flashforge_serial_number` def), `src/libslic3r/Preset.cpp:1439` and `:4056`

**Interfaces:**
- Produces:
  ```cpp
  namespace Slic3r {
  constexpr const char* OBICO_URL_KEY   = "flashforge_obico_url";
  constexpr const char* OBICO_TOKEN_KEY = "flashforge_obico_token";
  /// {"url": <trimmed, no trailing slash>, "token": <trimmed>} when both keys are set, else null.
  nlohmann::json obico_link_json(const DynamicPrintConfig& config);
  /// {"configured": bool, "url": <url or null>} — never the token. For MCP and logs.
  nlohmann::json obico_status_json(const DynamicPrintConfig& config);
  }
  ```

- [ ] **Step 1: Define the two preset keys** in `PrintConfig.cpp` right after the `flashforge_serial_number` block:

```cpp
    def = this->add("flashforge_obico_url", coString);
    def->label = L("Obico server URL");
    def->tooltip = L("Optional. A self-hosted Obico server that watches this printer through the flashforge-obico "
                     "agent, e.g. http://10.0.0.2:3334. When set, the device console shows the cameras Obico "
                     "advertises and Obico's failure-detection state. Leave empty to view the printer's camera directly.");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString());

    def = this->add("flashforge_obico_token", coString);
    def->label = L("Obico printer token");
    def->tooltip = L("The printer's auth token in Obico (the same one the flashforge-obico agent uses). "
                     "Required together with the Obico server URL.");
    def->mode = comAdvanced;
    def->cli = ConfigOptionDef::nocli;
    def->set_default_value(new ConfigOptionString());
```

- [ ] **Step 2: Register the keys** in `Preset.cpp`: in the list at line ~1439 change `"printhost_apikey", "flashforge_serial_number",` to `"printhost_apikey", "flashforge_serial_number", "flashforge_obico_url", "flashforge_obico_token",`; in the list at line ~4056 add two lines `"flashforge_obico_url",` and `"flashforge_obico_token",` after `"flashforge_serial_number",`.

- [ ] **Step 3: Write the failing test** `tests/slic3rutils/test_obico_link.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/ObicoLink.hpp"

using json = nlohmann::json;
using namespace Slic3r;

namespace {
DynamicPrintConfig config_with(const std::string& url, const std::string& token)
{
    DynamicPrintConfig config;
    config.set_key_value(OBICO_URL_KEY, new ConfigOptionString(url));
    config.set_key_value(OBICO_TOKEN_KEY, new ConfigOptionString(token));
    return config;
}
} // namespace

TEST_CASE("the Obico preset keys exist as advanced strings", "[obico]")
{
    const ConfigOptionDef* url   = print_config_def.get(OBICO_URL_KEY);
    const ConfigOptionDef* token = print_config_def.get(OBICO_TOKEN_KEY);
    REQUIRE(url != nullptr);
    REQUIRE(token != nullptr);
    CHECK(url->type == coString);
    CHECK(token->type == coString);
    CHECK(url->mode == comAdvanced);
}

TEST_CASE("obico_link_json needs both keys and normalises the URL", "[obico]")
{
    CHECK(obico_link_json(config_with("http://10.0.0.2:3334/", " tok ")) ==
          json{{"url", "http://10.0.0.2:3334"}, {"token", "tok"}});
    CHECK(obico_link_json(config_with("http://10.0.0.2:3334", "")).is_null());
    CHECK(obico_link_json(config_with("", "tok")).is_null());
    CHECK(obico_link_json(config_with("   ", "tok")).is_null());
    CHECK(obico_link_json(DynamicPrintConfig()).is_null());
}

TEST_CASE("obico_status_json never carries the token", "[obico]")
{
    const json configured = obico_status_json(config_with("https://obico.example/", "secret"));
    CHECK(configured == json{{"configured", true}, {"url", "https://obico.example"}});
    CHECK(configured.dump().find("secret") == std::string::npos);
    CHECK(obico_status_json(config_with("", "")) == json{{"configured", false}, {"url", nullptr}});
    CHECK(obico_status_json(config_with("http://x", "")) == json{{"configured", false}, {"url", nullptr}});
}
```

  Add `test_obico_link.cpp` to `tests/slic3rutils/CMakeLists.txt` after `test_flashforge_live.cpp`.

- [ ] **Step 4: Build → fails** (missing header). Run the build command from Global Constraints; expected: compile error `ObicoLink.hpp not found`.

- [ ] **Step 5: Implement.** `src/slic3r/Utils/ObicoLink.hpp`:

```cpp
#ifndef slic3r_ObicoLink_hpp_
#define slic3r_ObicoLink_hpp_

#include <nlohmann/json_fwd.hpp>

namespace Slic3r {

class DynamicPrintConfig;

// A Flashforge physical printer may name a self-hosted Obico server (see
// docs/superpowers/specs/2026-09-15-obico-camera-source-design.md). Obico is where the printer's
// cameras and failure-detection state are read from once the flashforge-obico agent owns the
// printer's single-client camera stream. These two helpers are the only place the preset keys are
// interpreted, so the "both or nothing" rule and the redaction rule live in one spot.
constexpr const char* OBICO_URL_KEY   = "flashforge_obico_url";
constexpr const char* OBICO_TOKEN_KEY = "flashforge_obico_token";

/// What the console page needs to open Obico's token-authenticated websocket:
/// {"url": <base URL, trimmed, no trailing slash>, "token": <trimmed>}. Null unless both keys are
/// set; the page treats null as "no Obico" and draws the camera straight from the printer.
nlohmann::json obico_link_json(const DynamicPrintConfig& config);

/// {"configured": <bool>, "url": <base URL or null>} — safe to return from an MCP tool or write to
/// a log, because the token is deliberately not in it.
nlohmann::json obico_status_json(const DynamicPrintConfig& config);

} // namespace Slic3r

#endif // slic3r_ObicoLink_hpp_
```

  `src/slic3r/Utils/ObicoLink.cpp`:

```cpp
#include "ObicoLink.hpp"

#include "libslic3r/PrintConfig.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {

namespace {

std::string trimmed_option(const DynamicPrintConfig& config, const char* key)
{
    const auto* option = config.option<ConfigOptionString>(key);
    if (option == nullptr)
        return {};
    std::string value = option->value;
    boost::algorithm::trim(value);
    return value;
}

std::string base_url(const DynamicPrintConfig& config)
{
    std::string url = trimmed_option(config, OBICO_URL_KEY);
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    return url;
}

} // namespace

nlohmann::json obico_link_json(const DynamicPrintConfig& config)
{
    const std::string url   = base_url(config);
    const std::string token = trimmed_option(config, OBICO_TOKEN_KEY);
    if (url.empty() || token.empty())
        return nullptr;
    return nlohmann::json{{"url", url}, {"token", token}};
}

nlohmann::json obico_status_json(const DynamicPrintConfig& config)
{
    const nlohmann::json link = obico_link_json(config);
    if (link.is_null())
        return nlohmann::json{{"configured", false}, {"url", nullptr}};
    return nlohmann::json{{"configured", true}, {"url", link["url"]}};
}

} // namespace Slic3r
```

  Add `Utils/ObicoLink.cpp` and `Utils/ObicoLink.hpp` to `src/slic3r/CMakeLists.txt` after the `FlashforgeApi` lines.

- [ ] **Step 6: Build and run** `slic3rutils_tests "[obico]"` → 3 test cases pass. Also run `"[flashforge]"` to confirm nothing else moved.

- [ ] **Step 7: Commit** `feat: Obico server URL and token as Flashforge printer preset keys`.

---

### Task 2: Physical printer dialog fields

**Files:**
- Modify: `src/slic3r/GUI/PhysicalPrinterDialog.cpp:331-333` (append options), `:700-711` (show/hide), `:804` (`check_host_key_valid` key list)

- [ ] **Step 1: Append the two options** right after the `flashforge_serial_number` option is appended (line ~333):

```cpp
    option = m_optgroup->get_option("flashforge_obico_url");
    option.opt.width = Field::def_width_wider();
    m_optgroup->append_single_option_line(option);

    option = m_optgroup->get_option("flashforge_obico_token");
    option.opt.width = Field::def_width_wider();
    m_optgroup->append_single_option_line(option);
```

- [ ] **Step 2: Show/hide with the serial number.** In the `host_type` change handler (lines ~700-711), everywhere `flashforge_serial_number` is shown or hidden, do the same for both new keys:

```cpp
        if (opt->value == htFlashforge) {
            m_optgroup->show_field("printhost_apikey");
            m_optgroup->show_field("flashforge_serial_number");
            m_optgroup->show_field("flashforge_obico_url");
            m_optgroup->show_field("flashforge_obico_token");
            m_optgroup->hide_field("printhost_authorization_type");
        } else {
            m_optgroup->hide_field("flashforge_serial_number");
            m_optgroup->hide_field("flashforge_obico_url");
            m_optgroup->hide_field("flashforge_obico_token");
        }
```
  and in the non-physical branch (line ~711) hide both alongside `flashforge_serial_number`.

- [ ] **Step 3: Key list.** Add `"flashforge_obico_url", "flashforge_obico_token"` to the `keys` vector in `check_host_key_valid()` (line 804) so a preset saved before this change gets empty strings rather than missing options.

- [ ] **Step 4: Validation of "both or neither".** `OnOK` saves unconditionally today; add a guard before `save_preset`:

```cpp
void PhysicalPrinterDialog::OnOK(wxEvent& event)
{
    const std::string obico_url   = m_config->opt_string("flashforge_obico_url");
    const std::string obico_token = m_config->opt_string("flashforge_obico_token");
    if (obico_url.empty() != obico_token.empty()) {
        show_error(this, _L("Obico needs both a server URL and a printer token. Fill in both, or clear both."));
        return;
    }
    wxGetApp().get_tab(Preset::TYPE_PRINTER)->save_preset("", false, false, true, m_preset_name);
    event.Skip();
}
```
  (`show_error` is already available from `GUI.hpp` in this file; check the include list and add `#include "GUI.hpp"` if it is missing.)

- [ ] **Step 5: Build `OrcaSlicer` target** (background, ~10-20 min). Meanwhile continue with Task 3. When done, launch the app, open the C5P printer's connection dialog, confirm the two fields appear only for FlashForge, and that saving with one filled is refused.

- [ ] **Step 6: Commit** `feat: Obico fields in the Flashforge physical printer dialog`.

---

### Task 3: Handler passes the link; MCP reports and sets it

**Files:**
- Modify: `src/slic3r/GUI/FlashforgeConsoleHandler.cpp:~510` (`start_polling`, identity), `src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterUtils.hpp:137` and `.cpp:586-623` (`save_print_host_preset`), `src/slic3r/GUI/OrcaMCP/OrcaMCPPrinterTools.cpp:570-680` (`add_physical_printer` schema/handler, `get_printer_status` response)
- Test: extend `tests/slic3rutils/test_obico_link.cpp` (no new C++ test is possible for the handler without a GUI; the identity change is one line over a tested function)

- [ ] **Step 1: Handler.** Include `"slic3r/Utils/ObicoLink.hpp"` and in `start_polling`, after `session->identity = json{{"preset", preset}, {"host", config.opt_string("print_host")}};` add:

```cpp
        // The page opens Obico's websocket itself; this is the only place the token leaves the preset,
        // and it goes to our own file:// page, the same way other vendors' device pages get their keys.
        if (const json obico = obico_link_json(config); !obico.is_null())
            session->identity["obico"] = obico;
```

- [ ] **Step 2: `save_print_host_preset`** gains two trailing parameters `const std::optional<std::string>& obico_url, const std::optional<std::string>& obico_token` (update the declaration in the `.hpp` and the call in `add_physical_printer`). Inside, next to `printhost_apikey`:

```cpp
    if (obico_url.has_value())
        settings[OBICO_URL_KEY] = *obico_url;
    if (obico_token.has_value())
        settings[OBICO_TOKEN_KEY] = *obico_token;
```
  Include `"slic3r/Utils/ObicoLink.hpp"` in the `.cpp`.

- [ ] **Step 3: `add_physical_printer` schema** — add after `api_key`:

```cpp
                {"obico_url", {
                    {"type", "string"},
                    {"description", "Flashforge only: base URL of a self-hosted Obico server that watches this printer "
                                    "(e.g. http://10.0.0.2:3334). Set together with obico_token; pass \"\" for both to clear."}
                }},
                {"obico_token", {
                    {"type", "string"},
                    {"description", "Flashforge only: the printer's Obico auth token. Never returned by any tool."}
                }},
```
  Handler: `const std::optional<std::string> obico_url = optional_string(params, "obico_url"); const std::optional<std::string> obico_token = optional_string(params, "obico_token");` and, before the main-thread call, `if (obico_url.has_value() != obico_token.has_value()) return error_response("obico_url and obico_token must be given together");` and `if (obico_url.has_value() && obico_url->empty() != obico_token->empty()) return error_response("Obico needs both a server URL and a printer token; pass \"\" for both to clear");`. Capture both in the lambda and pass them through.

- [ ] **Step 4: `get_printer_status`** — add `{"obico", obico_status_json(cfg)}` to the Flashforge success response (and to the non-Flashforge one as `{"obico", obico_status_json(cfg)}` too; it will say not configured). Include the header.

- [ ] **Step 5: Build tests + app**, run `"[obico]"` and `"[flashforge]"`. With the app running and the MCP bridge up, call `get_printer_status` through the MCP tool and confirm the `obico` block is present and has no token. Call `add_physical_printer` with the C5P values plus `obico_url`/`obico_token` read from the deployed agent's `.env` on Unraid (never echoed into the conversation: pipe them through a shell variable into the tool call is not possible, so this step is **done by the user in the dialog**; the MCP path is verified with a throwaway preset name and dummy values `http://127.0.0.1:1` / `dummy`, then that preset is deleted).

- [ ] **Step 6: Commit** `feat: console receives the Obico link; MCP reports and sets it without exposing the token`.

---

### Task 4: The page — Obico link module

**Files:**
- Modify: `resources/web/flashforge/index.html` (new block after the bridge is created, ~line 522; hooks in `receive` ~line 493 and `render` ~line 1693)

**Interfaces:**
- Produces (page globals inside the IIFE):
  ```js
  obicoLink.sync(s)        // called from render(); opens/closes/reconnects per s.obico and `suspended`
  obicoLink.close()        // called from receive() on "suspended"
  obicoLink.doc()          // latest Obico printer document or null
  obicoLink.state()        // "off" | "connecting" | "connected" | "unreachable" | "rejected"
  ```

- [ ] **Step 1: Add the module** just after `var bridge = ...` (line 522):

```js
  /* ══ Obico ════════════════════════════════════════════════════════════
     A Flashforge preset may name an Obico server (flashforge_obico_url/_token). Obico is where the
     cameras come from once the flashforge-obico agent owns the printer's single-client stream: the
     agent advertises each camera's re-served address in Obico's webcam list, and Obico pushes that
     list, the latest snapshot and its watch state over a websocket that authenticates with the
     printer's token alone. The page opens that socket itself; the slicer only hands over the link. */
  var obicoLink = (function(){
    var link = null, ws = null, doc = null, state = "off", timer = null;
    var backoffMs = 2000, closesBeforeDoc = 0;
    var BACKOFF_MAX_MS = 60000, REJECTED_AFTER_CLOSES = 2;

    function wsUrl(l){
      return l.url.replace(/^http/i, "ws").replace(/\/+$/, "") + "/ws/token/web/" + encodeURIComponent(l.token) + "/";
    }
    function sameLink(a, b){ return !!a && !!b && a.url === b.url && a.token === b.token; }
    function repaint(){ if (lastState) render(lastState); }

    function open(){
      if (ws || !link) return;
      state = doc ? state : "connecting";
      var socket;
      try { socket = new WebSocket(wsUrl(link)); }
      catch (e){ onClosed(); return; }
      ws = socket;
      socket.onmessage = function(ev){
        var parsed;
        try { parsed = JSON.parse(ev.data); } catch (e){ return; }
        if (!parsed || typeof parsed !== "object") return;
        doc = parsed; state = "connected"; backoffMs = 2000; closesBeforeDoc = 0;
        repaint();
      };
      socket.onclose = function(){ if (ws === socket) onClosed(); };
      socket.onerror = function(){ /* onclose follows */ };
    }
    function onClosed(){
      ws = null;
      if (!link) return;
      if (state === "connecting" && ++closesBeforeDoc >= REJECTED_AFTER_CLOSES){
        state = "rejected";          // closed twice before any document: the token is not accepted
        repaint();
        return;
      }
      state = "unreachable";
      repaint();
      timer = setTimeout(function(){ timer = null; open(); }, backoffMs);
      backoffMs = Math.min(backoffMs * 2, BACKOFF_MAX_MS);
    }
    function close(){
      if (timer){ clearTimeout(timer); timer = null; }
      var socket = ws; ws = null;
      if (socket){ try { socket.close(); } catch (e){} }
      doc = null; state = "off"; backoffMs = 2000; closesBeforeDoc = 0;
    }
    function sync(s){
      var wanted = (s && s.obico && s.obico.url && s.obico.token && !suspended) ? s.obico : null;
      if (!wanted){ if (link){ link = null; close(); } return; }
      if (sameLink(link, wanted)) { if (!ws && !timer && state !== "rejected") open(); return; }
      close(); link = wanted; open();
    }
    return { sync: sync, close: function(){ link = null; close(); }, doc: function(){ return doc; }, state: function(){ return state; } };
  })();
```

  Note `suspended` and `lastState` are declared later in the file with `var` (hoisted) and only read at call time, so the module can reference them.

- [ ] **Step 2: Hooks.** In `receive()`, inside the `msg.method === "suspended"` branch before `if (lastState) render(lastState)`, add `obicoLink.close();`. In `render(s)`, as the first statement after `lastState = s;`, add `obicoLink.sync(s);`.

- [ ] **Step 3: Verify in a browser** (Task 6 has the harness); for now just open the file in Chrome (`open resources/web/flashforge/index.html`) and confirm the console shows no script error and SAMPLE still renders.

- [ ] **Step 4: Commit** `feat(console): keep a token-authenticated websocket to the printer's Obico server`.

---

### Task 5: The page — camera chooser, strip and watch line

**Files:**
- Modify: `resources/web/flashforge/index.html`: markup (~line 401-413), CSS (~line 303-306), `renderCamera` (961-1007), `renderBanner` (784-789)

- [ ] **Step 1: Markup.** Inside `<div class="card camera">`, after the `camfoot` div, add:

```html
      <div class="camstrip" id="camStrip" hidden></div>
      <div class="watch" id="camWatch" hidden></div>
```

- [ ] **Step 2: CSS**, after `.camoff{...}`:

```css
.camstrip{display:flex;gap:8px;padding:8px 12px 0;overflow-x:auto}
.camstrip .thumb{position:relative;flex:0 0 auto;width:96px;aspect-ratio:16/10;border-radius:6px;overflow:hidden;
  border:1px solid var(--edge);cursor:pointer;opacity:.72;background:#0a0d12}
.camstrip .thumb img{width:100%;height:100%;object-fit:cover;display:block}
.camstrip .thumb span{position:absolute;left:0;right:0;bottom:0;padding:2px 6px;font-size:10px;color:#E9EDF3;
  background:rgba(8,11,15,.62);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.camstrip .thumb[data-selected="1"]{opacity:1;border-color:var(--brass)}
.watch{padding:8px 12px 10px;font-size:11.5px;line-height:1.45;color:var(--muted)}
.watch[data-kind="warn"]{color:var(--warn)}
.camtag{position:absolute;right:12px;bottom:12px;padding:4px 8px;border-radius:6px;background:rgba(8,11,15,.62);
  border:1px solid rgba(233,237,243,.14);color:#C8D0DC;font-size:10.5px}
```

- [ ] **Step 3: Pure helpers** (place just above `renderCamera`):

```js
  /* The ordered list of places the camera picture can come from. Each entry is
     {kind, url, name, primary}. `failed` holds URLs whose <img> errored since the last Obico push. */
  function cameraSources(s, p, doc){
    var out = [];
    var cams = (doc && doc.settings && Array.isArray(doc.settings.webcams)) ? doc.settings.webcams : [];
    cams.filter(function(w){ return w && w.stream_url; })
        .sort(function(a, b){ return (b.is_primary_camera ? 1 : 0) - (a.is_primary_camera ? 1 : 0); })
        .forEach(function(w){ out.push({ kind:"obico-stream", url:w.stream_url, name:w.name || "Primary", primary:!!w.is_primary_camera }); });
    if (doc && doc.pic && doc.pic.img_url) out.push({ kind:"obico-snapshot", url:doc.pic.img_url, name:"Snapshot via Obico", primary:true });
    if (s.connected && p.camera_stream_url) out.push({ kind:"printer", url:p.camera_stream_url, name:"Printer", primary:true });
    return out;
  }

  /* Picks what the big viewport shows: the selected Obico camera if it still exists, else the first
     source that has not failed. */
  function chooseCamera(sources, selectedName, failed){
    var streams = sources.filter(function(c){ return c.kind === "obico-stream"; });
    if (selectedName){
      var chosen = streams.filter(function(c){ return c.name === selectedName && !failed[c.url]; })[0];
      if (chosen) return chosen;
    }
    return sources.filter(function(c){ return !failed[c.url]; })[0] || null;
  }

  function obicoWatch(doc, state, sourceKind){
    if (state === "off") return null;
    if (state === "connecting") return { kind:"", text:"Obico: connecting…" };
    if (state === "rejected")   return { kind:"warn", text:"Obico rejected the printer token. Check the Obico printer token in the printer settings." };
    if (state === "unreachable"){
      var showing = sourceKind === "obico-stream" ? "the camera stream" : sourceKind === "obico-snapshot" ? "the last snapshot" : sourceKind === "printer" ? "the printer's own camera" : "no camera";
      return { kind:"warn", text:"Obico unreachable — showing " + showing + "." };
    }
    var cp = doc && doc.current_print;
    if (cp && cp.alerted_at && !cp.alert_muted) return { kind:"warn", text:"Obico has flagged a possible failure." };
    if (!cp) return { kind:"", text:"Obico: watching, idle" };
    var p = num(doc.normalized_p, 0);
    var level = p < 0.33 ? "low" : p < 0.66 ? "medium" : "high";
    return { kind: level === "high" ? "warn" : "", text:"Obico: watching · failure confidence " + level };
  }
```

- [ ] **Step 4: Rewrite `renderCamera`.** Replace the body's camera-image part (the `if (url !== camSrc)` block) with a chooser-driven version. Keep the file/badge lines that follow it unchanged.

```js
  var camSrc = null, camSelected = null, camFailed = {}, camFailedFor = null, stripHtml = null;
  function renderCamera(s, p, raw, pro, printing){
    var doc = obicoLink.doc(), ostate = obicoLink.state();
    /* Failures are forgiven whenever Obico pushes a fresh document: a re-server that was down may be
       back, and the snapshot URL is new every time anyway. */
    var docKey = doc ? (doc.pic && doc.pic.img_url) + "|" + JSON.stringify(doc.settings && doc.settings.webcams) : null;
    if (docKey !== camFailedFor){ camFailed = {}; camFailedFor = docKey; }

    var sources = cameraSources(s, p, doc);
    var chosen  = chooseCamera(sources, camSelected, camFailed);
    var url     = chosen ? chosen.url : "";
    var scene   = el("camScene");
    if (url !== camSrc){
      camSrc = url;
      if (url){
        scene.innerHTML = '<img class="cam" alt="Live camera view of the build plate">' +
          (chosen.kind === "obico-snapshot" ? '<span class="camtag">Snapshot via Obico</span>' : "");
        var img = scene.firstChild;
        show(el("camLive"), chosen.kind !== "obico-snapshot");
        img.onerror = function(){
          camFailed[url] = true;
          camSrc = null;                      // let the next render pick the next source
          if (lastState) render(lastState);
        };
        img.src = url;
      } else {
        show(el("camLive"), false);
        var tried = sources.length ? " Tried: " + sources.map(function(c){ return esc(c.name); }).join(", ") + "." : "";
        scene.innerHTML = '<div class="camoff">' +
          (!s.connected && !doc ? "Waiting for the printer…" :
           sources.length ? "Camera stream unavailable." + tried : "This printer reports no camera stream.") + '</div>';
      }
    }

    var streams = sources.filter(function(c){ return c.kind === "obico-stream"; });
    var strip = el("camStrip");
    show(strip, streams.length > 1);
    var html = streams.length > 1 ? streams.map(function(c){
      var sel = chosen && chosen.url === c.url;
      return '<div class="thumb" data-name="' + esc(c.name) + '" data-selected="' + (sel ? 1 : 0) + '">' +
             '<img alt="" src="' + esc(c.url) + '"><span>' + esc(c.name) + '</span></div>';
    }).join("") : "";
    if (html !== stripHtml){
      stripHtml = html;
      strip.innerHTML = html;
      Array.prototype.forEach.call(strip.querySelectorAll(".thumb"), function(t){
        t.onclick = function(){ camSelected = t.getAttribute("data-name"); if (lastState) render(lastState); };
      });
    } else {
      Array.prototype.forEach.call(strip.querySelectorAll(".thumb"), function(t){
        t.setAttribute("data-selected", chosen && chosen.name === t.getAttribute("data-name") && chosen.kind === "obico-stream" ? "1" : "0");
      });
    }

    var watch = obicoWatch(doc, ostate, chosen ? chosen.kind : null);
    var watchEl = el("camWatch");
    show(watchEl, !!watch);
    if (watch){ setText(watchEl, watch.text); watchEl.setAttribute("data-kind", watch.kind); }

    setText(el("camFile"), printing ? (p.print_file || "Printing") : (p.name || ""));
    /* ...the existing badge code stays as it is... */
```

  Snapshot refresh: when `chosen.kind === "obico-snapshot"` the URL changes on every Obico push (signed URL), so the `url !== camSrc` path naturally re-sets `src`; no timer needed.

- [ ] **Step 5: Banner.** Extend `renderBanner` so an Obico alert shows even while the printer is connected:

```js
  function renderBanner(s){
    var doc = obicoLink.doc(), cp = doc && doc.current_print;
    var alert = (cp && cp.alerted_at && !cp.alert_muted) ?
      '<div class="banner"><b>Obico has flagged a possible failure.</b>&nbsp;Check the camera; pause from Obico or from the controls below.</div>' : "";
    var offline = (s.connected || s.connecting) ? "" :
      '<div class="banner"><b>Printer not reachable.</b>&nbsp;'+
      esc(s.error || "No answer from the printer's local API.")+
      ' OrcaSlicer keeps trying; the page fills in as soon as the printer answers.</div>';
    setHTML(el("banner"), alert + offline);
  }
```

- [ ] **Step 6: No-Obico regression check.** Open the file in Chrome: SAMPLE renders exactly as before (no strip, no watch line; camera says "This printer reports no camera stream." because SAMPLE has an empty `camera_stream_url`).

- [ ] **Step 7: Commit** `feat(console): cameras and watch state from Obico, with fallback to the printer's own stream`.

---

### Task 6: Page verification with a fake Obico and the real re-server

**Files:**
- Create (scratchpad only, not committed): `fake_obico.py`

- [ ] **Step 1: Fake Obico** in the scratchpad, run with `uv run --with websockets fake_obico.py`: an HTTP server on 127.0.0.1:3399 serving `/media/snap.jpg` (any JPEG; grab one from `http://10.0.0.2:8081/cameras/0/snapshot`) and `/broken/stream` (404), and a websocket server on 127.0.0.1:3398 accepting `/ws/token/web/testtoken/` that pushes, every 2 s, a document:

```json
{"status": {"state": {"text": "Printing"}}, "pic": {"img_url": "http://127.0.0.1:3399/media/snap.jpg?t=<n>"},
 "normalized_p": 0.2, "current_print": {"id": 1, "alerted_at": null, "alert_muted": false},
 "settings": {"webcams": [
   {"name": "Printer", "is_primary_camera": true,  "stream_url": "http://10.0.0.2:8081/cameras/0/stream"},
   {"name": "Side",    "is_primary_camera": false, "stream_url": "http://127.0.0.1:3399/broken/stream"}]}}
```
  and closes immediately for any other token path. (Use a single `websockets.serve` and `http.server` in threads; ~60 lines.)

- [ ] **Step 2: Playwright.** Navigate to `file:///Users/hanan/Projects/OrcaMCP/resources/web/flashforge/index.html`, wait 2 s (SAMPLE renders), then `browser_evaluate`:

```js
window.orcaFlashforge.receive({id:0, method:"status", ok:true, result: Object.assign({}, window.__sampleForTest || {}, {
  connected:true, preset:"C5P", host:"10.0.0.10", poll_ms:2000,
  obico:{url:"http://127.0.0.1:3398", token:"testtoken"},
  printer:{state:"ready", camera_stream_url:"http://10.0.0.10:8080/?action=stream", name:"Creator 5 Pro", model:"Creator 5 Pro", pid:41, temperatures:{nozzles:[]}, material_station:{present:false, slots:[]}, raw:{}}
})})
```
  (`SAMPLE` is not on `window`; pass a full minimal status as above instead of relying on it.)
  Then assert via `browser_snapshot`/`browser_evaluate`:
  - `#camWatch` text starts with "Obico: watching · failure confidence low";
  - `#camScene img.cam` has `src` = `http://10.0.0.2:8081/cameras/0/stream` and `naturalWidth > 0` within 5 s (a real frame from the deployed agent);
  - `#camStrip` visible with two thumbs, "Printer" selected;
  - click the "Side" thumb → its stream 404s → `camFailed` marks it and the big view falls back to "Printer" (assert `img.cam` src is again the 10.0.0.2 stream within 2 s);
  - stop the fake websocket server → within ~3 s `#camWatch` says "Obico unreachable — showing the camera stream." and the big image is still the 10.0.0.2 stream (cached webcam list);
  - send a status without `obico` → strip and watch line hidden, `img.cam` src is the printer's own URL.
  - Wrong token (`token:"nope"`) → after two closes `#camWatch` says "Obico rejected the printer token…".

- [ ] **Step 3: Record results** in the PR/commit message body of Task 5 (amend) or a short note in the spec's "Testing" section stating what was verified in the browser and that the in-app pass is pending the user's dialog entry.

---

### Task 7: Documentation

**Files:**
- Modify: `docs/printers/flashforge-creator-5.md` (after "What you can control", and the camera bullet under "Known limitations"), `docs/printers/flashforge-lan-api.md:277-286` (§5), `docs/tools/reference.md` (Printer Tools: document `add_physical_printer` and `get_printer_status` if absent, else add the new fields), `CLAUDE.md` (Printers row: mention Obico params)

- [ ] **Step 1: Creator 5 doc**, new subsection after "### What you can control":

```markdown
### Failure detection with Obico

The printer's own cloud detection is unavailable in LAN mode, and its camera serves exactly one
viewer at a time. The companion [flashforge-obico](https://github.com/okets/flashforge-obico) agent
owns that stream, feeds a self-hosted [Obico](https://www.obico.io/) server, and re-serves every
camera at full frame rate on the LAN.

To let the console use it, fill in two more fields in the printer connection dialog (FlashForge
host type, Advanced mode): **Obico server URL** (e.g. `http://10.0.0.2:3334`) and **Obico printer
token** (the token the agent uses). Both or neither. The console then:

- shows every camera Obico lists for the printer, played from the agent's re-served stream; with
  more than one camera a thumbnail strip appears under the picture and a click swaps cameras;
- shows a line under the camera with Obico's state: connecting, watching with a low/medium/high
  failure confidence, or a warning when Obico has flagged a failure;
- falls back in order: advertised stream → Obico's latest snapshot → the printer's own stream.

Clear both fields and the console draws the camera straight from the printer as before.

Internally these are the preset keys `flashforge_obico_url` and `flashforge_obico_token`;
`add_physical_printer` takes them as `obico_url` / `obico_token`, and `get_printer_status` reports
`obico.configured` and `obico.url` (never the token).
```
  Known-limitations camera bullet → "The printer's camera serves one viewer at a time; while an Obico agent holds it, configure Obico here so the console reads the re-served stream. It is not available through the Bambu-style live-view player…".

- [ ] **Step 2: LAN API doc §5** — replace the "Test this before you design around it" paragraph with the verified facts: single client (second connection reset within ~10 ms, three of three trials), no `?action=snapshot` (empty reply), and that the bridge must be the sole consumer and re-serve frames onward, pointing at flashforge-obico.

- [ ] **Step 3: Tools reference** — under "## Printer Tools" add (or extend) `### add_physical_printer` with a parameter table including `obico_url` / `obico_token`, and `### get_printer_status` with the `obico` block in the example response.

- [ ] **Step 4: CLAUDE.md** — in the Printers row of the tools table, after `add_physical_printer`, add "(incl. optional Obico URL/token for Flashforge)". Add a Key Files row for `src/slic3r/Utils/ObicoLink.cpp`.

- [ ] **Step 5: Commit** `docs: Obico as the Flashforge console's camera and watch source`.

---

## Self-review

**Spec coverage:** §3.1 keys → Task 1 (defs, lists) + Task 2 (dialog, validation). §3.2 identity → Task 3. §3.3 module → Task 4 (connect/reconnect/expose/close on suspend; token-rejected detection). §3.4 chooser + strip + single-`<img>` rule → Task 5. §3.5 watch line and alert banner → Task 5. §3.6 MCP read-only + add_physical_printer → Task 3. §3.7 docs → Task 7. §4 failures → Tasks 4/5 (unreachable keeps cached webcams; rejected stops reconnecting; failed URLs forgiven on next push). §5 testing: C++ → Task 1; page → Task 6 (browser harness replaces the "pure functions evaluated from Catch2" idea, which is not feasible without a JS engine — the spec allowed a hand-run checklist; this is an automated one); first-thing-to-verify websocket-from-file → Task 6 covers WebKit-family Chromium via Playwright; the wx web view itself is confirmed in Task 2 Step 5 / user acceptance.

**Placeholders:** none. **Type consistency:** `obico_link_json`/`obico_status_json`/`OBICO_URL_KEY`/`OBICO_TOKEN_KEY` used identically in Tasks 1, 3; page helpers `cameraSources`, `chooseCamera`, `obicoWatch`, `obicoLink.{sync,close,doc,state}` consistent between Tasks 4, 5, 6.

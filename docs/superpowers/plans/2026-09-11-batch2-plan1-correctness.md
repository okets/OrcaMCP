# Batch 2, Plan 1 — Correctness fixes (T1–T4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the four defects the 2026-09-10 MCP testing session found — a JSON array rejected for a list-typed config key, a `get_presets` response too big to answer, a live app reported as "not running", and an exact colour match reported as an error — and mark out-of-gamut colour results.

**Architecture:** Each fix pulls its decision out of the handler into a pure free function that Catch2 can drive without a GUI, then rewires the handler to call it. Three pure units are added: `config_value_to_string` / `config_value_expected_shape` (JSON value → the text `ConfigOption::deserialize` actually expects, per option type), the `get_presets` cap and hint, and a new `OrcaMCPColorRecipe` module for the colour-recipe and gamut decisions. The two bridge fixes are Python-side only and are covered by stdlib `unittest` tests that patch `urllib.request.urlopen`.

**Tech Stack:** C++17, nlohmann::json, libslic3r `ConfigOption` / `DynamicPrintConfig`, wxWidgets (handlers only), Catch2 v3 (`tests/slic3rutils/`), Python 3 stdlib (`scripts/orcamcp-bridge.py`, `scripts/tests/` with `unittest`), CMake.

**Spec:** docs/superpowers/plans/2026-09-10-batch-2-spec.md

## Global Constraints

Every task below inherits these. They are copied verbatim from the spec's Global Constraints section.

- **Branch:** `sync-upstream-2.5`. Do not push. Do not create branches.
- **Commit style:** `fix:` / `feat:` + what changed, past tense. The body names the **root cause**, not the symptom, and lists related occurrences checked — *including the ones deliberately left alone, and why*. Read `git log -5` for the bar. Every commit ends with:
  `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`
- **Threading:** every tool handler body runs inside `run_on_main_thread()` (`OrcaMCPCommon.hpp`). Anything touching `Model`, `Plater` or the preset bundle must be inside it. A modal opened inside it hangs the GUI forever.
- **Dialogs:** never call `set_mcp_dialog_suppression()` directly. `McpDialogSuppressionGuard` (RAII, nest-safe) is the only sanctioned way.
- **Pure logic goes in a free function with Catch2 coverage** under `tests/slic3rutils/`. This project tests logic without a printer wherever possible; geometry maths must be testable without a GUI.
- **Undo/redo:** any tool that mutates the model must take a snapshot so `undo` works, the way the existing transform tools do. Check how `move_object` does it and follow it. *(No task in this plan mutates the model; nothing here needs a snapshot.)*
- **Registration:** tools are registered with `register_tool({...})` in `register_builtin_tools()` (`OrcaMCPServer.cpp`) or the per-area registrars (`OrcaMCPFilamentTools.cpp`, `OrcaMCPPrinterTools.cpp`). New tool groups get their own file following that pattern, added to `src/slic3r/CMakeLists.txt`.
- **Docs:** every new or changed tool updates `docs/tools/reference.md`, and the tool table plus the count in `CLAUDE.md`. The count command is in CLAUDE.md. *(This plan registers **no new tool**, so the count stays 70 / 71 reachable and the `CLAUDE.md` tool table needs no edit. `docs/tools/reference.md` does change — five tools change behaviour.)*
- **Response shape:** `{"status": "success"|"error"|"partial", ...}` plus `active_warnings` on anything that mutates the scene, matching existing tools.
- **Verify before reporting:**
  ```
  cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8
  build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
  ```
  Baseline at the time of writing: **174 cases / 1195 assertions / 0 failures**. The skipped count varies run to run (bundled-Python and numpy cases) — that is expected, not a regression.
- **Do not start the app or contact the printer** unless the plan says to. *(No task in this plan starts the app.)*

---

## What the investigation found

Read this before Task 1. Every task below is written against these facts, which were checked in the source, not assumed.

### T1 — why the array is rejected (and the two places it is worse than rejected)

`ApplyConfig` (`src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp:224`) flattens every JSON value to text at line 264:

```cpp
const std::string value_str = value.is_string() ? value.get<std::string>() : value.dump();
```

So `["#00FFFF","#FF00FF","#FFFF00","#808080"]` becomes the literal text `["#00FFFF","#FF00FF","#FFFF00","#808080"]` and is handed to `config->set_deserialize(key, value_str, context)` at line 278. What happens next depends entirely on the option type, and **none of the four outcomes is "rejected with a reason"**:

| Key (type) | What `deserialize` does with the array literal | Observed result |
|---|---|---|
| `filament_colour` (`coStrings`) | `unescape_strings_cstyle` (`src/libslic3r/Config.cpp:149`) finds no `;`, so it stores the **whole literal as one string** and returns `true`. No throw. | Then the sweep-item-I check at line 285, `color_option_is_valid`, sees a value that is not `#RRGGBB`, restores `previous` (line 289) and pushes the key into `result.invalid`. **Sweep item I is what rejects it.** |
| `filament_notes`, `filament_mixed_components` (`coStrings`, not colour) | Same: one garbage string, returns `true`. No colour check to catch it. | **Silently accepted and stored as garbage.** `status: "success"`. |
| `flush_volumes_vector`, `flush_volumes_matrix` (`coFloats`) | `ConfigOptionFloatsTempl::deserialize` (`src/libslic3r/Config.hpp:911`) splits on **`,`**, `iss >> value` fails on the element carrying the `[`, leaves `0`, pushes it, and **`return true;` unconditionally** (line 935). | **Silently accepted and stored as wrong numbers.** `status: "success"`. |
| `filament_map` (`coInts`), `filament_is_mixed` (`coBools`) | `ConfigOptionIntsTempl::deserialize` (`Config.hpp:1089`) and `ConfigOptionBoolsTempl::deserialize_with_substitutions` (`Config.hpp:1959`) both split on **`,`** and reject the bracketed element. | `BadOptionValueException` → caught at line 279 → `result.invalid`. |

Two consequences the spec's fix direction did not know about:

1. **The spec says "join with `;`". That is only right for `coStrings`.** `coInts`, `coFloats`, `coBools`, `coPercents`, `coFloatsOrPercents` and `coPoints` all split on `,`. Joining an int array with `;` would produce one unparsable token. The separator must come from the option type.
2. **Before sweep item I, the `filament_colour` array was silently accepted as one garbage string.** Item I turned a silent corruption into a rejection for colour keys only; the same corruption is still live for every non-colour `coStrings` key and for every `coFloats` key.

`set_object_config` (`src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:1694-1711`) has the identical `is_string() ? get : dump()` line and the identical problem. The `print` / `filament` / `printer` / `project` branches of `apply_config` all funnel into the same `ApplyConfig` loop, so fixing the loop fixes all four.

`ApplyConfigResult::invalid` (`OrcaMCPPresetConfigUtils.hpp:29-38`) is read by `OrcaMCPPrinterUtils.cpp:625`, so it must keep its current meaning.

### T2 — why the filters were unreachable, and it is the bridge

`scripts/tools_schema.py` is a **checked-in static file**, regenerated by hand with `scripts/regen_tools_schema.py` against a running server. Its `get_presets` entry is still the pre-item-H one:

```python
'inputSchema': {'additionalProperties': False, 'properties': {}, 'required': [], 'type': 'object'}
```

The bridge serves that file at `scripts/orcamcp-bridge.py:354-359`: on the **first** `tools/list`, if a 0.1 s connection probe fails — the normal case, because the MCP client starts before OrcaSlicer — it answers from `get_full_tools_list()` and never revisits it. `initialize` (line 325) advertises `"capabilities": {"tools": {}}`, i.e. **no `listChanged`**, so the client has no reason to ask again either. That is exactly the observed `"properties": {}`. It is the bridge, not the client, and it survives an app update because the file is only regenerated by hand.

The unfiltered size problem is separate: `get_presets` (`OrcaMCPServer.cpp:870-941`) has no cap, and its `counts` block reports the true match totals (318 filament presets) only because nothing is truncated.

### T3 — why a live app is reported as not running

`check_orcaslicer_connection` (`scripts/orcamcp-bridge.py:257-279`) does a GET with a **0.3-second** timeout, and its `except Exception` treats *every* failure — refused, timed out, HTTP 500, DNS — as "not connected", then **caches that False for `CONNECTION_CACHE_TTL = 3` seconds** (line 66). `handle_local_request` (line 364) then answers every `tools/call` for those 3 seconds with the fabricated `"OrcaMCP is not running."` (line 380-382).

The server genuinely cannot answer in 0.3 s under load: `HttpServer` runs a **single** `io_service.run()` thread (`src/slic3r/GUI/HttpServer.cpp:210-216`), and every MCP handler blocks that thread inside `run_on_main_thread()`'s `future.get()` (`OrcaMCPCommon.hpp:29`). Requests are strictly serialised — the io thread cannot even accept the probe while a handler waits on the GUI thread. An 8-call burst therefore queues, the probe times out, the verdict sticks for 3 s, and the last calls in the burst get the false "not running". That is the whole mechanism.

### T4 — the exact match

`suggest_color_mix` (`src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp:293-298`) turns a one-component result into an error:

```cpp
if (result.components.size() < 2) {
    const auto& c = result.components.front();
    return nlohmann::json{{"status", "error"}, {"message",
        "target_color already matches physical filament " + std::to_string(c.filament_index) +
        " (" + c.color_hex + "); no mix needed"}};
}
```

`ColorDecomposeRecipeResult` and its components are plain data (`src/libslic3r/ColorDecomposeRecipe.hpp:29-41`) — no wx, no preset bundle — so the decision that turns a result into a recipe is pure and testable.

Neither `suggest_color_mix` nor `get_color_palette` appears anywhere in `docs/tools/reference.md`; there is no filament/colour section at all. Both tools change in this plan, so both get one.

---

## File Structure

**Created**

| File | Responsibility |
|---|---|
| `src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.hpp` / `.cpp` | The pure colour decisions behind `suggest_color_mix` and `get_color_palette`: a recipe from a `ColorDecomposeRecipeResult`, the gamut threshold and label, and which hue sectors a palette cannot reach. Includes only `<string>`, `<vector>` and `libslic3r/ColorDecomposeRecipe.hpp` — deliberately no wx, so a test TU can include it. |
| `tests/slic3rutils/test_config_value_shape.cpp` | Catch2 cover for `config_value_to_string` / `config_value_expected_shape`. |
| `tests/slic3rutils/test_color_recipe.cpp` | Catch2 cover for `OrcaMCPColorRecipe`. |
| `scripts/tests/test_bridge_liveness.py` | `unittest` cover for the bridge's live/busy/down verdict and the advice it gives. |
| `scripts/tests/test_bridge_schema_refresh.py` | `unittest` cover for the `notifications/tools/list_changed` refresh. |

**Modified**

| File | Change |
|---|---|
| `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp` / `.cpp` | New pure `config_value_to_string`, `config_value_expected_shape`, `preset_query_effective_limit`, `preset_truncation_hint`; `PresetQuery` gains `limit`; new `PresetListCount`; `ApplyConfigResult` gains `unknown` / `rejected`; `ApplyConfig` shapes values before deserialising; the three preset-JSON builders report matched vs returned. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp` | `apply_config` and `set_object_config` responses gain `unknown_keys` / `rejected_values`; `set_object_config` uses the shared shaping; `get_presets` gains `limit` and reports `returned` / `truncated` / `hint`. |
| `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp` | `suggest_color_mix` returns an exact match as a success and both colour tools carry gamut information. |
| `src/slic3r/CMakeLists.txt` | Add the two new `OrcaMCPColorRecipe` files. |
| `tests/slic3rutils/CMakeLists.txt` | Add the two new test files. |
| `scripts/orcamcp-bridge.py` | Three-valued liveness verdict, forward-on-busy, split error advice, `listChanged` capability, `notifications/tools/list_changed`. |
| `scripts/tools_schema.py` | Replace the stale `get_presets` entry. |
| `scripts/tests/test_tools_schema.py` | Offline guard so a stale filter schema cannot come back. |
| `docs/tools/reference.md` | `apply_config`, `set_object_config`, `get_presets` updates; a new Filament & Colour Tools section for `suggest_color_mix` and `get_color_palette`. |
| `docs/setup/troubleshooting.md` | What "OrcaMCP is not running" now does and does not mean. |

---

## Task 1: The config value shaper (pure)

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp:19-27` (after `PresetQuery`, before `ApplyConfigResult`)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp:74-82` (after `preset_query_matches`)
- Create: `tests/slic3rutils/test_config_value_shape.cpp`
- Modify: `tests/slic3rutils/CMakeLists.txt:27` (after `test_preset_query.cpp`)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  ```cpp
  namespace Slic3r { namespace GUI {
  struct ConfigValueText {
      bool        ok = false;
      std::string text;    // meaningful only when ok
      std::string reason;  // meaningful only when !ok
  };
  ConfigValueText config_value_to_string(const nlohmann::json& value, ConfigOptionType type);
  std::string     config_value_expected_shape(ConfigOptionType type);
  }}
  ```
  Task 2 and Task 3 both call exactly these two.

- [ ] **Step 1: Write the failing test**

Create `tests/slic3rutils/test_config_value_shape.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp"

// apply_config and set_object_config both take a JSON value and hand text to
// ConfigOption::deserialize. get_valid_config_keys advertises list-typed keys as "strings" /
// "ints" / "bools" / "floats", so an array is the obvious thing for a caller to send -- and the
// separator deserialize wants is NOT the same for all of them: ConfigOptionStrings splits on ';'
// (Config.cpp:149) while ConfigOptionInts, ConfigOptionFloats and ConfigOptionBools split on ','
// (Config.hpp:1089 / 911 / 1959). This is the one place that knows which.

using Slic3r::GUI::config_value_expected_shape;
using Slic3r::GUI::config_value_to_string;
using Slic3r::coBool;
using Slic3r::coBools;
using Slic3r::coFloat;
using Slic3r::coFloats;
using Slic3r::coInts;
using Slic3r::coString;
using Slic3r::coStrings;

TEST_CASE("a string is passed through untouched", "[orcamcp][config]")
{
    // The joined form keeps working: it is what the session had to fall back to.
    const auto joined = config_value_to_string(nlohmann::json("#00FFFF;#FF00FF"), coStrings);
    CHECK(joined.ok);
    CHECK(joined.text == "#00FFFF;#FF00FF");

    const auto scalar = config_value_to_string(nlohmann::json("0.2"), coFloat);
    CHECK(scalar.ok);
    CHECK(scalar.text == "0.2");
}

TEST_CASE("an array of strings is joined the way ConfigOptionStrings reads it", "[orcamcp][config]")
{
    // The T1 reproduction, verbatim.
    const nlohmann::json value = {"#00FFFF", "#FF00FF", "#FFFF00", "#808080"};
    const auto shaped = config_value_to_string(value, coStrings);

    CHECK(shaped.ok);
    CHECK(shaped.text == "#00FFFF;#FF00FF;#FFFF00;#808080");
}

TEST_CASE("a string element that contains the separator is quoted", "[orcamcp][config]")
{
    // ';' inside a value would otherwise split it into two. escape_strings_cstyle quotes only
    // the elements that need it, which is why the plain case above stays readable.
    const nlohmann::json value = {"plain", "has;semicolon"};
    const auto shaped = config_value_to_string(value, coStrings);

    CHECK(shaped.ok);
    CHECK(shaped.text == "plain;\"has;semicolon\"");
}

TEST_CASE("numeric and boolean arrays join with a comma, not a semicolon", "[orcamcp][config]")
{
    const auto ints = config_value_to_string(nlohmann::json({1, 2, 3}), coInts);
    CHECK(ints.ok);
    CHECK(ints.text == "1,2,3");

    const auto floats = config_value_to_string(nlohmann::json({0, 140, 140.5, 0}), coFloats);
    CHECK(floats.ok);
    CHECK(floats.text == "0,140,140.5,0");

    // ConfigOptionBools::deserialize only accepts "1" and "0" without a substitution.
    const auto bools = config_value_to_string(nlohmann::json({true, false, true}), coBools);
    CHECK(bools.ok);
    CHECK(bools.text == "1,0,1");
}

TEST_CASE("a scalar boolean becomes 1 or 0", "[orcamcp][config]")
{
    CHECK(config_value_to_string(nlohmann::json(true), coBool).text == "1");
    CHECK(config_value_to_string(nlohmann::json(false), coBool).text == "0");
}

TEST_CASE("a shape that cannot work is refused with a reason", "[orcamcp][config]")
{
    const auto array_for_scalar = config_value_to_string(nlohmann::json({1, 2}), coFloat);
    CHECK_FALSE(array_for_scalar.ok);
    CHECK(array_for_scalar.reason.find("not a list") != std::string::npos);

    const auto nested = config_value_to_string(nlohmann::json({{1, 2}, {3, 4}}), coFloats);
    CHECK_FALSE(nested.ok);

    const auto object = config_value_to_string(nlohmann::json::object({{"r", 1}}), coString);
    CHECK_FALSE(object.ok);

    const auto null_value = config_value_to_string(nlohmann::json(nullptr), coString);
    CHECK_FALSE(null_value.ok);

    // "" means "one empty value" to ConfigOptionFloats (it pushes 0), never "no values", so an
    // empty array has no honest text form.
    const auto empty = config_value_to_string(nlohmann::json::array(), coFloats);
    CHECK_FALSE(empty.ok);
}

TEST_CASE("the expected shape names what the caller should have sent", "[orcamcp][config]")
{
    CHECK(config_value_expected_shape(coStrings) == "a string, or an array of strings");
    CHECK(config_value_expected_shape(coFloats) == "a number, or an array of numbers");
    CHECK(config_value_expected_shape(coInts) == "a whole number, or an array of whole numbers");
    CHECK(config_value_expected_shape(coBools) == "true or false, or an array of them");
    CHECK(config_value_expected_shape(coFloat) == "a number");
    CHECK(config_value_expected_shape(coString) == "a string");
}
```

Register it in `tests/slic3rutils/CMakeLists.txt` by adding one line after `test_preset_query.cpp`:

```cmake
    test_config_value_shape.cpp
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
```
Expected: FAIL at compile time — `config_value_to_string` and `config_value_expected_shape` are not declared in `OrcaMCPPresetConfigUtils.hpp`.

- [ ] **Step 3: Declare the interface**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp`, add `#include "libslic3r/PrintConfig.hpp"` next to the existing `#include "libslic3r/Preset.hpp"`, then insert this immediately after the `preset_query_matches` declaration (line 27):

```cpp
// One JSON value from a tool call, turned into the text ConfigOption::deserialize expects.
struct ConfigValueText
{
    bool        ok = false;
    std::string text;    // meaningful only when ok
    std::string reason;  // meaningful only when !ok
};

// get_valid_config_keys advertises list-typed keys as "strings" / "ints" / "bools" / "floats", so
// an array is the obvious thing for a caller to send -- but dumping the array and handing the
// literal text to deserialize does not fail loudly. ConfigOptionStrings stores the whole "[...]"
// as one string (Config.cpp:149 finds no ';'), and ConfigOptionFloats stores 0 for the element
// carrying the '[' and returns true anyway (Config.hpp:935). Both are silent corruption.
//
// So the shape is decided here, from the option's declared type, before anything is written. The
// separator is not one separator: ConfigOptionStrings splits on ';' (via unescape_strings_cstyle),
// while ConfigOptionInts (Config.hpp:1089), ConfigOptionFloats (Config.hpp:911) and
// ConfigOptionBools (Config.hpp:1959) split on ','.
ConfigValueText config_value_to_string(const nlohmann::json& value, ConfigOptionType type);

// What a caller should have sent for an option of this type, phrased for an error message.
std::string config_value_expected_shape(ConfigOptionType type);
```

- [ ] **Step 4: Implement it**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp`, insert immediately after `preset_query_matches` (which ends at line 82):

```cpp
namespace {

// The text form of one array element. Booleans become "1"/"0" because ConfigOptionBools accepts
// nothing else without a forward-compatibility substitution (Config.hpp:1975).
bool json_scalar_to_text(const nlohmann::json& value, std::string& out)
{
    if (value.is_string()) {
        out = value.get<std::string>();
        return true;
    }
    if (value.is_boolean()) {
        out = value.get<bool>() ? "1" : "0";
        return true;
    }
    if (value.is_number()) {
        out = value.dump();
        return true;
    }
    return false;
}

} // namespace

std::string config_value_expected_shape(ConfigOptionType type)
{
    switch (type) {
    case coStrings:            return "a string, or an array of strings";
    case coFloats:
    case coPercents:
    case coFloatsOrPercents:   return "a number, or an array of numbers";
    case coInts:
    case coEnums:              return "a whole number, or an array of whole numbers";
    case coBools:              return "true or false, or an array of them";
    case coPoints:             return "\"XxY\", or an array of them";
    case coBool:               return "true or false";
    case coFloat:
    case coPercent:
    case coFloatOrPercent:     return "a number";
    case coInt:
    case coEnum:               return "a whole number";
    default:                   return "a string";
    }
}

ConfigValueText config_value_to_string(const nlohmann::json& value, ConfigOptionType type)
{
    const bool is_list = (int(type) & int(coVectorType)) != 0;

    std::string scalar_text;
    if (json_scalar_to_text(value, scalar_text))
        return {true, scalar_text, {}};

    if (!value.is_array())
        return {false, {}, value.is_null() ? "null is not a setting value"
                                           : "an object is not a setting value"};

    if (!is_list)
        return {false, {}, "an array was given for a key that is not a list"};

    // "" means "one empty value" to ConfigOptionFloats (Config.hpp:916 pushes 0), never "no
    // values", so an empty array has no honest text form. Say so instead of writing a zero.
    if (value.empty())
        return {false, {}, "an empty array cannot be expressed; omit the key instead"};

    std::vector<std::string> elements;
    elements.reserve(value.size());
    for (const auto& element : value) {
        std::string text;
        if (!json_scalar_to_text(element, text))
            return {false, {}, "an array element was itself an array, an object or null"};
        elements.push_back(std::move(text));
    }

    // ConfigOptionStrings is the only vector type that reads ';', and escape_strings_cstyle
    // (Config.cpp:75) is what writes that form -- it quotes only the elements that need it, so a
    // value containing ';' survives instead of splitting in two.
    if (type == coStrings)
        return {true, escape_strings_cstyle(elements), {}};

    std::string joined;
    for (size_t i = 0; i < elements.size(); ++i) {
        if (i > 0)
            joined += ',';
        joined += elements[i];
    }
    return {true, joined, {}};
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 \
&& /Users/hanan/Projects/OrcaMCP/build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[config]"
```
Expected: PASS, 7 test cases.

- [ ] **Step 6: Commit**

```bash
cd /Users/hanan/Projects/OrcaMCP
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp tests/slic3rutils/test_config_value_shape.cpp tests/slic3rutils/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat: one place decides what text a config value becomes

apply_config and set_object_config both flatten a JSON value with is_string() ? get
: dump() and hand the result to ConfigOption::deserialize. That is not a shape check
-- it is a guess, and for three of the four list types the guess fails silently
rather than loudly. config_value_to_string decides it from the option's declared
type instead, and config_value_expected_shape says what should have been sent.

Separator is per type, which the fix direction in the spec did not know:
ConfigOptionStrings splits on ';' via unescape_strings_cstyle (Config.cpp:149) while
ConfigOptionInts (Config.hpp:1089), ConfigOptionFloats (Config.hpp:911) and
ConfigOptionBools (Config.hpp:1959) split on ','. Joining an int array with ';'
would have produced one unparsable token.

Pure and unwired in this commit; the two callers follow.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: `apply_config` accepts arrays and says why a key failed

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp:29-38` (`ApplyConfigResult`)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp:262-294` (the `ApplyConfig` loop)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:1174-1210` (the `apply_config` response)
- Modify: `docs/tools/reference.md:531-577` (the `apply_config` section)

**Interfaces:**
- Consumes: `Slic3r::GUI::config_value_to_string(const nlohmann::json&, ConfigOptionType) -> ConfigValueText{ok,text,reason}` and `Slic3r::GUI::config_value_expected_shape(ConfigOptionType) -> std::string`, from Task 1.
- Produces:
  ```cpp
  struct ApplyConfigResult {
      // ... existing members unchanged ...
      struct RejectedValue { std::string key, reason, expected; };
      std::vector<std::string>   unknown;   // print_config_def has no such key
      std::vector<RejectedValue> rejected;  // the key exists; the value was not accepted
  };
  ```
  `invalid` keeps its current meaning (the union of both) because `OrcaMCPPrinterUtils.cpp:625` reads it. Task 3 reuses the same `unknown_keys` / `rejected_values` response field names.

- [ ] **Step 1: Write the failing test**

There is no GUI-free seam through `ApplyConfig` (it reaches into `wxGetApp().preset_bundle`), so the automated cover for this task is Task 1's, which already fails against the old joining rule. The behavioural check here is a manual one, recorded as the acceptance criterion and run in Step 5 of Task 9 only if the user asks for a live check — **do not start the app for it**. Instead, write the failing check as a compile-time one: add this to `tests/slic3rutils/test_config_value_shape.cpp`, which pins the exact strings the response will carry.

```cpp
TEST_CASE("the rejection message tells a caller both halves", "[orcamcp][config]")
{
    // apply_config pairs the specific complaint with the shape that would have worked; a caller
    // that only sees "invalid_keys" cannot tell a typo'd key from a wrong-shaped value.
    const auto shaped = config_value_to_string(nlohmann::json({1, 2}), Slic3r::coFloat);
    REQUIRE_FALSE(shaped.ok);
    CHECK(shaped.reason == "an array was given for a key that is not a list");
    CHECK(config_value_expected_shape(Slic3r::coFloat) == "a number");
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 \
&& /Users/hanan/Projects/OrcaMCP/build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "the rejection message tells a caller both halves"
```
Expected: PASS if Task 1 landed exactly as written. If it FAILS, Task 1's strings drifted — fix Task 1's implementation to match, not this test.

- [ ] **Step 3: Extend `ApplyConfigResult`**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp`, inside `struct ApplyConfigResult`, after the `color_errors` member:

```cpp
    // "No such key" and "that value was not accepted" are different problems for a caller: one is
    // a typo, the other is a shape it can correct. `invalid` stays the union of the two, because
    // OrcaMCPPrinterUtils::save_physical_printer_preset already reads it as "anything that failed".
    struct RejectedValue
    {
        std::string key;
        std::string reason;    // what went wrong with this value
        std::string expected;  // the shape that would have been accepted
    };
    std::vector<std::string>   unknown;
    std::vector<RejectedValue> rejected;
```

- [ ] **Step 4: Rewrite the `ApplyConfig` loop**

Replace `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp:262-294` (the whole `for (auto& [key, value] : item.at("settings").items())` body) with:

```cpp
    for (auto& [key, value] : item.at("settings").items()) {
        const ConfigOptionDef* def = print_config_def.get(key);
        if (def == nullptr) {
            result.invalid.push_back(key);
            result.unknown.push_back(key);
            continue;
        }
        // Not "can't blindly dump json to string" any more: the dump is decided from the option's
        // declared type, so an array reaches a list-typed key as a list and a shape that cannot
        // work is reported instead of stored. See config_value_to_string.
        const ConfigValueText shaped = config_value_to_string(value, def->type);
        if (!shaped.ok) {
            BOOST_LOG_TRIVIAL(error) << "ApplyConfig: '" << key << "' rejected: " << shaped.reason;
            result.invalid.push_back(key);
            result.rejected.push_back({key, shaped.reason, config_value_expected_shape(def->type)});
            continue;
        }
        const std::string& value_str = shaped.text;
        // A colour key deserializes any string at all, and upstream then decodes an unparseable one
        // as black -- so "B17C38" (no '#') would be accepted here and show up as a black spool.
        // Keep the old value and report the key instead.
        const bool is_color = def->gui_type == ConfigOptionDef::GUIType::color;
        std::unique_ptr<ConfigOption> previous;
        if (is_color && config->option(key) != nullptr)
            previous.reset(config->option(key)->clone());
        try {
            config->set_deserialize(key, value_str, context);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "ApplyConfig: '" << key << ":" << value_str << "' failed: " << e.what();
            result.invalid.push_back(key);
            result.rejected.push_back({key, std::string("could not be read as a value: ") + e.what(),
                                       config_value_expected_shape(def->type)});
            continue;
        }
        std::string bad_color;
        if (is_color && !color_option_is_valid(config->option(key), bad_color)) {
            BOOST_LOG_TRIVIAL(error) << "ApplyConfig: '" << key << ":" << value_str << "' is not a #RRGGBB colour ("
                                     << bad_color << ")";
            if (previous)
                config->set_key_value(key, previous.release());
            result.invalid.push_back(key);
            result.rejected.push_back({key, "not a #RRGGBB colour (" + bad_color + ")",
                                       "\"#RRGGBB\" or \"#RRGGBBAA\", or an array of them"});
            continue;
        }
        result.applied.push_back(key);
    }
```

- [ ] **Step 5: Surface both lists in the `apply_config` response**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp`, in the `apply_config` handler:

Next to `nlohmann::json invalid_keys = nlohmann::json::array();` (line 1173) add:

```cpp
                nlohmann::json unknown_keys = nlohmann::json::array();
                nlohmann::json rejected_values = nlohmann::json::array();
```

Inside the `for (const auto& type : type_order)` loop, after the existing `for (const auto& key : result.invalid) invalid_keys.push_back(key);` (line 1194):

```cpp
                    for (const auto& key : result.unknown) unknown_keys.push_back(key);
                    for (const auto& rejected : result.rejected)
                        rejected_values.push_back({{"key", rejected.key},
                                                   {"reason", rejected.reason},
                                                   {"expected", rejected.expected}});
```

In the `nlohmann::json response = {...}` initializer (lines 1201-1208), after the `invalid_keys` line:

```cpp
                    // invalid_keys is the union of the two, kept because it is the published field.
                    // These two say which problem it was: a key that does not exist, or a value
                    // this key would not take -- and for the second, the shape that would work.
                    {"unknown_keys", unknown_keys},
                    {"rejected_values", rejected_values},
```

- [ ] **Step 6: Update the docs**

In `docs/tools/reference.md`, in the `### apply_config` section, replace the `**Returns:**` block and add a paragraph after the **Colours** paragraph (currently the last one before the `---`).

Replace the returns block with:

````markdown
**Returns:**
```json
{
  "status": "success",
  "applied_keys": ["layer_height"],
  "invalid_keys": [],
  "unknown_keys": [],
  "rejected_values": [],
  "duplicate_keys": [],
  "active_warnings": {"count": 0, "warnings": []}
}
```
````

And add:

````markdown
**Lists:** a key that `get_valid_config_keys` reports as `strings` / `ints` / `bools` / `floats` takes
a JSON array, and the joined string form keeps working:

```json
{"type": "project", "key": "filament_colour", "value": ["#00FFFF", "#FF00FF", "#FFFF00", "#808080"]}
{"type": "project", "key": "filament_colour", "value": "#00FFFF;#FF00FF;#FFFF00;#808080"}
```

Both apply the same four colours. The separator differs per type inside the slicer (`;` for string
lists, `,` for numeric and boolean lists), which is exactly why passing an array is the safer form.

**Why a key failed:** `invalid_keys` is the union of two different problems and stays that way, but
each has its own field now:

| Field | Meaning |
|---|---|
| `unknown_keys` | No such config key. Check `get_valid_config_keys`. |
| `rejected_values` | The key exists; this value was not accepted. Each entry is `{"key", "reason", "expected"}`, where `expected` names the shape that would have worked. |

```json
{
  "status": "partial",
  "applied_keys": [],
  "invalid_keys": ["layer_height"],
  "unknown_keys": [],
  "rejected_values": [
    {"key": "layer_height", "reason": "an array was given for a key that is not a list",
     "expected": "a number"}
  ]
}
```
````

- [ ] **Step 7: Build and run the full suite**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8 \
&& /Users/hanan/Projects/OrcaMCP/build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
```
Expected: PASS, at least 175 cases, 0 failures.

- [ ] **Step 8: Commit**

```bash
cd /Users/hanan/Projects/OrcaMCP
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp tests/slic3rutils/test_config_value_shape.cpp docs/tools/reference.md
git commit -m "$(cat <<'EOF'
fix: a filament colour list could only be set by guessing the separator (T1)

get_valid_config_keys calls filament_colour "strings", so a caller sends an array,
and ApplyConfig dumped it to the literal text ["#00FFFF",...]. Root cause: line 264
guessed the text form from the JSON type instead of the option type. What happened
next was never a clean rejection --

  - filament_colour (coStrings, gui_type color): unescape_strings_cstyle finds no
    ';' so it stores the whole literal as ONE string and returns true. The colour
    check added by sweep item I is what then rejected it. Item I is the rejecter
    here, not deserialize.
  - filament_notes and the other non-colour coStrings keys: same one-string
    corruption, no colour check to catch it -- accepted as "success" until now.
  - flush_volumes_vector/matrix (coFloats): ConfigOptionFloats::deserialize returns
    true unconditionally (Config.hpp:935) after storing 0 for the element carrying
    the '['. Accepted as "success" with wrong numbers until now.
  - filament_map (coInts), filament_is_mixed (coBools): threw, so these were the
    only ones the caller heard about.

apply_config's four branches (print/filament/printer/project) all funnel through
this one loop, so all four are fixed together. set_object_config has the same line
and is fixed in the next commit, deliberately kept separate because its response
shape is per object.

invalid_keys stays the union of both failures because
OrcaMCPPrinterUtils::save_physical_printer_preset reads it that way; unknown_keys
and rejected_values are additive.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `set_object_config` gets the same treatment

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:1694-1711` (the per-object settings loop) and `:1717-1728` (the per-object result)
- Modify: `docs/tools/reference.md:737-758` (the `set_object_config` section)

**Interfaces:**
- Consumes: `Slic3r::GUI::config_value_to_string`, `Slic3r::GUI::config_value_expected_shape` (Task 1); the `unknown_keys` / `rejected_values` field naming established in Task 2.
- Produces: nothing later tasks depend on.

- [ ] **Step 1: Write the failing test**

The per-object path has the same pure dependency and no GUI-free seam, so the regression cover is the same suite. Add one case to `tests/slic3rutils/test_config_value_shape.cpp` pinning the per-object shapes that matter:

```cpp
TEST_CASE("per-object list keys shape the same way", "[orcamcp][config]")
{
    // set_object_config takes the same keys apply_config does; nothing about being a per-object
    // override changes what deserialize wants. These are the two per-object list keys an agent
    // actually reaches for.
    const auto extruders = config_value_to_string(nlohmann::json({1, 3}), Slic3r::coInts);
    CHECK(extruders.ok);
    CHECK(extruders.text == "1,3");

    const auto not_a_list = config_value_to_string(nlohmann::json({0.2, 0.3}), Slic3r::coFloat);
    CHECK_FALSE(not_a_list.ok);
}
```

- [ ] **Step 2: Run the test to verify it passes on the pure layer**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 \
&& /Users/hanan/Projects/OrcaMCP/build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "per-object list keys shape the same way"
```
Expected: PASS. The pure layer already handles it; this task is the wiring.

- [ ] **Step 3: Rewrite the per-object loop**

Replace `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:1694-1711` with:

```cpp
                    std::vector<std::string> unknown_keys;
                    nlohmann::json rejected_values = nlohmann::json::array();

                    for (const auto& item : settings) {
                        std::string key = item["key"];
                        if (++occurrences[key] == 2)
                            duplicate_order.push_back(key);

                        const ConfigOptionDef* def = print_config_def.get(key);
                        if (def == nullptr) {
                            invalid_keys.push_back(key);
                            unknown_keys.push_back(key);
                            continue;
                        }
                        // Same shaping apply_config uses (config_value_to_string): a list-typed key
                        // takes a JSON array, and a shape that cannot work is reported rather than
                        // stored as the literal text of the array.
                        const ConfigValueText shaped = config_value_to_string(item["value"], def->type);
                        if (!shaped.ok) {
                            invalid_keys.push_back(key);
                            rejected_values.push_back({{"key", key},
                                                       {"reason", shaped.reason},
                                                       {"expected", config_value_expected_shape(def->type)}});
                            continue;
                        }

                        try {
                            obj->config.set_deserialize(key, shaped.text, context);
                            if (obj->config.has(key)) {
                                applied_keys.push_back(key);
                            } else {
                                invalid_keys.push_back(key);
                                rejected_values.push_back({{"key", key},
                                                           {"reason", "the override was not stored"},
                                                           {"expected", config_value_expected_shape(def->type)}});
                            }
                        } catch (const std::exception& e) {
                            invalid_keys.push_back(key);
                            rejected_values.push_back({{"key", key},
                                                       {"reason", std::string("could not be read as a value: ") + e.what()},
                                                       {"expected", config_value_expected_shape(def->type)}});
                        }
                    }
```

- [ ] **Step 4: Add the fields to the per-object result**

In the same handler, after the existing `if (!invalid_keys.empty()) { obj_result["invalid_keys"] = invalid_keys; }` block (line 1722-1724), add:

```cpp
                    // Same split apply_config reports: a key that does not exist, versus a value
                    // this key would not take. invalid_keys stays the union.
                    obj_result["unknown_keys"] = unknown_keys;
                    obj_result["rejected_values"] = rejected_values;
```

- [ ] **Step 5: Add the include**

`print_config_def` and `ConfigValueText` must be visible in `OrcaMCPServer.cpp`. `OrcaMCPPresetConfigUtils.hpp` is already included there and now pulls `libslic3r/PrintConfig.hpp` (Task 1, Step 3), so no new include is needed. Confirm with:

```bash
grep -n 'OrcaMCPPresetConfigUtils.hpp' /Users/hanan/Projects/OrcaMCP/src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp
```
Expected: one hit near the top of the file. If there is none, add `#include "OrcaMCPPresetConfigUtils.hpp"` beside the other OrcaMCP includes.

- [ ] **Step 6: Update the docs**

In `docs/tools/reference.md`, at the end of the `### set_object_config` section (just before its `---`), add:

````markdown
**Lists and failures:** identical to `apply_config` — a list-typed key takes a JSON array or the
joined string, `unknown_keys` holds keys that do not exist, and `rejected_values` holds
`{"key", "reason", "expected"}` for values this key would not take. `invalid_keys` remains the union
of both, per object.
````

- [ ] **Step 7: Build and run the full suite**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8 \
&& /Users/hanan/Projects/OrcaMCP/build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
```
Expected: PASS, 0 failures.

- [ ] **Step 8: Commit**

```bash
cd /Users/hanan/Projects/OrcaMCP
git add src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp tests/slic3rutils/test_config_value_shape.cpp docs/tools/reference.md
git commit -m "$(cat <<'EOF'
fix: set_object_config guessed a value's text form the same way apply_config did (T1 sweep)

Line 1698 carried the identical is_string() ? get : dump(), so a per-object list key
had the identical failure modes -- and one more, because it never looked the key up
in print_config_def at all: an unknown key and a wrong-shaped value both came back
as "invalid_keys" with no way to tell them apart.

Now routed through config_value_to_string like apply_config, with the same
unknown_keys / rejected_values split per object.

Other callers of set_deserialize checked and deliberately left alone: the 3MF and
config-file loaders (libslic3r) take text off disk, where the string IS the wire
format and there is no JSON value to shape; OrcaMCPPrinterUtils builds its settings
object in C++ with string literals only, so it cannot produce an array.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: `get_presets` is answerable without a filter

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp:14-27` (`PresetQuery`), `:41-47` (the three JSON builders)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp:84-95` (`PresetsToJson`), `:126-153` (`GetPresetsJson`), `:174-186` (`GetAllPresetJson`)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp:870-941` (the `get_presets` registration and handler)
- Modify: `tests/slic3rutils/test_preset_query.cpp` (add cases)
- Modify: `docs/tools/reference.md:458-509` (the `get_presets` section)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  ```cpp
  struct PresetListCount { int matched = 0; int returned = 0; };
  int         preset_query_effective_limit(int requested_limit, bool summary);
  std::string preset_truncation_hint(const std::map<std::string, PresetListCount>& counts);
  // PresetQuery gains: int limit = -1;   // -1 = "use the default for this summary"
  ```
  Task 6 needs the final `get_presets` tool description and property set, reproduced verbatim there.

- [ ] **Step 1: Write the failing test**

Append to `tests/slic3rutils/test_preset_query.cpp`:

```cpp
#include <map>

using Slic3r::GUI::PresetListCount;
using Slic3r::GUI::preset_query_effective_limit;
using Slic3r::GUI::preset_truncation_hint;

TEST_CASE("the default cap depends on how big a row is", "[orcamcp][presets]")
{
    // A summary row is ~164 characters; the unfiltered summary response was ~54,600 and no MCP
    // client would take it. A full-config row is ~5.7 KB, so the same count is 100x worse.
    CHECK(preset_query_effective_limit(-1, /*summary=*/true) == 25);
    CHECK(preset_query_effective_limit(-1, /*summary=*/false) == 5);
}

TEST_CASE("an explicit limit wins, and 0 means no cap", "[orcamcp][presets]")
{
    CHECK(preset_query_effective_limit(100, /*summary=*/true) == 100);
    CHECK(preset_query_effective_limit(1, /*summary=*/false) == 1);
    // The escape hatch for a caller that knows what it is asking for.
    CHECK(preset_query_effective_limit(0, /*summary=*/true) == 0);
}

TEST_CASE("a truncated response says what it dropped and how to narrow it", "[orcamcp][presets]")
{
    std::map<std::string, PresetListCount> counts;
    counts["filamentPresets"] = {318, 25};
    counts["printerPresets"]  = {9, 9};

    const std::string hint = preset_truncation_hint(counts);
    CHECK(hint.find("25 of 318 filamentPresets") != std::string::npos);
    // A type that was not truncated must not be mentioned -- the hint is about what is missing.
    CHECK(hint.find("printerPresets") == std::string::npos);
    CHECK(hint.find("name_contains") != std::string::npos);
    CHECK(hint.find("limit") != std::string::npos);
}

TEST_CASE("nothing truncated means no hint at all", "[orcamcp][presets]")
{
    std::map<std::string, PresetListCount> counts;
    counts["filamentPresets"] = {4, 4};

    CHECK(preset_truncation_hint(counts).empty());
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
```
Expected: FAIL at compile time — `PresetListCount`, `preset_query_effective_limit` and `preset_truncation_hint` are not declared.

- [ ] **Step 3: Declare the interface**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp`, add `#include <map>` beside the existing includes. Add a `limit` member to `PresetQuery`:

```cpp
struct PresetQuery
{
    std::string vendor;        // case-insensitive substring of the preset's vendor, empty = any
    std::string name_contains; // case-insensitive substring of the preset name, empty = any
    bool        summary = true;
    // Presets returned per type. Even at summary: true the unfiltered response was ~54,600
    // characters -- over an MCP client's per-result limit, so the tool built to be answerable
    // still could not be answered. -1 means "the default for this summary"; 0 means no cap.
    int         limit = -1;
};
```

And after `preset_query_matches` (line 27), before `ConfigValueText`:

```cpp
// How many presets of one type the query matched, and how many of them the response carries.
// They differ only when the cap truncated the list -- and the true total is the number a caller
// needs to know its filter was too wide, which is exactly what a truncated array cannot tell it.
struct PresetListCount
{
    int matched  = 0;
    int returned = 0;
};

// The per-type cap actually applied: the caller's `limit` when it gave one (0 = no cap), else 25
// summary rows or 5 full-config rows -- a full-config row is roughly 5.7 KB against a summary
// row's 164 characters, so the same count is not the same response size.
int preset_query_effective_limit(int requested_limit, bool summary);

// The one-line hint a truncated response carries, naming the filters that would narrow it.
// Empty when nothing was dropped.
std::string preset_truncation_hint(const std::map<std::string, PresetListCount>& counts);
```

Change the three builder signatures in the class body:

```cpp
    static nlohmann::json PresetsToJson(const std::vector<std::pair<const Preset*, bool>>& presets,
                                        const PresetQuery& query, PresetListCount& count);
    // Only presets the tab's combo box lists, i.e. the visible ones compatible with the selected
    // printer, filtered by `query` and capped by its limit. `count` reports both totals.
    static nlohmann::json GetPresetsJson(Preset::Type type, const PresetQuery& query, PresetListCount& count);
    static nlohmann::json GetAllPresetJson(const PresetQuery& query,
                                           std::map<std::string, PresetListCount>& counts);
```

- [ ] **Step 4: Implement the pure functions and the truncation**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp`, after `preset_query_matches` (line 82):

```cpp
int preset_query_effective_limit(int requested_limit, bool summary)
{
    if (requested_limit >= 0)
        return requested_limit;
    return summary ? 25 : 5;
}

std::string preset_truncation_hint(const std::map<std::string, PresetListCount>& counts)
{
    std::string truncated;
    for (const auto& [name, count] : counts) {
        if (count.returned >= count.matched)
            continue;
        if (!truncated.empty())
            truncated += ", ";
        truncated += std::to_string(count.returned) + " of " + std::to_string(count.matched) + " " + name;
    }
    if (truncated.empty())
        return {};
    return "Showing " + truncated + ". Narrow it with type, vendor or name_contains, "
           "raise limit, or pass limit: 0 for the whole list.";
}
```

Replace `PresetsToJson` (lines 84-95) with:

```cpp
nlohmann::json OrcaMCPPresetConfigUtils::PresetsToJson(const std::vector<std::pair<const Preset*, bool>>& presets,
                                                       const PresetQuery& query, PresetListCount& count)
{
    const int limit = preset_query_effective_limit(query.limit, query.summary);
    nlohmann::json j_array = nlohmann::json::array();
    count = {};
    for (const auto& [preset, is_selected] : presets) {
        if (!preset_query_matches(preset->name, preset_vendor(preset), query))
            continue;
        // Every match is counted, whether or not it fits: a caller that is only told what it got
        // back cannot tell "that is all of them" from "that is the first page".
        ++count.matched;
        if (limit > 0 && count.returned >= limit)
            continue;
        ++count.returned;
        j_array.push_back(PresetToJson(preset, is_selected, query));
    }
    return j_array;
}
```

In `GetPresetsJson` (line 126), change the signature to take `PresetListCount& count`, change the early return to also clear the count, and pass it through:

```cpp
nlohmann::json OrcaMCPPresetConfigUtils::GetPresetsJson(Preset::Type type, const PresetQuery& query,
                                                        PresetListCount& count) {
    count = {};
    Tab* tab = wxGetApp().get_tab(type);
    if (!tab) {
        return nlohmann::json::array();
    }
```
(the body is otherwise unchanged) and its last line becomes:
```cpp
    return PresetsToJson(presets, query, count);
```

Replace `GetAllPresetJson` (lines 174-186) with:

```cpp
nlohmann::json OrcaMCPPresetConfigUtils::GetAllPresetJson(const PresetQuery& query,
                                                          std::map<std::string, PresetListCount>& counts) {
    nlohmann::json printerPresetsJson = GetPresetsJson(Preset::Type::TYPE_PRINTER, query, counts["printerPresets"]);
    nlohmann::json filamentPresetsJson = GetPresetsJson(Preset::Type::TYPE_FILAMENT, query, counts["filamentPresets"]);
    nlohmann::json printPresetsJson = GetPresetsJson(Preset::Type::TYPE_PRINT, query, counts["printProcessPresets"]);

    return {
        {"printerPresets", printerPresetsJson},
        {"filamentPresets", filamentPresetsJson},
        {"printProcessPresets", printPresetsJson}
    };
}
```

- [ ] **Step 5: Run the pure tests to verify they pass**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 \
&& /Users/hanan/Projects/OrcaMCP/build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[presets]"
```
Expected: PASS, 8 test cases.

- [ ] **Step 6: Rewire the `get_presets` tool**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp`, replace the description string (lines 873-876) with this exact text — Task 6 copies it verbatim into `scripts/tools_schema.py`:

```cpp
        "List the printer, filament and print presets available for the selected printer. "
        "Returns names and identifying fields only; pass summary:false for full configs. "
        "Capped per type (default 25) -- narrow it with type/vendor/name_contains, or raise limit.",
```

Add a `limit` property after the `summary` property (line 892-897):

```cpp
                {"limit", {
                    {"type", "integer"},
                    {"minimum", 0},
                    {"description", "Max presets per type. Default 25 with summary, 5 without. "
                                    "0 = no cap (the unfiltered summary list is ~54,600 characters "
                                    "and overflows most MCP clients)."}
                }}
```

In the handler lambda, after the `summary` parse (line 903-904):

```cpp
            if (params.contains("limit") && !parse_integer_param(params["limit"], query.limit))
                return nlohmann::json{{"status", "error"}, {"message", "limit must be an integer"}};
            if (query.limit < 0)
                return nlohmann::json{{"status", "error"}, {"message", "limit must be 0 or more; 0 means no cap"}};
```

Replace the `run_on_main_thread` body (lines 913-940) with:

```cpp
            return run_on_main_thread([query, type]() -> nlohmann::json {
                nlohmann::json result;
                std::map<std::string, PresetListCount> counts;
                if (type.empty()) {
                    result = OrcaMCPPresetConfigUtils::GetAllPresetJson(query, counts);
                } else if (type == "printer") {
                    result = {{"printerPresets", OrcaMCPPresetConfigUtils::GetPresetsJson(
                                                     Preset::TYPE_PRINTER, query, counts["printerPresets"])}};
                } else if (type == "filament") {
                    result = {{"filamentPresets", OrcaMCPPresetConfigUtils::GetPresetsJson(
                                                      Preset::TYPE_FILAMENT, query, counts["filamentPresets"])}};
                } else {
                    result = {{"printProcessPresets", OrcaMCPPresetConfigUtils::GetPresetsJson(
                                                          Preset::TYPE_PRINT, query, counts["printProcessPresets"])}};
                }
                // Say what was searched and how much of it came back, so a caller can tell an empty
                // list from a filter that was too narrow, and knows the list is already restricted
                // to presets compatible with the selected printer. `counts` keeps its published
                // meaning -- every preset that matched -- and `returned` says how many fit.
                nlohmann::json matched_counts = nlohmann::json::object();
                nlohmann::json returned_counts = nlohmann::json::object();
                bool truncated = false;
                for (const auto& [key, count] : counts) {
                    matched_counts[key] = count.matched;
                    returned_counts[key] = count.returned;
                    truncated = truncated || count.returned < count.matched;
                }
                result["query"] = {
                    {"type", type.empty() ? nlohmann::json(nullptr) : nlohmann::json(type)},
                    {"vendor", query.vendor},
                    {"name_contains", query.name_contains},
                    {"summary", query.summary},
                    {"limit", preset_query_effective_limit(query.limit, query.summary)},
                    {"compatible_with_selected_printer_only", true},
                    {"counts", matched_counts},
                    {"returned", returned_counts},
                    {"truncated", truncated}
                };
                const std::string hint = preset_truncation_hint(counts);
                if (!hint.empty())
                    result["hint"] = hint;
                return result;
            });
```

Add `#include <map>` to `OrcaMCPServer.cpp` if it is not already there:
```bash
grep -n '#include <map>' /Users/hanan/Projects/OrcaMCP/src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp
```
It is used already for `grouped_settings`, so this should return a hit; add the include beside the other standard headers if it does not.

- [ ] **Step 7: Update the docs**

In `docs/tools/reference.md`, `### get_presets`: add a row to the parameter table after `summary`:

```markdown
| `limit` | integer | No | Max presets per type. Default 25 with `summary`, 5 without. `0` = no cap. |
```

Replace the `**Why `summary` defaults to true:**` paragraph with:

````markdown
**Why it is capped and summarised:** the unfiltered full-config response is ~1.9 MB and the
unfiltered `summary` response is still ~54,600 characters — both over an MCP client's per-result
limit, so the tool could not be answered at all. `summary` decides whether each preset carries its
`config` blob; `limit` decides how many presets of each type come back. The response shape is
otherwise unchanged (the same `printerPresets` / `filamentPresets` / `printProcessPresets` arrays).

When the cap dropped anything, the response carries a top-level `hint` naming the filters, and
`query.truncated` is `true`. `query.counts` still reports **every** preset that matched;
`query.returned` reports how many are in the arrays.
````

Add after the existing `**Returns:**` block:

````markdown
**Returns (unfiltered, truncated):**
```json
{
  "filamentPresets": ["... 25 presets ..."],
  "query": {
    "type": "filament",
    "vendor": "",
    "name_contains": "",
    "summary": true,
    "limit": 25,
    "compatible_with_selected_printer_only": true,
    "counts": {"filamentPresets": 318},
    "returned": {"filamentPresets": 25},
    "truncated": true
  },
  "hint": "Showing 25 of 318 filamentPresets. Narrow it with type, vendor or name_contains, raise limit, or pass limit: 0 for the whole list."
}
```
````

- [ ] **Step 8: Build and run the full suite**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8 \
&& /Users/hanan/Projects/OrcaMCP/build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
```
Expected: PASS, 0 failures.

- [ ] **Step 9: Commit**

```bash
cd /Users/hanan/Projects/OrcaMCP
git add src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPPresetConfigUtils.cpp src/slic3r/GUI/OrcaMCP/OrcaMCPServer.cpp tests/slic3rutils/test_preset_query.cpp docs/tools/reference.md
git commit -m "$(cat <<'EOF'
fix: get_presets could not answer the question it is for (T2, size half)

Sweep item H made the response 35x smaller and it was still unanswerable: 318
filament presets at ~164 characters a row is ~54,600 characters, over an MCP
client's per-result limit, so the tool spilled to a file instead of answering. Root
cause: the filters were the only size control, and a caller who does not yet know
what to filter for is exactly the caller who calls it unfiltered.

Now capped per type -- 25 summary rows or 5 full-config rows, because a full-config
row is ~5.7 KB against a summary row's 164 characters -- with limit: 0 as the
explicit escape hatch. query.counts keeps its published meaning (every preset that
matched) so the true total is still reported; query.returned and query.truncated say
what fit, and a truncated response names the filters in a hint.

The stale-schema half of T2 -- why those filters could not be sent at all from that
session -- is the next commit; it is the bridge, not the client.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: The bridge stops calling a busy app a dead one

**Files:**
- Modify: `scripts/orcamcp-bridge.py:30-36` (imports), `:257-279` (`check_orcaslicer_connection`), `:352-387` (`handle_local_request`), `:444-474` (`send_request`)
- Create: `scripts/tests/test_bridge_liveness.py`
- Modify: `docs/setup/troubleshooting.md:7-62` (the Connection Issues section)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  ```python
  LIVE = "live"   # something answered
  BUSY = "busy"   # reachable, did not answer in time -- NOT proof it is down
  DOWN = "down"   # connection refused / no listener / name resolution failed
  def check_orcaslicer_connection(use_cache: bool = True, timeout: float = 0.3) -> str
  ```
  Task 6 calls `check_orcaslicer_connection` and compares against `DOWN`.

- [ ] **Step 1: Write the failing test**

Create `scripts/tests/test_bridge_liveness.py`:

```python
#!/usr/bin/env python3
"""Tests for the liveness verdict in scripts/orcamcp-bridge.py (stdlib unittest, no deps).

Run from the repo root:  python3 -m unittest discover -s scripts/tests -p 'test_bridge*.py' -v

The bug these pin (T3): eight concurrent calls, six answered, the last two came back
"OrcaMCP is not running" while the process was alive. HttpServer runs ONE io thread
(HttpServer.cpp:210) and every handler blocks it inside run_on_main_thread, so a burst
serialises and a 0.3s probe cannot be answered. Treating that as "down" -- and caching
it for 3 seconds -- is what fabricated the verdict.
"""

import importlib.util
import os
import socket
import sys
import unittest
import urllib.error
from unittest import mock

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BRIDGE_PATH = os.path.join(REPO_ROOT, "scripts", "orcamcp-bridge.py")


def load_bridge():
    """The bridge's filename has a hyphen, so it cannot be imported by name."""
    sys.path.insert(0, os.path.join(REPO_ROOT, "scripts"))
    spec = importlib.util.spec_from_file_location("orcamcp_bridge", BRIDGE_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeResponse:
    def __init__(self, status=200):
        self.status = status

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


class LivenessVerdictTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_bridge()
        self.bridge._connection_cache = {"connected": None, "last_check": 0}

    def verdict(self, side_effect):
        with mock.patch.object(self.bridge.urllib.request, "urlopen", side_effect=side_effect):
            return self.bridge.check_orcaslicer_connection(use_cache=False)

    def test_a_200_is_live(self):
        self.assertEqual(self.verdict(lambda *a, **k: FakeResponse(200)), self.bridge.LIVE)

    def test_an_http_error_is_live_because_something_answered(self):
        error = urllib.error.HTTPError(self.bridge.ORCAMCP_URL, 500, "boom", {}, None)
        self.assertEqual(self.verdict(error), self.bridge.LIVE)

    def test_a_timeout_is_busy_not_down(self):
        self.assertEqual(self.verdict(socket.timeout("timed out")), self.bridge.BUSY)
        self.assertEqual(self.verdict(TimeoutError("timed out")), self.bridge.BUSY)
        self.assertEqual(
            self.verdict(urllib.error.URLError(socket.timeout("timed out"))), self.bridge.BUSY
        )

    def test_a_refused_connection_is_down(self):
        refused = urllib.error.URLError(ConnectionRefusedError(61, "Connection refused"))
        self.assertEqual(self.verdict(refused), self.bridge.DOWN)

    def test_an_unresolvable_host_is_down(self):
        self.assertEqual(
            self.verdict(urllib.error.URLError(socket.gaierror(8, "nodename nor servname"))),
            self.bridge.DOWN,
        )

    def test_an_unrecognised_failure_is_busy_because_it_is_not_proof_of_death(self):
        self.assertEqual(self.verdict(urllib.error.URLError("something else")), self.bridge.BUSY)


class VerdictCachingTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_bridge()
        self.bridge._connection_cache = {"connected": None, "last_check": 0}

    def test_busy_is_never_cached(self):
        # The 3-second cache is what turned one timed-out probe into a run of false verdicts.
        with mock.patch.object(
            self.bridge.urllib.request, "urlopen", side_effect=socket.timeout("timed out")
        ):
            self.bridge.check_orcaslicer_connection(use_cache=True)
        self.assertIsNone(self.bridge._connection_cache["connected"])

    def test_live_and_down_are_cached(self):
        with mock.patch.object(
            self.bridge.urllib.request, "urlopen", side_effect=lambda *a, **k: FakeResponse(200)
        ):
            self.bridge.check_orcaslicer_connection(use_cache=True)
        self.assertEqual(self.bridge._connection_cache["connected"], self.bridge.LIVE)


class NotRunningAdviceTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_bridge()
        self.bridge._connection_cache = {"connected": None, "last_check": 0}
        self.bridge.CACHED_TOOLS = [{"name": "get_scene_info"}]

    def call_get_scene_info(self):
        return self.bridge.handle_local_request(
            {"jsonrpc": "2.0", "id": 7, "method": "tools/call",
             "params": {"name": "get_scene_info", "arguments": {}}}
        )

    def test_a_busy_server_gets_the_request_forwarded(self):
        with mock.patch.object(self.bridge, "check_orcaslicer_connection", return_value=self.bridge.BUSY):
            self.assertIsNone(self.call_get_scene_info())

    def test_a_live_server_gets_the_request_forwarded(self):
        with mock.patch.object(self.bridge, "check_orcaslicer_connection", return_value=self.bridge.LIVE):
            self.assertIsNone(self.call_get_scene_info())

    def test_only_a_down_server_is_told_to_start(self):
        with mock.patch.object(self.bridge, "check_orcaslicer_connection", return_value=self.bridge.DOWN):
            response = self.call_get_scene_info()
        self.assertIn("not running", response["result"]["content"][0]["text"])


class SendRequestAdviceTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_bridge()

    def message(self, side_effect):
        with mock.patch.object(self.bridge.urllib.request, "urlopen", side_effect=side_effect):
            response = self.bridge.send_request({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                                 "params": {"name": "slice_all", "arguments": {}}})
        return response["error"]["message"]

    def test_a_timeout_says_busy_and_names_the_timeout_knob(self):
        message = self.message(socket.timeout("timed out"))
        self.assertIn("ORCAMCP_TIMEOUT", message)
        self.assertNotIn("Is OrcaSlicer running", message)

    def test_a_refused_connection_says_it_is_not_running(self):
        message = self.message(urllib.error.URLError(ConnectionRefusedError(61, "Connection refused")))
        self.assertIn("not running", message)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd /Users/hanan/Projects/OrcaMCP && python3 -m unittest discover -s scripts/tests -p 'test_bridge*.py' -v
```
Expected: FAIL — `AttributeError: module 'orcamcp_bridge' has no attribute 'LIVE'`.

- [ ] **Step 3: Implement the three-valued verdict**

In `scripts/orcamcp-bridge.py`, add `import socket` to the import block at line 30-36.

Replace `check_orcaslicer_connection` (lines 257-279) with:

```python
# What a liveness probe concluded. Three values, not two, because the advice differs completely:
# a refused connection means "start the app", a timeout means "it is busy, wait or raise the
# timeout", and telling a user to restart a running application is the worse of the two mistakes.
LIVE = "live"  # something answered, even an HTTP error
BUSY = "busy"  # reachable but did not answer in time, or failed in a way that is not proof of death
DOWN = "down"  # connection refused, no listener, or the host does not resolve


def _verdict_for_exception(exc) -> str:
    """Classify a urlopen failure. Unknown failures are BUSY: absence of an answer is not proof."""
    if isinstance(exc, urllib.error.HTTPError):
        # A status code means the server is there and formed a reply.
        return LIVE
    reason = getattr(exc, "reason", exc)
    if isinstance(reason, (socket.timeout, TimeoutError)):
        return BUSY
    if isinstance(reason, (ConnectionRefusedError, socket.gaierror)):
        return DOWN
    if isinstance(reason, OSError) and reason.errno in (
        61,   # ECONNREFUSED on macOS
        111,  # ECONNREFUSED on Linux
        10061,  # WSAECONNREFUSED on Windows
    ):
        return DOWN
    return BUSY


def check_orcaslicer_connection(use_cache: bool = True, timeout: float = 0.3) -> str:
    """Probe OrcaSlicer and return LIVE, BUSY or DOWN.

    The probe timeout is deliberately short so startup stays fast. That is only safe because a
    timeout no longer means "down": HttpServer runs a single io thread (HttpServer.cpp:210) and
    every handler blocks it inside run_on_main_thread, so a burst of calls serialises and this
    probe queues behind them. Under that load the old bool verdict said "not running" about an
    application that was answering, and cached it for CONNECTION_CACHE_TTL seconds.
    """
    global _connection_cache

    if use_cache:
        now = time.time()
        if now - _connection_cache["last_check"] < CONNECTION_CACHE_TTL:
            if _connection_cache["connected"] is not None:
                return _connection_cache["connected"]

    try:
        req = urllib.request.Request(ORCAMCP_URL, method="GET")
        with urllib.request.urlopen(req, timeout=timeout) as response:
            verdict = LIVE if response.status == 200 else BUSY
    except Exception as exc:
        verdict = _verdict_for_exception(exc)

    # BUSY is a momentary state, so it is never cached: caching it is precisely how one timed-out
    # probe turned into three seconds of fabricated "not running" answers.
    if verdict in (LIVE, DOWN):
        _connection_cache = {"connected": verdict, "last_check": time.time()}
    return verdict
```

- [ ] **Step 4: Fix the three call sites of the old boolean**

`launch_orcamcp`, line 136:
```python
    if check_orcaslicer_connection() != DOWN:
        return {"success": True, "message": "OrcaMCP is already running"}
```

`launch_orcamcp`, line 197:
```python
            if check_orcaslicer_connection(use_cache=False) == LIVE:
```

`handle_local_request`, lines 354-368 — replace both connection checks:
```python
    # Optimization: For tools/list during initial startup (no cached tools),
    # return minimal list immediately without slow connection check
    if method == "tools/list" and CACHED_TOOLS is None:
        # First time - try a quick check, but return the static list fast if nothing answers
        if check_orcaslicer_connection(timeout=0.1) != LIVE:
            log_debug("Quick startup: returning static tools list")
            return make_success_response(request_id, {"tools": get_full_tools_list()})
        # Connected - let it through to get full tools list
        return None

    # For other methods, only a DOWN verdict is answered locally. A BUSY server is forwarded to:
    # the real request has the full ORCAMCP_TIMEOUT to be answered, and a real error from a real
    # attempt is worth more to a caller than a guess made from a 0.3-second probe.
    if check_orcaslicer_connection() != DOWN:
        return None
```

- [ ] **Step 5: Split the advice `send_request` gives**

Replace the `except` chain in `send_request` (lines 454-474) with:

```python
    except urllib.error.HTTPError as e:
        log_debug(f"HTTP error: {e}")
        return make_error_response(
            request_id,
            -32000,
            f"OrcaSlicer answered with HTTP {e.code} ({e.reason}) at {ORCAMCP_URL}."
        )
    except (socket.timeout, TimeoutError):
        log_debug(f"Request timed out after {TIMEOUT}s")
        return make_error_response(
            request_id,
            -32000,
            f"OrcaSlicer did not answer within {TIMEOUT}s. It serves one request at a time, so a "
            f"batch of calls queues; a slice or a render can also outlast the timeout. Wait and "
            f"retry, or raise ORCAMCP_TIMEOUT. This is not a sign that it stopped running."
        )
    except urllib.error.URLError as e:
        verdict = _verdict_for_exception(e)
        log_debug(f"Connection error ({verdict}): {e}")
        if verdict == DOWN:
            return make_error_response(
                request_id,
                -32000,
                f"Nothing is listening at {ORCAMCP_URL}. OrcaMCP is not running -- use the "
                f"'start_orca' tool. Error: {str(e)}"
            )
        return make_error_response(
            request_id,
            -32000,
            f"OrcaSlicer is reachable but the request did not complete: {str(e)}. Retry, or raise "
            f"ORCAMCP_TIMEOUT if a long operation is running."
        )
    except json.JSONDecodeError as e:
        log_debug(f"JSON decode error: {e}")
        return make_error_response(
            request_id,
            -32700,
            f"Invalid JSON response from OrcaSlicer: {str(e)}"
        )
    except Exception as e:
        log_debug(f"Unexpected error: {e}")
        return make_error_response(
            request_id,
            -32603,
            f"Internal error: {str(e)}"
        )
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cd /Users/hanan/Projects/OrcaMCP && python3 -m unittest discover -s scripts/tests -p 'test_bridge*.py' -v
```
Expected: PASS, 12 tests.

- [ ] **Step 7: Update the troubleshooting doc**

In `docs/setup/troubleshooting.md`, replace the body of `### "Request timed out"` and add a new subsection after it:

````markdown
### "Request timed out"

OrcaSlicer's embedded HTTP server runs **one** worker thread, and every MCP handler blocks it while
the operation runs on the GUI thread. Calls are therefore served strictly one at a time: a batch of
eight tool calls queues, and the last one waits for the seven ahead of it.

Raise the ceiling if you are batching or slicing:

```bash
export ORCAMCP_TIMEOUT=300
```

### "OrcaMCP is not running" while it clearly is

This should no longer happen. The bridge used to treat any failed liveness probe — including a
0.3-second probe that queued behind a batch — as "not running", and cached that answer for three
seconds, so a burst of calls could get the verdict fabricated for the calls at the end of it.

The probe now returns one of three verdicts, and only the last one produces that message:

| Verdict | Meaning | What the bridge does |
|---|---|---|
| live | Something answered, even an HTTP error | Forward the request |
| busy | Reachable, no answer inside the probe window | Forward the request anyway, with the full `ORCAMCP_TIMEOUT` |
| down | Connection refused, or nothing listening | Answer locally: "OrcaMCP is not running" |

So if you see it, nothing is listening on the port. Check `ORCAMCP_PORT`, and use `start_orca`.
````

- [ ] **Step 8: Commit**

```bash
cd /Users/hanan/Projects/OrcaMCP
git add scripts/orcamcp-bridge.py scripts/tests/test_bridge_liveness.py docs/setup/troubleshooting.md
git commit -m "$(cat <<'EOF'
fix: the bridge told an agent to restart an application that was answering (T3)

Eight concurrent suggest_color_mix calls; six answered, the last two came back
"OrcaMCP is not running" with the process alive and no crash report. Root cause:
check_orcaslicer_connection probed with a 0.3-second GET, caught every failure with
one bare `except Exception`, and cached the resulting False for
CONNECTION_CACHE_TTL = 3 seconds -- so a probe that merely queued became three
seconds of fabricated verdicts.

It genuinely could not be answered in 0.3s. HttpServer runs a single io_service.run
thread (HttpServer.cpp:210) and every handler blocks it inside run_on_main_thread's
future.get, so requests serialise and the probe queues behind the batch. That is
worth understanding rather than working around: the server is not meant to be
concurrent, so the client must not read slowness as death.

The probe now returns live / busy / down. Only "down" -- refused, unresolvable, no
listener -- is answered locally; "busy" is forwarded and gets the full
ORCAMCP_TIMEOUT to succeed, and is never cached. An HTTPError now counts as live: a
status code proves something is there, and the old code called that "not running"
too.

Related occurrence fixed in the same pass: send_request's URLError handler asked
"Is OrcaSlicer running?" for a plain timeout. Deliberately left alone: the server's
single-threaded design, which is correct given every handler needs the GUI thread --
the client side is where the wrong conclusion was drawn.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: A stale tool list cannot survive an app update

**Files:**
- Modify: `scripts/tools_schema.py` (the `get_presets` entry only)
- Modify: `scripts/orcamcp-bridge.py:60-66` (module state), `:319-329` (`initialize`), `:352-376` (the two static-list answers), `:190-208` (`launch_orcamcp`), `:444-453` (`send_request` success), `:512-539` (main loop)
- Create: `scripts/tests/test_bridge_schema_refresh.py`
- Modify: `scripts/tests/test_tools_schema.py` (add an offline guard)

**Interfaces:**
- Consumes: `LIVE` / `DOWN` and `check_orcaslicer_connection` from Task 5; the exact `get_presets` description and property set from Task 4, Step 6.
- Produces:
  ```python
  def tools_changed_notification_due() -> bool  # True at most once per bridge process
  ```

- [ ] **Step 1: Write the failing test**

Create `scripts/tests/test_bridge_schema_refresh.py`:

```python
#!/usr/bin/env python3
"""The bridge must not leave a client on a stale tool list (T2, schema half).

Run from the repo root:  python3 -m unittest discover -s scripts/tests -p 'test_bridge*.py' -v

scripts/tools_schema.py is a checked-in file regenerated by hand, and the bridge answers the very
first tools/list from it when OrcaSlicer is not up yet -- the normal case, because the MCP client
starts first. Without a tools/list_changed notification the client keeps whatever that file said
for the life of the session, which is how get_presets' filters were unreachable from a session
whose running server had them.
"""

import importlib.util
import os
import sys
import unittest
from unittest import mock

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BRIDGE_PATH = os.path.join(REPO_ROOT, "scripts", "orcamcp-bridge.py")


def load_bridge():
    sys.path.insert(0, os.path.join(REPO_ROOT, "scripts"))
    spec = importlib.util.spec_from_file_location("orcamcp_bridge", BRIDGE_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ListChangedCapabilityTests(unittest.TestCase):
    def test_initialize_advertises_listChanged(self):
        bridge = load_bridge()
        response = bridge.handle_local_request(
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}}
        )
        self.assertTrue(response["result"]["capabilities"]["tools"]["listChanged"])


class RefreshNotificationTests(unittest.TestCase):
    def setUp(self):
        self.bridge = load_bridge()
        self.bridge._connection_cache = {"connected": None, "last_check": 0}

    def serve_static_tools_list(self):
        with mock.patch.object(self.bridge, "check_orcaslicer_connection", return_value=self.bridge.DOWN):
            return self.bridge.handle_local_request({"jsonrpc": "2.0", "id": 1, "method": "tools/list"})

    def test_no_notification_before_a_live_server_is_seen(self):
        self.serve_static_tools_list()
        self.assertFalse(self.bridge.tools_changed_notification_due())

    def test_no_notification_when_the_static_list_was_never_served(self):
        self.bridge._live_contact = True
        self.assertFalse(self.bridge.tools_changed_notification_due())

    def test_one_notification_after_the_static_list_and_then_a_live_server(self):
        self.serve_static_tools_list()
        self.bridge._live_contact = True
        self.assertTrue(self.bridge.tools_changed_notification_due())
        # Once only: a client that re-fetched does not need telling again every request.
        self.assertFalse(self.bridge.tools_changed_notification_due())


class StaticSchemaFreshnessTests(unittest.TestCase):
    def test_get_presets_in_the_static_list_has_its_filters(self):
        sys.path.insert(0, os.path.join(REPO_ROOT, "scripts"))
        from tools_schema import FULL_TOOLS_LIST

        entry = next(t for t in FULL_TOOLS_LIST if t["name"] == "get_presets")
        properties = entry["inputSchema"]["properties"]
        self.assertEqual(
            set(properties), {"type", "vendor", "name_contains", "summary", "limit"}
        )


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd /Users/hanan/Projects/OrcaMCP && python3 -m unittest discover -s scripts/tests -p 'test_bridge*.py' -v
```
Expected: FAIL — `tools_changed_notification_due` does not exist, `listChanged` is missing, and `get_presets` in the static list has no properties at all.

- [ ] **Step 3: Refresh the static schema**

In `scripts/tools_schema.py`, find the `get_presets` entry and replace it in full with the following. The `inputSchema` shape (`additionalProperties: False`, `required: []`) is what `OrcaMCPServer::handle_tools_list` adds at `OrcaMCPServer.cpp:231-238`; the description and properties are copied verbatim from Task 4, Step 6.

```python
 {'description': 'List the printer, filament and print presets available for the '
                 'selected printer. Returns names and identifying fields only; '
                 'pass summary:false for full configs. Capped per type (default '
                 '25) -- narrow it with type/vendor/name_contains, or raise limit.',
  'inputSchema': {'additionalProperties': False,
                  'properties': {'limit': {'description': 'Max presets per type. '
                                                          'Default 25 with '
                                                          'summary, 5 without. 0 '
                                                          '= no cap (the '
                                                          'unfiltered summary '
                                                          'list is ~54,600 '
                                                          'characters and '
                                                          'overflows most MCP '
                                                          'clients).',
                                           'minimum': 0,
                                           'type': 'integer'},
                                 'name_contains': {'description': 'Only presets '
                                                                  'whose name '
                                                                  'contains this '
                                                                  '(case-insensitive, '
                                                                  'e.g. "PETG")',
                                                   'type': 'string'},
                                 'summary': {'default': True,
                                             'description': 'true (default): '
                                                            'name, vendor, '
                                                            'filament_type/printer_model '
                                                            'and flags. false: '
                                                            'also every config '
                                                            'key of every '
                                                            'matching preset.',
                                             'type': 'boolean'},
                                 'type': {'description': 'Only this preset type. '
                                                         'Omit (or "all") for all '
                                                         'three.',
                                          'enum': ['printer',
                                                   'filament',
                                                   'print',
                                                   'all'],
                                          'type': 'string'},
                                 'vendor': {'description': 'Only presets from '
                                                           'this vendor '
                                                           '(case-insensitive '
                                                           'substring, e.g. '
                                                           '"Flashforge")',
                                            'type': 'string'}},
                  'required': [],
                  'type': 'object'},
  'name': 'get_presets'},
```

- [ ] **Step 4: Add the offline guard to the existing schema test**

In `scripts/tests/test_tools_schema.py`, after `test_static_list_has_required_fields`:

```python
def test_static_list_carries_get_presets_filters():
    """The one entry that was found stale in the field, pinned so it cannot silently go back.

    test_static_list_matches_running_server catches every drift but skips when nothing is
    listening -- which is exactly the situation in which this file is edited.
    """
    entry = next(t for t in _load_static() if t["name"] == "get_presets")
    assert set(entry["inputSchema"]["properties"]) == {
        "type", "vendor", "name_contains", "summary", "limit"
    }
```

- [ ] **Step 5: Make the bridge tell the client the list moved**

In `scripts/orcamcp-bridge.py`, after `CONNECTION_CACHE_TTL = 3` (line 66):

```python
# tools_schema.py is regenerated by hand, and the bridge answers the first tools/list from it when
# OrcaSlicer is not up yet -- the normal case, since the MCP client starts first. Whatever that file
# said is then the client's tool list for the session, which is how get_presets' filters were
# unreachable from a session whose running server had them. These two flags let the bridge say
# "the list changed" the moment a live server is first reached.
_served_static_tools = False
_live_contact = False
_notified_tools_changed = False
```

After `make_tool_error_result` (line 254), add:

```python
def note_live_contact():
    """Record that a live OrcaSlicer answered. Cheap enough to call on every success."""
    global _live_contact
    _live_contact = True


def tools_changed_notification_due() -> bool:
    """True at most once: a static tool list was served, and a live server has since answered."""
    global _notified_tools_changed
    if _notified_tools_changed or not _served_static_tools or not _live_contact:
        return False
    _notified_tools_changed = True
    return True
```

In `handle_local_request`, declare the flag alongside `CACHED_TOOLS` (line 311):
```python
    global CACHED_TOOLS, _served_static_tools
```

Advertise the capability in the `initialize` response (line 325-327):
```python
            "capabilities": {
                # The static list this bridge may serve first is not necessarily current, so the
                # client must be willing to be told it changed.
                "tools": {"listChanged": True}
            },
```

Set the flag at both places that answer from the static list — the fast-startup branch (now inside Task 5's edit at line ~356) and the offline branch (line ~375):
```python
        if check_orcaslicer_connection(timeout=0.1) != LIVE:
            log_debug("Quick startup: returning static tools list")
            _served_static_tools = True
            return make_success_response(request_id, {"tools": get_full_tools_list()})
```
```python
    if method == "tools/list":
        # Return cached tools if available, otherwise the static list
        tools = CACHED_TOOLS if CACHED_TOOLS else get_full_tools_list()
        if not CACHED_TOOLS:
            _served_static_tools = True
        return make_success_response(request_id, {"tools": tools})
```

Record live contact in `send_request`, immediately after `response_data = response.read()...` (line 447):
```python
            note_live_contact()
```
and in `launch_orcamcp`, inside the successful poll (line 198-202):
```python
            if check_orcaslicer_connection(use_cache=False) == LIVE:
                note_live_contact()
                elapsed = int(time.time() - start_time)
```

In `main()`, after each `print(json.dumps(local_response), flush=True)` (line 508) and after `print(json.dumps(response), flush=True)` (line 538), add the same two lines:

```python
            if tools_changed_notification_due():
                print(json.dumps({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}), flush=True)
                log_debug("Told the client its tool list is stale")
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cd /Users/hanan/Projects/OrcaMCP && python3 -m unittest discover -s scripts/tests -p 'test_bridge*.py' -v \
&& python3 -m pytest scripts/tests/test_tools_schema.py -v
```
Expected: PASS. `test_static_list_matches_running_server` skips ("OrcaMCP server not reachable"); that is expected and is the reason the offline guard exists.

- [ ] **Step 7: Document the refresh**

In `docs/tools/reference.md`, in the `## Bridge Tools` section, after the `start_orca` entry, add:

````markdown
### Tool list freshness

The bridge answers the first `tools/list` from a checked-in snapshot (`scripts/tools_schema.py`)
when OrcaSlicer is not yet running — which is normal, since the MCP client starts first. That
snapshot is regenerated by hand and can lag a build.

To stop a client being stranded on it, the bridge advertises `tools.listChanged` and sends
`notifications/tools/list_changed` the first time a live OrcaSlicer answers after a snapshot was
served. A client that honours it re-fetches and picks up the current schemas without a restart.

After adding or changing a tool's parameters, regenerate the snapshot against a running build:

```bash
python3 scripts/regen_tools_schema.py
python3 -m pytest scripts/tests/test_tools_schema.py -v   # compares snapshot to the live server
```
````

- [ ] **Step 8: Commit**

```bash
cd /Users/hanan/Projects/OrcaMCP
git add scripts/orcamcp-bridge.py scripts/tools_schema.py scripts/tests/test_bridge_schema_refresh.py scripts/tests/test_tools_schema.py docs/tools/reference.md
git commit -m "$(cat <<'EOF'
fix: a client could keep last release's tool signatures forever (T2, schema half)

The session could not send get_presets' filters because the client advertised
"properties": {} for it. Established that this is the bridge, not the client:
scripts/tools_schema.py is a checked-in file regenerated by hand, and it still held
the pre-item-H entry. orcamcp-bridge.py:354 answers the FIRST tools/list from it
whenever a 0.1-second probe fails -- the normal case, because the MCP client starts
before OrcaSlicer -- and initialize advertised capabilities {"tools": {}}, with no
listChanged, so the client had no reason ever to ask again.

Two fixes, because either alone leaves the hole: the stale get_presets entry is
refreshed, and the bridge now advertises tools.listChanged and emits
notifications/tools/list_changed the first time a live server answers after a
snapshot was served. That makes every future stale entry self-heal on connect
instead of lasting the session.

Guarded offline: test_tools_schema.py's live comparison skips when nothing is
listening, which is exactly when that file gets hand-edited, so the get_presets
entry is pinned by an offline test too.

Deliberately not changed: the snapshot stays a checked-in file. It is what makes the
tools visible while OrcaSlicer is closed, which is how start_orca is reachable at
all.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: The colour recipe module (pure)

**Files:**
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.hpp`
- Create: `src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.cpp`
- Create: `tests/slic3rutils/test_color_recipe.cpp`
- Modify: `src/slic3r/CMakeLists.txt:416` (after `OrcaMCPConfigKeys.hpp`)
- Modify: `tests/slic3rutils/CMakeLists.txt` (after `test_hex_color.cpp`)

**Interfaces:**
- Consumes: `Slic3r::ColorDecomposeRecipeResult`, `Slic3r::ColorDecomposeRecipeComponent` (`src/libslic3r/ColorDecomposeRecipe.hpp:29-41`).
- Produces:
  ```cpp
  namespace Slic3r { namespace GUI { namespace OrcaMCP {
  struct ColorMixRecipe {
      std::vector<unsigned int> components;   // 1-based filament slots
      std::vector<int>          ratios;       // percent, sums to 100
      std::vector<std::string>  hexes;        // each component's own colour
      bool                      exact_match = false;
      bool                      valid = false;
  };
  ColorMixRecipe color_mix_recipe_from_result(const ColorDecomposeRecipeResult& result);

  constexpr double kGamutDeltaEThreshold = 20.0;
  const char* gamut_label(double delta_e);              // "inside" | "outside"
  std::vector<int> unreachable_hue_sectors(const std::vector<double>& hues);
  }}}
  ```
  Task 8 uses `ColorMixRecipe` / `color_mix_recipe_from_result`; Task 9 uses the other three.

- [ ] **Step 1: Write the failing test**

Create `tests/slic3rutils/test_color_recipe.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.hpp"

// The decisions behind suggest_color_mix and get_color_palette that do not need a printer: what a
// recommend_from_physical_filaments result means, how far is too far, and which colours the loaded
// filaments cannot reach at all.

using Slic3r::ColorDecomposeRecipeComponent;
using Slic3r::ColorDecomposeRecipeResult;
using Slic3r::GUI::OrcaMCP::color_mix_recipe_from_result;
using Slic3r::GUI::OrcaMCP::gamut_label;
using Slic3r::GUI::OrcaMCP::kGamutDeltaEThreshold;
using Slic3r::GUI::OrcaMCP::unreachable_hue_sectors;

namespace {

ColorDecomposeRecipeComponent component(unsigned int slot, const char* hex, int ratio)
{
    ColorDecomposeRecipeComponent c;
    c.filament_index = slot;
    c.color_hex = hex;
    c.ratio = ratio;
    return c;
}

} // namespace

TEST_CASE("a two-component result is an ordinary mix", "[orcamcp][colorrecipe]")
{
    ColorDecomposeRecipeResult result;
    result.valid = true;
    result.components = {component(2, "#FF00FF", 40), component(3, "#FFFF00", 60)};

    const auto recipe = color_mix_recipe_from_result(result);

    CHECK(recipe.valid);
    CHECK_FALSE(recipe.exact_match);
    CHECK(recipe.components == std::vector<unsigned int>{2, 3});
    CHECK(recipe.ratios == std::vector<int>{40, 60});
    CHECK(recipe.hexes == std::vector<std::string>{"#FF00FF", "#FFFF00"});
}

TEST_CASE("a one-component result is an exact match, not a failure", "[orcamcp][colorrecipe]")
{
    // T4: "target_color already matches physical filament 1" is the useful answer "load slot 1",
    // and it was being returned as status: error, which stops an agent walking a palette.
    ColorDecomposeRecipeResult result;
    result.valid = true;
    result.components = {component(1, "#00FFFF", 100)};

    const auto recipe = color_mix_recipe_from_result(result);

    CHECK(recipe.valid);
    CHECK(recipe.exact_match);
    CHECK(recipe.components == std::vector<unsigned int>{1});
    // The ratio is forced to 100 whatever the recipe table said: one component is all of it.
    CHECK(recipe.ratios == std::vector<int>{100});
    CHECK(recipe.hexes == std::vector<std::string>{"#00FFFF"});
}

TEST_CASE("an invalid or empty result stays invalid", "[orcamcp][colorrecipe]")
{
    ColorDecomposeRecipeResult not_valid;
    CHECK_FALSE(color_mix_recipe_from_result(not_valid).valid);

    ColorDecomposeRecipeResult valid_but_empty;
    valid_but_empty.valid = true;
    CHECK_FALSE(color_mix_recipe_from_result(valid_but_empty).valid);
}

TEST_CASE("the gamut label has a documented threshold", "[orcamcp][colorrecipe]")
{
    CHECK(kGamutDeltaEThreshold == 20.0);
    // The session's own numbers: good mixes landed at 3.6-14, pure red at 54.
    CHECK(std::string(gamut_label(0.0)) == "inside");
    CHECK(std::string(gamut_label(3.6)) == "inside");
    CHECK(std::string(gamut_label(14.0)) == "inside");
    CHECK(std::string(gamut_label(19.9)) == "inside");
    CHECK(std::string(gamut_label(20.0)) == "outside");
    CHECK(std::string(gamut_label(54.0)) == "outside");
}

TEST_CASE("empty hue sectors name the colours a palette cannot reach", "[orcamcp][colorrecipe]")
{
    // Cyan (180), magenta (300) and yellow (60) alternate layers, so mixes land BETWEEN their
    // hues -- never at red (0) or blue (240). That is the whole finding, expressed as a sector.
    const std::vector<double> cmy_palette = {60, 90, 120, 150, 180, 210, 240 - 30, 300, 330};
    const auto unreachable = unreachable_hue_sectors(cmy_palette);

    CHECK(std::find(unreachable.begin(), unreachable.end(), 0) != unreachable.end());
    CHECK(std::find(unreachable.begin(), unreachable.end(), 30) != unreachable.end());
    CHECK(std::find(unreachable.begin(), unreachable.end(), 180) == unreachable.end());
}

TEST_CASE("a palette that covers the circle leaves nothing unreachable", "[orcamcp][colorrecipe]")
{
    std::vector<double> every_sector;
    for (int degrees = 0; degrees < 360; degrees += 30)
        every_sector.push_back(degrees + 5.0);

    CHECK(unreachable_hue_sectors(every_sector).empty());
}

TEST_CASE("an empty palette cannot reach anything", "[orcamcp][colorrecipe]")
{
    CHECK(unreachable_hue_sectors({}).size() == 12);
}
```

Register it in `tests/slic3rutils/CMakeLists.txt` with one line after `test_hex_color.cpp`:

```cmake
    test_color_recipe.cpp
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
```
Expected: FAIL at compile time — `OrcaMCPColorRecipe.hpp` does not exist.

- [ ] **Step 3: Create the header**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.hpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.hpp
#pragma once
#include <string>
#include <vector>

#include "libslic3r/ColorDecomposeRecipe.hpp"

// The colour decisions behind suggest_color_mix and get_color_palette that need no printer, no
// preset bundle and no wx -- deliberately, so Catch2 can drive them. Everything here is pure.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// A recipe as the MCP response describes it: which slots, in what percentages.
struct ColorMixRecipe
{
    std::vector<unsigned int> components;         // 1-based filament slots
    std::vector<int>          ratios;             // percent, sums to 100
    std::vector<std::string>  hexes;              // each component's own colour
    bool                      exact_match = false;
    bool                      valid = false;
};

// What recommend_from_physical_filaments returned, read as a recipe. A one-component result is not
// a failure: it means the target is already loaded, so it comes back as that slot at 100% with
// exact_match set. Reporting it as an error is what stopped an agent walking a set of targets.
ColorMixRecipe color_mix_recipe_from_result(const ColorDecomposeRecipeResult& result);

// A mixed slot alternates layers of its components, so the result is close to a weighted average
// of the component RGB values -- not subtractive pigment mixing. Cyan, magenta and yellow
// therefore cannot make red or blue, and the recipe engine will still hand back its closest
// attempt. This threshold is where "closest attempt" stops being an answer: CIE76 delta E 20.
// The session that prompted it saw usable mixes at 3.6-14 and pure red at 54.
constexpr double kGamutDeltaEThreshold = 20.0;

// "inside" below the threshold, "outside" at or above it.
const char* gamut_label(double delta_e);

// Which 30-degree hue sectors (0, 30, ... 330; 0 is red) no colour in `hues` falls into. A palette
// that reaches no colour in a sector cannot mix one: with cyan, magenta and yellow loaded, the red
// and orange sectors come back empty, which is what "out of gamut" means for that machine.
// `hues` are degrees in [0, 360); anything outside is wrapped.
std::vector<int> unreachable_hue_sectors(const std::vector<double>& hues);

}}} // namespace Slic3r::GUI::OrcaMCP
```

- [ ] **Step 4: Create the implementation**

Create `src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.cpp`:

```cpp
// src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.cpp
#include "OrcaMCPColorRecipe.hpp"

#include <cmath>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {
constexpr int kHueSectorDegrees = 30;
constexpr int kHueSectorCount   = 360 / kHueSectorDegrees;
} // namespace

ColorMixRecipe color_mix_recipe_from_result(const ColorDecomposeRecipeResult& result)
{
    ColorMixRecipe recipe;
    if (!result.valid || result.components.empty())
        return recipe;

    recipe.valid = true;
    recipe.exact_match = result.components.size() == 1;
    for (const auto& component : result.components) {
        recipe.components.push_back(component.filament_index);
        recipe.hexes.push_back(component.color_hex);
        // One component is all of it, whatever ratio the recipe table happened to carry.
        recipe.ratios.push_back(recipe.exact_match ? 100 : component.ratio);
    }
    return recipe;
}

const char* gamut_label(double delta_e)
{
    return delta_e < kGamutDeltaEThreshold ? "inside" : "outside";
}

std::vector<int> unreachable_hue_sectors(const std::vector<double>& hues)
{
    bool reached[kHueSectorCount] = {false};
    for (double hue : hues) {
        double wrapped = std::fmod(hue, 360.0);
        if (wrapped < 0.0)
            wrapped += 360.0;
        reached[int(wrapped) / kHueSectorDegrees] = true;
    }

    std::vector<int> unreachable;
    for (int sector = 0; sector < kHueSectorCount; ++sector)
        if (!reached[sector])
            unreachable.push_back(sector * kHueSectorDegrees);
    return unreachable;
}

}}} // namespace Slic3r::GUI::OrcaMCP
```

Add both files to `src/slic3r/CMakeLists.txt` immediately after the `OrcaMCPConfigKeys.hpp` line (line 416):

```cmake
    GUI/OrcaMCP/OrcaMCPColorRecipe.hpp
    GUI/OrcaMCP/OrcaMCPColorRecipe.cpp
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 \
&& /Users/hanan/Projects/OrcaMCP/build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[colorrecipe]"
```
Expected: PASS, 7 test cases. If `std::find` is undeclared, add `#include <algorithm>` to the test file.

- [ ] **Step 6: Commit**

```bash
cd /Users/hanan/Projects/OrcaMCP
git add src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.cpp src/slic3r/CMakeLists.txt tests/slic3rutils/test_color_recipe.cpp tests/slic3rutils/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat: the colour decisions behind the mix tools, without a printer attached

suggest_color_mix and get_color_palette decide three things that need nothing from
wx, the preset bundle or a machine: what a recommend_from_physical_filaments result
means, how far from a target is too far, and which hues the loaded filaments cannot
reach at all. All three lived inside handler lambdas where no test could reach them.

Its own file rather than OrcaMCPFilamentUtils, which pulls MixedFilamentDialog and
therefore wx into any translation unit that includes it -- including a test one.

Pure and unwired in this commit; suggest_color_mix and get_color_palette follow.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: An exact colour match is a success

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp:2-13` (includes), `:289-344` (the `suggest_color_mix` body)
- Modify: `docs/tools/reference.md:981` (insert a new section before `## Printer Tools`), and `:5-26` (the Quick Reference Table)

**Interfaces:**
- Consumes: `Slic3r::GUI::OrcaMCP::ColorMixRecipe`, `Slic3r::GUI::OrcaMCP::color_mix_recipe_from_result` (Task 7).
- Produces: the `suggest_color_mix` response shape Task 9 extends — `{"status", "target_color", "recipe": {"components", "ratios", "predicted_color", "measured"}, "delta_e", "exact_match", "slot"}`.

- [ ] **Step 1: Write the failing test**

The exact-match decision is Task 7's `color_mix_recipe_from_result`, already covered. What this task must additionally pin is that the ratio a one-component result carries is normalised even when the source component says otherwise — the case that would silently produce a broken `set_mixed_filament` call. Add to `tests/slic3rutils/test_color_recipe.cpp`:

```cpp
TEST_CASE("an exact match reports 100 even if the source said otherwise", "[orcamcp][colorrecipe]")
{
    // recommend_from_physical_filaments fills ratio from its recipe table; for a single component
    // that number is not necessarily 100, and suggest_color_mix's response promises ratios that
    // sum to 100 -- a caller feeds them straight to set_mixed_filament.
    ColorDecomposeRecipeResult result;
    result.valid = true;
    result.components = {component(4, "#00FFFF", 70)};

    const auto recipe = color_mix_recipe_from_result(result);

    CHECK(recipe.exact_match);
    CHECK(recipe.ratios == std::vector<int>{100});
}
```

- [ ] **Step 2: Run the test to verify it passes on the pure layer**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 \
&& /Users/hanan/Projects/OrcaMCP/build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "an exact match reports 100 even if the source said otherwise"
```
Expected: PASS if Task 7 landed as written.

- [ ] **Step 3: Rewire the handler**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp`, add after the existing includes (line 7):

```cpp
#include "OrcaMCPColorRecipe.hpp"
```

Replace lines 289-344 (from `const ColorDecomposeRecipeResult result =` through the closing `return out;`) with:

```cpp
                const ColorDecomposeRecipeResult result =
                    recommend_from_physical_filaments(target, physicals, effective_material_type);
                const ColorMixRecipe recipe = color_mix_recipe_from_result(result);
                if (!recipe.valid)
                    return nlohmann::json{{"status", "error"}, {"message", "no mixable recipe found for target_color"}};

                // An exact match is the useful answer "load that slot, no mix required" -- not a
                // failure. It used to be returned as status: error, so an agent walking a set of
                // target colours had to match on the message text to carry on. status: error is
                // now reserved for calls that could not be answered at all.
                const std::string predicted_color =
                    recipe.exact_match ? recipe.hexes.front() : gui_mix_color(recipe.hexes, recipe.ratios);
                const double delta_e =
                    recipe.exact_match ? 0.0 : color_delta_e_hex(target_color, predicted_color);

                nlohmann::json out = {
                    {"status", "success"},
                    {"target_color", target_color},
                    {"recipe", {
                        {"components", recipe.components},
                        {"ratios", recipe.ratios},
                        {"predicted_color", predicted_color},
                        {"measured", false}
                    }},
                    {"delta_e", delta_e},
                    {"exact_match", recipe.exact_match},
                    {"slot", nullptr}
                };
                if (recipe.exact_match)
                    out["message"] = "target_color already matches physical filament " +
                                     std::to_string(recipe.components.front()) +
                                     " (" + recipe.hexes.front() + "); no mix needed";

                if (create) {
                    // An exact match has nothing to create: apply_mixed_filament needs 2-3
                    // components, and the slot the caller wants is already loaded.
                    if (recipe.exact_match) {
                        out["slot"] = recipe.components.front();
                        out["created"] = false;
                    } else {
                        // Route through the same params-shaped entry point set_mixed_filament uses,
                        // rather than hand-building a MixedFilamentResult, so there is one place
                        // (mixed_result_from_params) that turns {components, ratios} into a request.
                        const nlohmann::json synthetic_params = {{"components", recipe.components},
                                                                 {"ratios", recipe.ratios}};
                        MixedFilamentResult req;
                        std::string error;
                        if (!mixed_result_from_params(synthetic_params, req, error))
                            return nlohmann::json{{"status", "error"}, {"message", error}};
                        const int idx = wxGetApp().sidebar().apply_mixed_filament(req, -1, error);
                        if (idx < 0)
                            return nlohmann::json{{"status", "error"}, {"message", error}};
                        out["slot"] = idx + 1;
                        out["created"] = true;
                    }
                    out["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                }
                return out;
```

Note `gui_mix_color` keeps its existing contract: it is never consulted for an exact match, because the predicted colour of "just use slot N" is slot N's own colour, exactly.

- [ ] **Step 4: Document the two colour tools**

`docs/tools/reference.md` has no filament or colour section at all. Insert this whole section immediately before `## Printer Tools` (line 981):

````markdown
## Filament & Colour Tools

### suggest_color_mix
Suggest the closest achievable 2–3 component filament mix for a target colour, from the printer's
loaded physical filaments. Optionally create the mixed slot.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `target_color` | string | Yes | Target colour, exactly `#RRGGBB` |
| `material_type` | string | No | Restrict components to this filament type (e.g. `PLA`). Default: the type of filament slot 1. |
| `create` | boolean | No | Create the mixed slot. Default `false`. |

**Returns:**
```json
{
  "status": "success",
  "target_color": "#BA44ED",
  "recipe": {
    "components": [1, 2],
    "ratios": [50, 50],
    "predicted_color": "#BC7FF7",
    "measured": false
  },
  "delta_e": 8.4,
  "exact_match": false,
  "slot": null
}
```

**Exact matches are successes.** When `target_color` is already one of the loaded filaments, the
answer is "load that slot, no mix required" — a single-component recipe at ratio 100,
`delta_e: 0`, `exact_match: true`, `status: "success"` and an explanatory `message`. `status:
"error"` is reserved for calls that could not be answered. With `create: true` an exact match
reports the matching slot and `created: false`; there is nothing to create.

**How the mix actually works — read this before choosing filaments.** A mixed slot **alternates
layers** of its components, so the result is close to a weighted average of the component RGB
values, *not* subtractive pigment mixing. Cyan + magenta gives lavender, not blue; magenta + yellow
gives salmon, not red. A CMY filament set does not behave like printer inks.

### get_color_palette
Enumerate an achievable palette of filament mixes (pairs, and optionally triples) from the loaded
physical filaments — a shortlist to choose from before painting.

**Parameters:**
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `max_count` | integer | No | Max entries. Default 12, cap 48. |
| `max_components` | integer | No | `2` for pairs only, `3` to include triples. Default 2. |
| `material_type` | string | No | Restrict to this filament type. |

**Returns:**
```json
{
  "status": "success",
  "palette": [
    {"components": [1, 2], "ratios": [70, 30], "predicted_color": "#5FC9E8", "measured": true}
  ]
}
```

`measured: true` means the colour came from the standard recipe table rather than the blend model.
Entries within ΔE 5 of a loaded filament, or of an already-accepted entry, are dropped, and the
list is ordered by hue.

---
````

Also add a row to the Quick Reference Table at the top of the file, after the `**Presets**` row:

```markdown
| **Filament & Colour** | `get_filaments`, `set_mixed_filament`, `delete_mixed_filament`, `set_object_filament`, `get_flush_volumes`, `set_flush_volumes`, `auto_calc_flush_volumes`, `get_toolchanger_config`, `suggest_color_mix`, `get_color_palette` |
```

- [ ] **Step 5: Build and run the full suite**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8 \
&& /Users/hanan/Projects/OrcaMCP/build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
```
Expected: PASS, 0 failures.

- [ ] **Step 6: Commit**

```bash
cd /Users/hanan/Projects/OrcaMCP
git add src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp tests/slic3rutils/test_color_recipe.cpp docs/tools/reference.md
git commit -m "$(cat <<'EOF'
fix: suggest_color_mix called its most useful answer an error (T4)

"target_color already matches physical filament 1 (#00FFFF); no mix needed" is the
right answer -- load slot 1 -- and it came back as status: error, so an agent
walking a set of target colours stopped partway and had to match on message text to
carry on. Root cause: the handler treated components.size() < 2 as a failure of the
recipe engine rather than as a recipe with one component.

Now a single-component recipe at ratio 100, delta_e 0, exact_match true, status
success, with the message kept. The ratio is normalised to 100 whatever the recipe
table carried, because a caller feeds these straight to set_mixed_filament. With
create: true an exact match reports the matching slot and created: false rather
than trying to build a one-component mixed slot, which apply_mixed_filament would
reject.

Also documented: neither suggest_color_mix nor get_color_palette appeared anywhere
in docs/tools/reference.md -- there was no filament or colour section at all --
including the fact that a mixed slot averages layers instead of mixing pigment, so
nobody buys CMY filament expecting ink behaviour.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Out-of-gamut results say so

**Files:**
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.hpp:85` (`enumerate_mix_palette` doc comment) and `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.cpp:383-437` (`enumerate_mix_palette`)
- Modify: `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp` (the `suggest_color_mix` response built in Task 8, and the `get_color_palette` handler at lines 365-379)
- Modify: `docs/tools/reference.md` (the section added in Task 8)
- Modify: `docs/printers/flashforge-creator-5.md:157` (the `### Material mapping` section)

**Interfaces:**
- Consumes: `kGamutDeltaEThreshold`, `gamut_label`, `unreachable_hue_sectors` (Task 7); the `suggest_color_mix` response shape (Task 8).
- Produces: nothing later tasks depend on. This is the last task.

- [ ] **Step 1: Write the failing test**

`enumerate_mix_palette` needs a preset bundle, so the palette's gamut summary is built from a new pure helper. Add to `tests/slic3rutils/test_color_recipe.cpp`:

```cpp
TEST_CASE("unreachable sectors are reported as names a person can act on", "[orcamcp][colorrecipe]")
{
    using Slic3r::GUI::OrcaMCP::hue_sector_name;

    CHECK(std::string(hue_sector_name(0)) == "red");
    CHECK(std::string(hue_sector_name(30)) == "orange");
    CHECK(std::string(hue_sector_name(60)) == "yellow");
    CHECK(std::string(hue_sector_name(120)) == "green");
    CHECK(std::string(hue_sector_name(180)) == "cyan");
    CHECK(std::string(hue_sector_name(240)) == "blue");
    CHECK(std::string(hue_sector_name(300)) == "magenta");
    // Every sector the enumerator can produce has a name; nothing falls through to a number.
    for (int sector = 0; sector < 360; sector += 30)
        CHECK(std::string(hue_sector_name(sector)).empty() == false);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8
```
Expected: FAIL at compile time — `hue_sector_name` is not declared.

- [ ] **Step 3: Add the sector name**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.hpp`, after `unreachable_hue_sectors`:

```cpp
// A name for one of the sector boundaries unreachable_hue_sectors returns (0, 30, ... 330).
// "the red and orange sectors are unreachable" is actionable; "sectors 0 and 30" is not.
const char* hue_sector_name(int sector_degrees);
```

In `src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.cpp`, after `unreachable_hue_sectors`:

```cpp
const char* hue_sector_name(int sector_degrees)
{
    switch (((sector_degrees % 360) + 360) % 360) {
    case 0:   return "red";
    case 30:  return "orange";
    case 60:  return "yellow";
    case 90:  return "yellow-green";
    case 120: return "green";
    case 150: return "spring green";
    case 180: return "cyan";
    case 210: return "azure";
    case 240: return "blue";
    case 270: return "violet";
    case 300: return "magenta";
    case 330: return "rose";
    default:  return "off-sector";
    }
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j8 \
&& /Users/hanan/Projects/OrcaMCP/build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests "[colorrecipe]"
```
Expected: PASS, 9 test cases.

- [ ] **Step 5: Mark `suggest_color_mix`**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp`, in the `nlohmann::json out = {...}` initializer written in Task 8, after the `{"exact_match", recipe.exact_match},` line:

```cpp
                    // A mixed slot averages layers instead of mixing pigment, so whole regions of
                    // the colour wheel are simply unreachable -- the session that prompted this got
                    // a confident-looking recipe for pure red at delta_e 54. Say which side of the
                    // threshold this landed on rather than letting the number speak for itself.
                    {"gamut", gamut_label(delta_e)},
                    {"gamut_delta_e_threshold", kGamutDeltaEThreshold},
```

And immediately after the `out["message"] = ...` block, add:

```cpp
                if (std::string(gamut_label(delta_e)) == "outside")
                    out["message"] = "The closest mix is delta_e " + std::to_string(int(delta_e + 0.5)) +
                                     " from target_color, past the delta_e " +
                                     std::to_string(int(kGamutDeltaEThreshold)) +
                                     " gamut threshold. A mixed slot alternates layers, so it averages "
                                     "its components' colours rather than mixing them like pigment; this "
                                     "colour is not reachable from the loaded filaments. Load a filament "
                                     "closer to it instead.";
```

- [ ] **Step 6: Mark `get_color_palette`**

In `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.cpp`, replace the tail of `enumerate_mix_palette` (lines 427-437) with:

```cpp
    nlohmann::json palette = nlohmann::json::array();
    std::vector<double> hues;
    hues.reserve(accepted.size());
    for (const auto& c : accepted) {
        palette.push_back({
            {"components", c.components},
            {"ratios", c.ratios},
            {"predicted_color", c.predicted_color},
            {"measured", c.measured}
        });
        hues.push_back(hue_degrees(c.predicted_color));
    }
    // The palette IS the gamut: a hue no entry reaches cannot be mixed from these filaments. With
    // cyan, magenta and yellow loaded the red and orange sectors come back empty, which is the
    // honest form of "this set does not behave like printer inks".
    nlohmann::json unreachable = nlohmann::json::array();
    for (int sector : unreachable_hue_sectors(hues))
        unreachable.push_back({{"hue_degrees", sector}, {"name", hue_sector_name(sector)}});

    return {{"entries", palette}, {"unreachable_hues", unreachable}};
```

Change the declaration comment and return description in `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.hpp` (the last two lines of the `enumerate_mix_palette` comment, line 83-85):

```cpp
// Returns {"entries": [{"components": [1-based...], "ratios": [...], "predicted_color": "#RRGGBB",
// "measured": bool}...], "unreachable_hues": [{"hue_degrees": int, "name": string}...]}, where the
// second is the 30-degree hue sectors no entry reaches -- the colours this filament set cannot
// mix. Main thread only.
nlohmann::json enumerate_mix_palette(int max_count, int max_components, const std::string& material_type);
```

Add `#include "OrcaMCPColorRecipe.hpp"` to `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.cpp` beside its other OrcaMCP includes.

In `src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp`, update the `get_color_palette` handler body (lines 371-378) to unpack the new shape:

```cpp
            return run_on_main_thread([max_count, max_components, material_type]() -> nlohmann::json {
                nlohmann::json enumerated = enumerate_mix_palette(max_count, max_components, material_type);
                nlohmann::json r = {
                    {"status", "success"},
                    {"palette", enumerated["entries"]},
                    {"unreachable_hues", enumerated["unreachable_hues"]},
                    {"gamut_delta_e_threshold", kGamutDeltaEThreshold}
                };
                r["active_warnings"] = get_active_warnings_json(wxGetApp().plater());
                return r;
            });
```

- [ ] **Step 7: Update the docs**

In `docs/tools/reference.md`, in the `### suggest_color_mix` returns block added in Task 8, add the two new fields:

```json
  "delta_e": 8.4,
  "exact_match": false,
  "gamut": "inside",
  "gamut_delta_e_threshold": 20.0,
  "slot": null
```

And add after the "How the mix actually works" paragraph:

````markdown
**Gamut.** `gamut` is `"inside"` while `delta_e` is below `gamut_delta_e_threshold` (CIE76 ΔE 20)
and `"outside"` at or above it, with a `message` explaining why. The threshold is calibrated from
real results: usable mixes land at ΔE 3.6–14, while pure red from a cyan/magenta/yellow set lands
at ΔE 54. An `"outside"` recipe is the engine's closest attempt, not an answer — load a closer
filament instead of mixing.
````

In the `### get_color_palette` returns block, replace it with:

````markdown
**Returns:**
```json
{
  "status": "success",
  "palette": [
    {"components": [1, 2], "ratios": [70, 30], "predicted_color": "#5FC9E8", "measured": true}
  ],
  "unreachable_hues": [
    {"hue_degrees": 0, "name": "red"},
    {"hue_degrees": 30, "name": "orange"}
  ],
  "gamut_delta_e_threshold": 20.0
}
```

`unreachable_hues` lists the 30-degree hue sectors no palette entry reaches — the colours this
filament set cannot mix at all. With cyan, magenta and yellow loaded, red and orange come back
here, because alternating layers averages the components rather than mixing them like pigment.
````

In `docs/printers/flashforge-creator-5.md`, add this at the end of the `### Material mapping` section
(which starts at line 157), because the finding is machine-specific and the session showed it is the
thing people get wrong when buying filament for it:

````markdown
**Mixed slots average, they do not mix like pigment.** A mixed slot alternates layers of its
components, so the printed result is close to a weighted average of their RGB values. Magenta +
yellow at 40/60 gives a salmon (`#FA8B6C`), not red; cyan + magenta gives lavender (`#BA44ED`), not
blue. A cyan/magenta/yellow set therefore does **not** behave like printer inks, and the reds and
blues of the colour wheel are outside what it can reach at all.

`suggest_color_mix` marks that with `gamut: "outside"` past ΔE 20, and `get_color_palette` lists the
hue sectors nothing in the palette reaches under `unreachable_hues`. Buy filament for the colours
you want to land on, not for the primaries you would mix them from.
````

- [ ] **Step 8: Build and run the full suite**

```bash
cmake --build /Users/hanan/Projects/OrcaMCP/build/arm64 --config RelWithDebInfo --target OrcaSlicer slic3rutils_tests -- -j8 \
&& /Users/hanan/Projects/OrcaMCP/build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
```
Expected: PASS, 0 failures.

- [ ] **Step 9: Run every Python test too**

```bash
cd /Users/hanan/Projects/OrcaMCP && python3 -m unittest discover -s scripts/tests -p 'test_bridge*.py' -v \
&& python3 -m pytest scripts/tests/test_tools_schema.py -v
```
Expected: PASS (with `test_static_list_matches_running_server` skipped).

- [ ] **Step 10: Commit**

```bash
cd /Users/hanan/Projects/OrcaMCP
git add src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPColorRecipe.cpp src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.hpp src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentUtils.cpp src/slic3r/GUI/OrcaMCP/OrcaMCPFilamentTools.cpp tests/slic3rutils/test_color_recipe.cpp docs/tools/reference.md docs/printers/flashforge-creator-5.md
git commit -m "$(cat <<'EOF'
feat: a colour the filaments cannot make is now said to be unreachable

suggest_color_mix returned a confident-looking recipe for pure red at delta_e 54,
and get_color_palette gave no way to see that whole sectors of the wheel were
missing. Root cause is not a bug in either: a mixed slot alternates layers, so it
averages its components' RGB values instead of mixing them like pigment, and a
cyan/magenta/yellow set genuinely cannot reach red or blue. The engine's closest
attempt was being presented as an answer.

suggest_color_mix now carries gamut: inside|outside against a documented CIE76
delta_e 20 threshold, calibrated from the session's own numbers (usable mixes 3.6-14,
pure red 54), with a message saying why when it is outside.

get_color_palette reports unreachable_hues -- the 30-degree hue sectors no entry
reaches. The palette IS the achievable gamut, so its gaps are the answer; with CMY
loaded, red and orange come back, which is the honest form of "this is not ink".

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review

**1. Spec coverage**

| Spec requirement (Plan 1 section) | Task |
|---|---|
| T1 — accept a JSON array for list-typed keys, keep the joined string working | Task 1 (shaping), Task 2 (`apply_config`) |
| T1 — separate "unknown key" from "value not accepted", say what shape was expected | Task 2 (`unknown_keys` / `rejected_values` / `expected`) |
| T1 — sweep the sibling paths (`set_object_config`, the `print`/`filament`/`printer` branches) | Task 3 (`set_object_config`); the four `apply_config` branches share one loop, fixed in Task 2 and stated in its commit body |
| T1 note — establish whether sweep item I is the rejecter | Answered in *What the investigation found* — item I **is** the rejecter for `filament_colour`; for non-colour `coStrings` and for `coFloats` there is no rejection at all, it is silent corruption |
| T2 — cap the default, report the true total, hint naming the filter parameters | Task 4 |
| T2 — establish whether a stale schema survives an app update; if the bridge caches, fix the bridge | Answered in *What the investigation found* (it is the bridge: `scripts/tools_schema.py` plus the startup fast path); fixed in Task 6 |
| T3 — distinguish connection-refused from timeout/busy, matching advice | Task 5 |
| T3 second question — does the server serialise requests? | Answered: yes, one `io_service.run()` thread, every handler blocking on `run_on_main_thread`; documented in `troubleshooting.md` in Task 5 |
| T4 — exact match returns success, single component at 100, `delta_e: 0`, `exact_match: true`, message kept | Task 7 (pure), Task 8 (wiring) |
| Out-of-gamut marker on `suggest_color_mix` and `get_color_palette`, documented threshold | Task 7 (pure), Task 9 (wiring) |

No spec requirement in the Plan 1 section is unassigned.

**2. Placeholder scan**

Every code step carries real code. No "TBD", no "add validation", no "similar to Task N" — Task 3 repeats the shaping code rather than pointing at Task 2. No step is conditional; every file a step names was confirmed to exist at the line given. Task 2, Task 3 and Task 8 open with a test that passes against the previous task's output rather than a failing one — stated explicitly in their steps, because their deliverable is wiring a pure unit that already has failing-first cover.

**3. Type consistency**

- `ConfigValueText{ok, text, reason}` and `config_value_expected_shape(ConfigOptionType)` are defined in Task 1 and used with the same names and argument order in Tasks 2 and 3.
- `ApplyConfigResult::RejectedValue{key, reason, expected}` (Task 2) maps 1:1 onto the `rejected_values` JSON objects in both Task 2 and Task 3.
- `PresetListCount{matched, returned}`, `preset_query_effective_limit(int, bool)` and `preset_truncation_hint(const std::map<std::string, PresetListCount>&)` are declared in Task 4 Step 3 and called with those exact signatures in Steps 4 and 6.
- `LIVE` / `BUSY` / `DOWN` and `check_orcaslicer_connection(use_cache, timeout) -> str` (Task 5) are what Task 6 compares against.
- `ColorMixRecipe{components, ratios, hexes, exact_match, valid}` (Task 7) is destructured with those member names in Task 8; `gamut_label`, `kGamutDeltaEThreshold`, `unreachable_hue_sectors` and `hue_sector_name` are used in Task 9 exactly as declared.
- `get_presets`' description string and property set appear verbatim in Task 4 Step 6 and Task 6 Step 3, and are asserted by the test in Task 6 Step 1.

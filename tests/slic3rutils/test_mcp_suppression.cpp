#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <wx/defs.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPCommon.hpp"

// What an agent reads when MCP suppression answered a dialog for it. Until 2026-09-26 only the
// prompt's text was captured, so "Object too large: ... scale it down?" read the same whether the
// model had been scaled by 0.00328 or left alone. A prompt that offered a choice now carries the
// answer; an OK-only notice has nothing to answer and keeps its text as it was.

using namespace Slic3r::GUI;

TEST_CASE("a suppressed Yes/No prompt is answered Yes, and anything else OK", "[McpSuppression][orcamcp][suppression]")
{
    CHECK(mcp_default_answer(wxYES | wxNO) == wxID_YES);
    CHECK(mcp_default_answer(wxYES) == wxID_YES);  // "Object too large" past 10000x offers only Yes
    CHECK(mcp_default_answer(wxYES | wxNO | wxCANCEL) == wxID_YES);
    CHECK(mcp_default_answer(wxOK | wxCANCEL) == wxID_OK);
    CHECK(mcp_default_answer(wxOK) == wxID_OK);
    CHECK(mcp_default_answer(wxOK | wxICON_INFORMATION) == wxID_OK);
}

TEST_CASE("only a prompt with a choice records an answer", "[McpSuppression][orcamcp][suppression]")
{
    CHECK(mcp_prompt_offers_choice(wxYES | wxNO));
    CHECK(mcp_prompt_offers_choice(wxYES | wxICON_QUESTION));
    CHECK(mcp_prompt_offers_choice(wxOK | wxCANCEL));
    CHECK_FALSE(mcp_prompt_offers_choice(wxOK));
    CHECK_FALSE(mcp_prompt_offers_choice(wxOK | wxICON_WARNING));
}

TEST_CASE("answers are named the way the buttons read", "[McpSuppression][orcamcp][suppression]")
{
    CHECK(mcp_answer_label(wxID_YES) == "Yes");
    CHECK(mcp_answer_label(wxID_NO) == "No");
    CHECK(mcp_answer_label(wxID_OK) == "OK");
    CHECK(mcp_answer_label(wxID_CANCEL) == "Cancel");
}

TEST_CASE("an answered prompt reads '<prompt> (auto-answered <answer>)'", "[McpSuppression][orcamcp][suppression]")
{
    CHECK(mcp_answered_prompt("Object too large: scale it down?", "Yes") ==
          "Object too large: scale it down? (auto-answered Yes)");

    clear_mcp_suppressed_messages();
    add_mcp_suppressed_answer("Save before continuing?", "No: continued without saving");
    add_mcp_suppressed_message("Load 3MF: loading geometry data only.");
    CHECK(get_mcp_suppressed_messages() == std::vector<std::string>{
                                               "Save before continuing? (auto-answered No: continued without saving)",
                                               "Load 3MF: loading geometry data only."});
    clear_mcp_suppressed_messages();
}

TEST_CASE("a list of affected items is cut to a readable length", "[McpSuppression][orcamcp][suppression]")
{
    CHECK(mcp_list_summary({}, 8).empty());
    CHECK(mcp_list_summary({"layer_height"}, 8) == "layer_height");
    CHECK(mcp_list_summary({"a", "b", "c"}, 8) == "a, b, c");
    CHECK(mcp_list_summary({"a", "b", "c", "d"}, 2) == "a, b and 2 more");
}

// load_model's multipart parameter. A file whose objects sit at different heights raises "load as
// a single object with multiple parts?", which suppression answers Yes. Once merged, no tool splits
// the object again, so an agent that wanted the objects separate had no way to get them: the call
// that knows the answer now sets it for that one prompt, by key, for the rest of its guard.
TEST_CASE("a keyed prompt takes the answer its caller set, and only that prompt", "[McpSuppression][orcamcp][suppression]")
{
    clear_mcp_prompt_answers();
    CHECK(mcp_answer_for(wxYES | wxNO, MCP_PROMPT_MULTIPART).id == wxID_YES);

    set_mcp_prompt_answer(MCP_PROMPT_MULTIPART, wxID_NO);
    CHECK(mcp_answer_for(wxYES | wxNO, MCP_PROMPT_MULTIPART).id == wxID_NO);
    CHECK(mcp_answer_for(wxYES | wxNO, "").id == wxID_YES);                 // an untagged prompt
    CHECK(mcp_answer_for(wxYES | wxNO, "some_other_prompt").id == wxID_YES);

    clear_mcp_prompt_answers();
    CHECK(mcp_answer_for(wxYES | wxNO, MCP_PROMPT_MULTIPART).id == wxID_YES);
}

// Found in the orchestrator's acceptance pass: "(auto-answered Yes)" told an agent the objects had
// been merged, but not that it could have kept them apart. A tool that sets a keyed answer can add
// the way to the other outcome, and the recorded answer carries it.
TEST_CASE("a keyed answer can say how to get the other outcome", "[McpSuppression][orcamcp][suppression]")
{
    clear_mcp_prompt_answers();
    set_mcp_prompt_answer(MCP_PROMPT_MULTIPART, wxID_YES,
                          "pass multipart: \"separate\" to keep them as separate objects");
    const McpAnswer merged = mcp_answer_for(wxYES | wxNO, MCP_PROMPT_MULTIPART);
    CHECK(merged.id == wxID_YES);
    CHECK(mcp_answered_prompt("Multi-part object detected: load as one object?", merged.text) ==
          "Multi-part object detected: load as one object? "
          "(auto-answered Yes; pass multipart: \"separate\" to keep them as separate objects)");

    // Without a note, and for an untagged prompt, the answer is the button alone.
    set_mcp_prompt_answer(MCP_PROMPT_MULTIPART, wxID_NO);
    CHECK(mcp_answer_for(wxYES | wxNO, MCP_PROMPT_MULTIPART).text == "No");
    CHECK(mcp_answer_for(wxYES | wxNO, "").text == "Yes");
    CHECK(mcp_answer_for(wxOK, "").text == "OK");
    clear_mcp_prompt_answers();
}

// A modal dialog that no specific MCP handler answers blocks the GUI thread, and with it the MCP
// call, until someone clicks it: the texture importer and the sync-printer tips were found that
// way, one at a time. Every DPIDialog now falls back to Cancel under suppression.
TEST_CASE("a modal no handler answered is cancelled under MCP and shown otherwise", "[McpSuppression][orcamcp][suppression]")
{
    const McpUnhandledModal suppressed = mcp_unhandled_modal(true, "Filament grouping");
    CHECK(suppressed.suppress);
    CHECK(suppressed.answer == wxID_CANCEL);
    CHECK(suppressed.message == "Filament grouping was suppressed (auto-answered Cancel)");

    CHECK(mcp_unhandled_modal(true, "").message == "A dialog was suppressed (auto-answered Cancel)");
    CHECK_FALSE(mcp_unhandled_modal(false, "Filament grouping").suppress);
}

// show_error defers its dialog (CallAfter), so under MCP it opened after the call had returned and
// suppression was over: a failed load_model left an "OrcaMCP error" modal nobody answers. Under
// suppression on the GUI thread the error is captured instead. Off it, the dialog is still deferred:
// the capture lists belong to the GUI thread.
TEST_CASE("an error shown under MCP on the GUI thread is captured, not left for a dialog after the call", "[McpSuppression][orcamcp][suppression]")
{
    CHECK(mcp_captures_error(/*suppression_enabled=*/true, /*on_main_thread=*/true));
    CHECK_FALSE(mcp_captures_error(/*suppression_enabled=*/false, /*on_main_thread=*/true));
    CHECK_FALSE(mcp_captures_error(/*suppression_enabled=*/true, /*on_main_thread=*/false));
}

TEST_CASE("a captured error is one of the messages, and the notices leave it out", "[McpSuppression][orcamcp][suppression]")
{
    Slic3r::GUI::OrcaMCP::McpDialogSuppressionGuard guard;
    add_mcp_suppressed_message("Object too large (auto-answered Yes)");
    add_mcp_suppressed_error("Loading of a model file failed.");
    CHECK(guard.messages() == std::vector<std::string>{"Object too large (auto-answered Yes)", "Loading of a model file failed."});
    CHECK(guard.errors() == std::vector<std::string>{"Loading of a model file failed."});
    CHECK(guard.notices() == std::vector<std::string>{"Object too large (auto-answered Yes)"});
}

TEST_CASE("a nested guard keeps the errors its caller is collecting", "[McpSuppression][orcamcp][suppression]")
{
    Slic3r::GUI::OrcaMCP::McpDialogSuppressionGuard outer;
    add_mcp_suppressed_error("first");
    {
        Slic3r::GUI::OrcaMCP::McpDialogSuppressionGuard inner;
        add_mcp_suppressed_error("second");
    }
    CHECK(outer.errors() == std::vector<std::string>{"first", "second"});
}

TEST_CASE("a new call starts with no errors from the last one", "[McpSuppression][orcamcp][suppression]")
{
    {
        Slic3r::GUI::OrcaMCP::McpDialogSuppressionGuard earlier;
        add_mcp_suppressed_error("from an earlier call");
    }
    Slic3r::GUI::OrcaMCP::McpDialogSuppressionGuard guard;
    CHECK(guard.errors().empty());
    CHECK(guard.messages().empty());
}

TEST_CASE("a guard's report puts errors in error_messages and the rest in info_messages", "[McpSuppression][orcamcp][suppression]")
{
    Slic3r::GUI::OrcaMCP::McpDialogSuppressionGuard guard;
    const nlohmann::json quiet = guard.report({{"status", "success"}});
    CHECK_FALSE(quiet.contains("info_messages"));
    CHECK_FALSE(quiet.contains("error_messages"));

    add_mcp_suppressed_message("a notice");
    add_mcp_suppressed_error("an error");
    const nlohmann::json said = guard.report({{"status", "success"}});
    CHECK(said["info_messages"] == nlohmann::json::array({"a notice"}));
    CHECK(said["error_messages"] == nlohmann::json::array({"an error"}));
}

// export_gcode reported export_started when the app had refused the export with an error dialog
// ("Another export job is running."). A call the app answered with an error failed.
TEST_CASE("a response the app answered with an error fails with the error's words", "[McpSuppression][orcamcp][suppression]")
{
    Slic3r::GUI::OrcaMCP::McpDialogSuppressionGuard guard;
    CHECK(guard.fail_on_errors({{"status", "export_started"}})["status"] == "export_started");

    add_mcp_suppressed_error("Another export job is running.");
    const nlohmann::json failed = guard.fail_on_errors({{"status", "export_started"}, {"note", "written asynchronously"}});
    CHECK(failed["status"] == "error");
    CHECK(failed["message"] == "Another export job is running.");
    CHECK(failed["error_messages"] == nlohmann::json::array({"Another export job is running."}));
}

// load_model, load_project and new_project fail the same way: a failed load says what the app's
// error said -- an STL the reader cannot parse, G-code the processor cannot read -- rather than only
// "Failed to load model file", and one with no error dialog keeps its own words.
TEST_CASE("a failure with several error dialogs says all of them, one per line", "[McpSuppression][orcamcp][suppression]")
{
    Slic3r::GUI::OrcaMCP::McpDialogSuppressionGuard guard;
    const nlohmann::json own_words = guard.fail_on_errors({{"status", "error"}, {"message", "Failed to load model file"}});
    CHECK(own_words["message"] == "Failed to load model file");
    CHECK_FALSE(own_words.contains("error_messages"));

    add_mcp_suppressed_error("first");
    add_mcp_suppressed_error("second");
    const nlohmann::json failed = guard.fail_on_errors({{"status", "error"}, {"message", "Failed to load model file"}});
    CHECK(failed["message"] == "first\nsecond");
    CHECK(failed["error_messages"] == nlohmann::json::array({"first", "second"}));
}

// A settings change the slicer has not taken in yet is applied by the tool about to slice, report or
// export (OrcaMCP::apply_pending_update). That update can raise an error dialog, and one raised with
// no suppression open is a modal that blocks every later call: the helper takes the caller's guard,
// and runs only while it is open.
TEST_CASE("a pending settings update runs under the caller's suppression, which captures its errors", "[McpSuppression][orcamcp][suppression]")
{
    using namespace Slic3r::GUI::OrcaMCP;
    const PipelineState idle{};
    Slic3r::GUI::OrcaMCP::McpDialogSuppressionGuard guard;
    bool suppressed_while_applying = false;
    CHECK(apply_pending_update(guard, idle, /*scheduled=*/true, [&] {
        suppressed_while_applying = is_mcp_dialog_suppression_enabled();
        add_mcp_suppressed_error("Placeholder parser error");
    }));
    CHECK(suppressed_while_applying);
    CHECK(guard.errors() == std::vector<std::string>{"Placeholder parser error"});
}

TEST_CASE("a pending settings update is not applied while the pipeline is busy or nothing is pending", "[McpSuppression][orcamcp][suppression]")
{
    using namespace Slic3r::GUI::OrcaMCP;
    Slic3r::GUI::OrcaMCP::McpDialogSuppressionGuard guard;
    PipelineState slicing{};
    slicing.is_slicing      = true;
    slicing.process_working = true;
    bool applied = false;
    CHECK_FALSE(apply_pending_update(guard, slicing, /*scheduled=*/true, [&] { applied = true; }));
    CHECK_FALSE(apply_pending_update(guard, PipelineState{}, /*scheduled=*/false, [&] { applied = true; }));
    CHECK_FALSE(applied);
}

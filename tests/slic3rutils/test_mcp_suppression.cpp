#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <wx/defs.h>

#include "slic3r/GUI/GUI.hpp"

// What an agent reads when MCP suppression answered a dialog for it. Until 2026-09-26 only the
// prompt's text was captured, so "Object too large: ... scale it down?" read the same whether the
// model had been scaled by 0.00328 or left alone. A prompt that offered a choice now carries the
// answer; an OK-only notice has nothing to answer and keeps its text as it was.

using namespace Slic3r::GUI;

TEST_CASE("a suppressed Yes/No prompt is answered Yes, and anything else OK", "[orcamcp][suppression]")
{
    CHECK(mcp_default_answer(wxYES | wxNO) == wxID_YES);
    CHECK(mcp_default_answer(wxYES) == wxID_YES);  // "Object too large" past 10000x offers only Yes
    CHECK(mcp_default_answer(wxYES | wxNO | wxCANCEL) == wxID_YES);
    CHECK(mcp_default_answer(wxOK | wxCANCEL) == wxID_OK);
    CHECK(mcp_default_answer(wxOK) == wxID_OK);
    CHECK(mcp_default_answer(wxOK | wxICON_INFORMATION) == wxID_OK);
}

TEST_CASE("only a prompt with a choice records an answer", "[orcamcp][suppression]")
{
    CHECK(mcp_prompt_offers_choice(wxYES | wxNO));
    CHECK(mcp_prompt_offers_choice(wxYES | wxICON_QUESTION));
    CHECK(mcp_prompt_offers_choice(wxOK | wxCANCEL));
    CHECK_FALSE(mcp_prompt_offers_choice(wxOK));
    CHECK_FALSE(mcp_prompt_offers_choice(wxOK | wxICON_WARNING));
}

TEST_CASE("answers are named the way the buttons read", "[orcamcp][suppression]")
{
    CHECK(mcp_answer_label(wxID_YES) == "Yes");
    CHECK(mcp_answer_label(wxID_NO) == "No");
    CHECK(mcp_answer_label(wxID_OK) == "OK");
    CHECK(mcp_answer_label(wxID_CANCEL) == "Cancel");
}

TEST_CASE("an answered prompt reads '<prompt> (auto-answered <answer>)'", "[orcamcp][suppression]")
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

TEST_CASE("a list of affected items is cut to a readable length", "[orcamcp][suppression]")
{
    CHECK(mcp_list_summary({}, 8).empty());
    CHECK(mcp_list_summary({"layer_height"}, 8) == "layer_height");
    CHECK(mcp_list_summary({"a", "b", "c"}, 8) == "a, b, c");
    CHECK(mcp_list_summary({"a", "b", "c", "d"}, 2) == "a, b and 2 more");
}

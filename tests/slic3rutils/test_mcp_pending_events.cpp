#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <wx/app.h>
#include <wx/event.h>

#include "slic3r/GUI/OrcaMCP/OrcaMCPPendingEvents.hpp"

// How a tool handles, inside its call, the events an action of the app's queued for itself (show_view: the
// tab bar posts the plater its view change). wxEvtHandler::ProcessPendingEvents, called on an empty queue,
// returned with the queue's lock held; the slicing thread's next post to the plater then waited forever,
// holding the slicing process' mutex, and the main thread with it at the slice's next stop (2026-09-29).

using namespace Slic3r::GUI::OrcaMCP;

namespace {

// Posts one event to `handler` from another thread, as the slicing thread does, and says whether the post
// returned within two seconds. A post that never returns keeps its thread, and the handler, alive: both are
// leaked then, so the failing test does not free what the blocked thread still uses.
bool another_thread_can_post_to(wxEvtHandler* handler)
{
    auto        posted = std::make_shared<std::atomic<bool>>(false);
    std::thread poster([handler, posted] {
        wxQueueEvent(handler, new wxCommandEvent(wxEVT_BUTTON));
        *posted = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!*posted && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (*posted)
        poster.join();
    else
        poster.detach();
    return *posted;
}

} // namespace

TEST_CASE("handling a handler's queue when it is empty leaves another thread free to post to it", "[McpPendingEvents][orcamcp]")
{
    wxAppConsole app; // wx keeps a handler's queue only with an application object
    auto*        handler = new wxEvtHandler;
    int          handled = 0;
    handler->Bind(wxEVT_BUTTON, [&handled](wxCommandEvent&) { ++handled; });

    process_queued_events(*handler);

    const bool posted = another_thread_can_post_to(handler);
    CHECK(posted);
    if (!posted)
        return; // the handler stays with the blocked thread
    process_queued_events(*handler);
    CHECK(handled == 1);
    delete handler;
}

TEST_CASE("every event queued before the call is handled in it, not only the first", "[McpPendingEvents][orcamcp]")
{
    wxAppConsole app;
    wxEvtHandler handler;
    int          handled = 0;
    handler.Bind(wxEVT_BUTTON, [&handled](wxCommandEvent&) { ++handled; });
    for (int i = 0; i < 3; ++i)
        wxQueueEvent(&handler, new wxCommandEvent(wxEVT_BUTTON));

    process_queued_events(handler);

    CHECK(handled == 3);
}

TEST_CASE("an event that queues another for the same handler does not keep the call from returning", "[McpPendingEvents][orcamcp]")
{
    wxAppConsole app;
    wxEvtHandler handler;
    int          handled = 0;
    handler.Bind(wxEVT_BUTTON, [&handler, &handled](wxCommandEvent&) {
        ++handled;
        wxQueueEvent(&handler, new wxCommandEvent(wxEVT_BUTTON)); // the next one is the event loop's
    });
    wxQueueEvent(&handler, new wxCommandEvent(wxEVT_BUTTON));

    process_queued_events(handler);

    CHECK(handled == 1);
    handler.DeletePendingEvents();
}

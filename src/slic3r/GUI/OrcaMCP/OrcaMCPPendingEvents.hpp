// src/slic3r/GUI/OrcaMCP/OrcaMCPPendingEvents.hpp
#pragma once

class wxEvtHandler;

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Handles, now, the events queued on `handler` when it is called -- what an action of the app's posted
// for itself (the tab bar posts the plater its view change) -- so they run inside the tool call, under
// its dialog suppression, rather than after it returned. Events those events queue are left to the
// event loop, as are events the running loop may not handle now (a wxYield's), so it always returns.
//
// Never wxEvtHandler::ProcessPendingEvents on its own: it handles one event, and called on an empty
// queue it returns with the queue's lock held (event.cpp: its wxCHECK_RET runs inside the critical
// section), so the next wxQueueEvent from another thread -- the slicing thread's, holding
// BackgroundSlicingProcess' mutex -- waits forever, and so does the main thread at the slice's next
// stop. wx has no public test for one handler's queue; this reads it under its lock.
void process_queued_events(wxEvtHandler& handler);

}}} // namespace Slic3r::GUI::OrcaMCP

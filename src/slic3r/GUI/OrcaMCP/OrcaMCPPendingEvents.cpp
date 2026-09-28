// src/slic3r/GUI/OrcaMCP/OrcaMCPPendingEvents.cpp
#include "OrcaMCPPendingEvents.hpp"

#include <wx/event.h>
#include <wx/list.h>
#include <wx/thread.h>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// wxEvtHandler keeps its queue and the queue's lock protected. A class derived from it may name them,
// and a pointer to a member so named reads them on any handler.
struct QueueOf : wxEvtHandler
{
    static size_t queued(wxEvtHandler& handler)
    {
#if wxUSE_THREADS
        wxCriticalSectionLocker locker(handler.*(&QueueOf::m_pendingEventsLock));
#endif
        const wxList* events = handler.*(&QueueOf::m_pendingEvents);
        return events == nullptr ? 0 : events->GetCount();
    }
};

} // namespace

void process_queued_events(wxEvtHandler& handler)
{
    if (wxTheApp == nullptr)
        return; // wx handles a queue only with an application object
    // At most as many calls as events queued now: each call handles one, or none when the running loop may
    // not handle it yet (inside a wxYield), and one may queue another.
    for (size_t calls = QueueOf::queued(handler); calls > 0 && QueueOf::queued(handler) > 0; --calls)
        handler.ProcessPendingEvents();
}

}}} // namespace Slic3r::GUI::OrcaMCP

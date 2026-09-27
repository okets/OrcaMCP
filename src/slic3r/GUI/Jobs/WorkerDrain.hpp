#ifndef slic3r_GUI_WorkerDrain_hpp_
#define slic3r_GUI_WorkerDrain_hpp_

// Orca: emptying a worker from a destructor, which must not throw.

#include <chrono>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

class Worker;

struct WorkerDrain
{
    bool                     idle = false; // every job ended and was delivered before the deadline
    std::vector<std::string> escaped;      // what deliveries threw, in order
};

// Main thread. Cancels every job and delivers what they still send -- a finalize, a main-thread call,
// a status -- until the worker is idle or `timeout` has passed, however often they send. What a
// delivery throws (a finalize that left its error set, a main-thread call's own) is logged and
// recorded, and the drain goes on with the rest.
WorkerDrain drain_worker(Worker& worker, std::chrono::milliseconds timeout, const char* worker_name) noexcept;

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_WorkerDrain_hpp_

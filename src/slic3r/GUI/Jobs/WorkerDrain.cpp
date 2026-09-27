#include "WorkerDrain.hpp"

#include "Worker.hpp"

#include <boost/log/trivial.hpp>

#include <exception>
#include <thread>

namespace Slic3r { namespace GUI {

namespace {

constexpr std::chrono::milliseconds k_drain_poll{5};

std::string describe(const std::exception_ptr& escaped)
{
    try {
        std::rethrow_exception(escaped);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "an exception that is not a std::exception";
    }
}

void note_escape(WorkerDrain& drain, const std::exception_ptr& escaped, const char* worker_name) noexcept
{
    try {
        std::string what = describe(escaped);
        BOOST_LOG_TRIVIAL(error) << "Draining worker '" << worker_name << "': a delivery threw (" << what << "); going on";
        drain.escaped.push_back(std::move(what));
    } catch (...) {}
}

// Delivers what the worker has queued; false when a delivery threw (noted), the rest still queued.
bool deliver_queued(Worker& worker, WorkerDrain& drain, const char* worker_name) noexcept
{
    try {
        worker.process_events();
        return true;
    } catch (...) {
        note_escape(drain, std::current_exception(), worker_name);
        return false;
    }
}

} // namespace

WorkerDrain drain_worker(Worker& worker, std::chrono::milliseconds timeout, const char* worker_name) noexcept
{
    WorkerDrain drain;
    try {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        worker.cancel_all();
        for (;;) {
            const bool delivered_all = deliver_queued(worker, drain, worker_name);
            if (delivered_all && worker.is_idle()) {
                drain.idle = true;
                return drain;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                break;
            if (delivered_all)
                std::this_thread::sleep_for(k_drain_poll);
        }
        BOOST_LOG_TRIVIAL(warning) << "Worker '" << worker_name << "' still busy after " << timeout.count()
                                   << " ms of draining: a job ignores its cancel";
    } catch (...) {
        note_escape(drain, std::current_exception(), worker_name);
    }
    return drain;
}

}} // namespace Slic3r::GUI

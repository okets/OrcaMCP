#include "OrcaMCPMainThreadGate.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

MainThreadGate::MainThreadGate(Post post) : m_calls(std::move(post)) {}

nlohmann::json MainThreadGate::call(Work work)
{
    // By reference: QueuedCalls never runs the work once this caller has been released, and waits for
    // it once it has started.
    nlohmann::json value;
    if (!m_calls.run([&] { value = work(); }))
        throw McpShuttingDown();
    return value;
}

bool MainThreadGate::close() { return m_calls.close(); }

bool MainThreadGate::is_closed() const { return m_calls.is_closed(); }

bool MainThreadGate::defer_until_work_ends(std::function<void()> task) { return m_calls.defer_until_work_ends(std::move(task)); }

}}} // namespace Slic3r::GUI::OrcaMCP

#include "ThreadCancel.hpp"

namespace Slic3r {

namespace {
thread_local const std::function<bool()>* t_cancelled = nullptr;
}

ScopedThreadCancelCheck::ScopedThreadCancelCheck(std::function<bool()> cancelled)
    : m_cancelled(std::move(cancelled)), m_previous(t_cancelled)
{
    t_cancelled = &m_cancelled;
}

ScopedThreadCancelCheck::~ScopedThreadCancelCheck() { t_cancelled = m_previous; }

bool this_thread_cancelled() { return t_cancelled != nullptr && *t_cancelled && (*t_cancelled)(); }

} // namespace Slic3r

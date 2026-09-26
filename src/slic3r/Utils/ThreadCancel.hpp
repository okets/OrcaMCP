#ifndef slic3r_ThreadCancel_hpp_
#define slic3r_ThreadCancel_hpp_

#include <functional>

namespace Slic3r {

// A cancellation check for the blocking calls one thread makes, installed for a scope.
//
// Why: the HTTP server's thread serves MCP calls and login callbacks that block on the network --
// printer discovery for up to a minute, Flashforge requests, cloud sign-in exchanges. When the app
// quits it joins that thread, and it must neither wait out such a call nor abandon the thread while
// the call still uses what the app is about to destroy. With a check in scope, every synchronous
// Http transfer on the thread (Http::perform_sync) is aborted once the check says so, and reports
// "Request cancelled" to its error callback; Flashforge discovery stops too. Other threads, and Http's
// asynchronous transfers, are unaffected. Unit-tested in tests/slic3rutils/test_thread_cancel.cpp.
class ScopedThreadCancelCheck
{
public:
    explicit ScopedThreadCancelCheck(std::function<bool()> cancelled);
    ~ScopedThreadCancelCheck(); // restores the check this one replaced
    ScopedThreadCancelCheck(const ScopedThreadCancelCheck&)            = delete;
    ScopedThreadCancelCheck& operator=(const ScopedThreadCancelCheck&) = delete;

private:
    std::function<bool()>        m_cancelled;
    const std::function<bool()>* m_previous;
};

// True when the check in scope on this thread says its work is cancelled; false with none in scope.
bool this_thread_cancelled();

} // namespace Slic3r

#endif

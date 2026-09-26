#ifndef slic3r_Utils_FlashforgeLocalApi_hpp_
#define slic3r_Utils_FlashforgeLocalApi_hpp_

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "FlashforgeApi.hpp"

namespace Slic3r { namespace FlashforgeLocalApi {

// How the Flashforge local API is reached: which host, which URL. FlashforgeApi is what the API
// says; this is how a request gets there. Pure, so tests/slic3rutils/test_flashforge_local_api.cpp
// can pin every rule without a printer.

// The local API always listens here, whatever port the preset's print_host carries.
constexpr int kPort = 8898;

// The bare host the local API is reached at, from any address shape a preset or DeviceManager holds:
// "10.0.0.100", "10.0.0.100:8080", "http://10.0.0.100:8080/path", "printer.local/", "[fe80::1]:80".
// Any scheme, credentials, path, query and port are dropped -- the local API has its own port -- and
// an IPv6 literal comes back in the brackets a URL needs. Surrounding whitespace is ignored; an
// address with no host gives an empty string. Never logs, never fails. The one parser both Flashforge and FlashforgePrinterAgent use,
// so the two cannot disagree about where the printer is.
std::string host_of(const std::string& address);

// "http://<host>:8898/<path>" for a host as host_of returns it.
std::string url_of(const std::string& host, const std::string& path);

// ── When a request fails ────────────────────────────────────────────────────────────────────────

// One failed request, as Http reported it.
struct RequestFailure
{
    // Http's text for a failure before any HTTP response, "curl:<summary>:\n<detail>\n[Error N]".
    // Empty when the printer answered, with an HTTP error status or an API error code.
    std::string error;
    unsigned    http_status{0};
    long        elapsed_ms{0};
    // The API's own refusal ("Flashforge local API error 1: ..."), when it answered with one.
    std::string api_error;
    // The user stopped the request (an upload's cancel button): not the printer's failure.
    bool        cancelled{false};
};

constexpr int kCurlCouldntResolveHost = 6;
constexpr int kCurlCouldntConnect     = 7;
constexpr int kCurlOperationTimedout  = 28;

// The curl code at the end of Http's error text; 0 when there is none (an HTTP error status, or text
// Http did not write).
int curl_code_of(const std::string& error);

// curl's own description of what failed: the middle line of Http's text. Empty when curl gave none,
// which curl 7.75 does exactly when connect() failed at once on this computer, before any packet
// reached the printer. When the printer itself refuses, it reads "Failed to connect to ...".
std::string curl_detail_of(const std::string& error);

// Whether a request that failed with `curl_code` on its `attempt`-th try (1-based) is made again:
// once, and only when the TCP connection was never made. Nothing reached the printer then, so
// repeating it cannot repeat a command, and this holds for every local-API request. Never when the
// caller may not wait (`may_wait` false: the GUI thread, which the send dialog reads slots from).
bool should_retry(int curl_code, int attempt, bool may_wait);
constexpr std::chrono::milliseconds kRetryDelay{500};

// One attempt: true on success, otherwise fills in how it failed.
using AttemptFn = std::function<bool(RequestFailure&)>;
using SleepFn   = std::function<void(std::chrono::milliseconds)>;

// Runs `attempt`, and again after kRetryDelay (slept through `sleep`) while should_retry says so.
// `last_failure` is the final attempt's failure; `attempts` is how many were made.
bool run_with_retry(const AttemptFn& attempt, const SleepFn& sleep, bool may_wait, RequestFailure& last_failure, int& attempts);

// What the agent or the user reads when a request never got an HTTP answer: the host and port, what
// happened, and what to do next. `failure.error` must carry a curl code.
std::string describe_failure(const std::string& host, const RequestFailure& failure, int attempts);

// The warning logged for a failed request: URL, curl code and detail, HTTP status, elapsed time,
// attempts. Never a body -- the request body holds the printer's check code.
std::string failure_log_line(const std::string& url, const RequestFailure& failure, int attempts);

// Whether the `consecutive`-th failure in a row for one host is logged: the first, then every 100th.
// A status poll against an unreachable printer would otherwise write a warning every five seconds.
bool should_log_failure(int consecutive);

// Consecutive failed requests per host, for should_log_failure and the "answers again" line.
// Thread-safe: the agent's poll, the Device page's poll and MCP calls all record into one.
class FailureStreaks
{
public:
    // The streak's length, this failure included.
    int record_failure(const std::string& host);
    // The length of the streak this success ended; 0 when there was none.
    int record_success(const std::string& host);

private:
    std::mutex                 m_mutex;
    std::map<std::string, int> m_streaks;
};

// The process-wide streaks every Flashforge host records into.
FailureStreaks& failure_streaks();

// ── The last status each printer answered with ─────────────────────────────────────────────────

// A printer's last good status, and how old it is.
struct CachedStatus
{
    FlashforgeApi::PrinterStatus status;
    long                         age_s{0};
};

// The last status each host answered with. Every successful Flashforge::fetch_status records into
// it (the agent's poll, the Device page's poll, MCP calls), so when a live read fails the caller can
// still say what the printer last reported and how long ago. Thread-safe.
class StatusCache
{
public:
    using Clock = std::chrono::steady_clock;

    void                        put(const std::string& host, const FlashforgeApi::PrinterStatus& status, Clock::time_point now = Clock::now());
    std::optional<CachedStatus> get(const std::string& host, Clock::time_point now = Clock::now()) const;

private:
    struct Entry
    {
        FlashforgeApi::PrinterStatus status;
        Clock::time_point            at;
    };
    mutable std::mutex           m_mutex;
    std::map<std::string, Entry> m_entries;
};

// The process-wide cache every Flashforge host records into.
StatusCache& status_cache();

}} // namespace Slic3r::FlashforgeLocalApi

#endif

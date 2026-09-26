#include "FlashforgeLocalApi.hpp"

#include "Http.hpp"

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/format.hpp>

#include <regex>
#include <vector>

namespace Slic3r { namespace FlashforgeLocalApi {

std::string host_of(const std::string& address)
{
    const std::string trimmed = boost::algorithm::trim_copy(address);
    if (trimmed.empty())
        return {};
    // Upstream's one URL parser: it adds the scheme a bare "ip:port" lacks before curl reads it,
    // which is exactly the case the old scheme-less branch got wrong.
    return Http::get_host_from_url(trimmed);
}

std::string url_of(const std::string& address, const std::string& path)
{
    return (boost::format("http://%1%:%2%/%3%") % host_of(address) % kPort % path).str();
}

namespace {

// Http's error text is "curl:<summary>:\n<detail>\n[Error N]"; these split it along those lines.
std::vector<std::string> error_lines(const std::string& error)
{
    std::vector<std::string> lines;
    boost::algorithm::split(lines, error, boost::algorithm::is_any_of("\n"));
    return lines;
}

// "Couldn't connect to server", out of the "curl:Couldn't connect to server:" first line.
std::string curl_summary_of(const std::string& error)
{
    std::string summary = error_lines(error).front();
    if (boost::algorithm::starts_with(summary, "curl:"))
        summary.erase(0, 5);
    if (boost::algorithm::ends_with(summary, ":"))
        summary.pop_back();
    return summary;
}

std::string tried_suffix(int attempts)
{
    return attempts > 1 ? "; tried " + std::to_string(attempts) + " times" : std::string();
}

std::string endpoint(const std::string& host)
{
    return host + ":" + std::to_string(kPort);
}

// Only macOS keeps a per-app Local Network permission that can block a LAN connection.
std::string local_network_hint()
{
#ifdef __APPLE__
    return " If it does and this is a newly built or installed app, check System Settings > Privacy & Security > "
           "Local Network.";
#else
    return {};
#endif
}

} // namespace

int curl_code_of(const std::string& error)
{
    static const std::regex code(R"(\[Error (\d+)\]\s*$)");
    std::smatch match;
    if (!std::regex_search(error, match, code))
        return 0;
    return std::stoi(match[1].str());
}

std::string curl_detail_of(const std::string& error)
{
    const std::vector<std::string> lines = error_lines(error);
    if (lines.size() < 3 || !boost::algorithm::starts_with(lines.front(), "curl:"))
        return {};
    return boost::algorithm::trim_copy(lines[1]);
}

bool should_retry(int curl_code, int attempt)
{
    return curl_code == kCurlCouldntConnect && attempt == 1;
}

bool run_with_retry(const AttemptFn& attempt, const SleepFn& sleep, RequestFailure& last_failure, int& attempts)
{
    for (attempts = 1;; ++attempts) {
        last_failure = {};
        if (attempt(last_failure))
            return true;
        if (!should_retry(curl_code_of(last_failure.error), attempts))
            return false;
        sleep(kRetryDelay);
    }
}

std::string describe_failure(const std::string& host, const RequestFailure& failure, int attempts)
{
    const int         code   = curl_code_of(failure.error);
    const std::string detail = curl_detail_of(failure.error);
    const std::string tried  = tried_suffix(attempts);

    if (code == kCurlCouldntConnect && detail.empty())
        return "Could not connect to the printer at " + endpoint(host) + ": the connection failed after " +
               std::to_string(failure.elapsed_ms) + " ms, before reaching the printer" + tried +
               ". That usually means the printer is not on the network right now: it is off or asleep, has "
               "lost its Wi-Fi, or has a new IP address. Wake it, check that the IP address on its screen "
               "matches the preset's print_host, and try again." + local_network_hint();

    if (code == kCurlCouldntConnect)
        return "The printer at " + endpoint(host) + " did not accept the connection (" + detail + tried +
               "). Its network API may be off, or " + host + " may now belong to another device: check that "
               "the IP address on the printer's screen matches the preset's print_host.";

    if (code == kCurlOperationTimedout)
        return "The printer at " + endpoint(host) + " did not answer within " +
               std::to_string((failure.elapsed_ms + 500) / 1000) + " s" + tried +
               ". Check that it is on and on the same network as this computer, and try again.";

    if (code == kCurlCouldntResolveHost)
        return "Could not find the printer's host name '" + host + "'" + tried +
               ". Use the printer's IP address as the preset's print_host (add_physical_printer).";

    return "The request to the printer at " + endpoint(host) + " failed: " + curl_summary_of(failure.error) +
           (detail.empty() ? std::string() : " (" + detail + ")") + ", curl error " + std::to_string(code) + tried +
           ".";
}

std::string failure_log_line(const std::string& url, const RequestFailure& failure, int attempts)
{
    const std::string detail = curl_detail_of(failure.error);
    std::string line = "[Flashforge HTTP] POST " + url + " failed after " + std::to_string(attempts) + " attempt" +
                       (attempts == 1 ? "" : "s") + ": curl " + std::to_string(curl_code_of(failure.error));
    if (!failure.error.empty())
        line += " (" + (detail.empty() ? std::string("no detail: connect() failed at once on this computer") : detail) + ")";
    line += ", HTTP " + std::to_string(failure.http_status) + ", " + std::to_string(failure.elapsed_ms) + " ms";
    if (!failure.api_error.empty())
        line += ", " + failure.api_error;
    return line;
}

bool should_log_failure(int consecutive)
{
    return consecutive == 1 || (consecutive > 0 && consecutive % 100 == 0);
}

int FailureStreaks::record_failure(const std::string& host)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return ++m_streaks[host];
}

int FailureStreaks::record_success(const std::string& host)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_streaks.find(host);
    if (it == m_streaks.end())
        return 0;
    const int ended = it->second;
    m_streaks.erase(it);
    return ended;
}

FailureStreaks& failure_streaks()
{
    static FailureStreaks streaks;
    return streaks;
}

}} // namespace Slic3r::FlashforgeLocalApi

#include <catch2/catch_all.hpp>

#include "libslic3r/Thread.hpp"

#include <tbb/global_control.h>
#include <tbb/info.h>
#include <tbb/parallel_for.h>
#include <tbb/partitioner.h>
#include <tbb/task_arena.h>

#include <chrono>
#include <clocale>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r;
using namespace std::chrono_literals;

namespace {

// A condition deadline, never a speed assertion: what the tests wait for takes microseconds.
constexpr auto deadline = 30s;

// Runs fn on a new thread, which starts with no locale or TBB state of its own, and returns its result.
template<class Fn> auto on_new_thread(Fn fn)
{
    std::optional<decltype(fn())> result;
    std::thread([&] { result.emplace(fn()); }).join();
    return std::move(*result);
}

// How the current thread prints and parses one half.
std::string format_half()
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f", 0.5);
    return buf;
}

double parse_half() { return std::strtod("0.5", nullptr); }

// Sets the process's numeric locale to one with a decimal comma, and restores it. setlocale runs on
// a new thread: on Windows a thread in per-thread locale mode (CNumericLocalesSetter puts one there)
// would change only its own locale.
class ScopedCommaDecimalLocale
{
public:
    ScopedCommaDecimalLocale()
        : m_previous(on_new_thread([] { return std::string(std::setlocale(LC_NUMERIC, nullptr)); }))
        , m_active(on_new_thread(set_comma_locale))
    {}
    ~ScopedCommaDecimalLocale()
    {
        on_new_thread([this] { return std::setlocale(LC_NUMERIC, m_previous.c_str()) != nullptr; });
    }
    bool active() const { return m_active; }

private:
    static bool set_comma_locale()
    {
        for (const char *name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR.utf8", "nl_NL.UTF-8",
                                 "ru_RU.UTF-8", "de-DE", "German_Germany.1252"})
            if (std::setlocale(LC_NUMERIC, name) != nullptr && std::localeconv()->decimal_point[0] == ',')
                return true;
        return false;
    }

    std::string m_previous;
    bool        m_active;
};

struct WorkerReport
{
    std::string                half;        // 0.5 as the worker prints it
    double                     parsed_half; // "0.5" as the worker parses it
    std::optional<std::string> name;        // the worker's thread name, where the platform can read it
};

// Runs tasks in the calling thread's current arena until `wanted` TBB workers have each run one, or
// the deadline passes, and returns what each worker reported. Every task waits for the others, so
// the threads that came first stay busy and TBB has to bring more workers in.
std::vector<WorkerReport> report_from_workers(size_t wanted)
{
    const std::thread::id     caller = std::this_thread::get_id();
    const auto                until  = std::chrono::steady_clock::now() + deadline;
    std::mutex                mutex;
    std::condition_variable   cv;
    std::set<std::thread::id> seen;
    std::vector<WorkerReport> reports;
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, size_t(tbb::this_task_arena::max_concurrency()), 1),
        [&](const tbb::blocked_range<size_t> &) {
            std::unique_lock<std::mutex> lock(mutex);
            if (std::this_thread::get_id() != caller && seen.insert(std::this_thread::get_id()).second) {
                reports.push_back({format_half(), parse_half(), get_current_thread_name()});
                cv.notify_all();
            }
            cv.wait_until(lock, until, [&] { return reports.size() >= wanted; });
        },
        tbb::simple_partitioner());
    return reports;
}

// Every worker prints and parses 0.5 with a decimal point, and carries a slic3r_tbb_<n> name of its
// own where the platform can read names back.
void check_prepared(const std::vector<WorkerReport> &reports)
{
    std::set<std::string> names;
    for (const WorkerReport &report : reports) {
        CHECK(report.half == "0.5");
        CHECK_THAT(report.parsed_half, Catch::Matchers::WithinAbs(0.5, 1e-12));
        if (report.name) {
            CHECK_THAT(*report.name, Catch::Matchers::StartsWith("slic3r_tbb_"));
            CHECK(names.insert(*report.name).second);
        }
    }
}

// Holds every TBB worker that takes one of its tasks, on a thread and arena of its own, until released.
class WorkerOccupier
{
public:
    WorkerOccupier() : m_thread([this] { occupy(); }) {}
    ~WorkerOccupier()
    {
        release();
        m_thread.join();
    }

    bool wait_until_holding_a_worker()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, deadline, [this] { return m_workers_held > 0; });
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_released = true;
        }
        m_cv.notify_all();
    }

private:
    void occupy()
    {
        const std::thread::id owner = std::this_thread::get_id();
        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, size_t(tbb::this_task_arena::max_concurrency()), 1),
            [this, owner](const tbb::blocked_range<size_t> &) {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (std::this_thread::get_id() != owner) {
                    ++m_workers_held;
                    m_cv.notify_all();
                }
                m_cv.wait(lock, [this] { return m_released; });
            },
            tbb::simple_partitioner());
    }

    std::mutex              m_mutex;
    std::condition_variable m_cv;
    size_t                  m_workers_held = 0;
    bool                    m_released     = false;
    std::thread             m_thread; // last: it starts using the members above at once
};

} // namespace

TEST_CASE("Preparing the TBB workers returns while they are busy with other work", "[Thread][Regression]")
{
    if (tbb::info::default_concurrency() < 2)
        SKIP("TBB starts no worker thread on a single-CPU machine");

    WorkerOccupier occupier;
    REQUIRE(occupier.wait_until_holding_a_worker());

    // The call runs on a thread of its own, as the slicing thread's does. Its state is shared, so the
    // thread can be left behind if the call never returns.
    struct CallState
    {
        std::mutex              mutex;
        std::condition_variable cv;
        bool                    returned = false;
    };
    auto        state  = std::make_shared<CallState>();
    std::thread caller([state] {
        name_tbb_thread_pool_threads_set_locale();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->returned = true;
        }
        state->cv.notify_all();
    });
    const auto returns_within = [&state](std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> lock(state->mutex);
        return state->cv.wait_for(lock, timeout, [&state] { return state->returned; });
    };
    const bool returned_while_busy = returns_within(deadline);

    // Free the workers, so that a call waiting for them returns and the test ends instead of hanging.
    occupier.release();
    if (returns_within(2 * deadline))
        caller.join();
    else
        caller.detach();
    CHECK(returned_while_busy);
}

TEST_CASE("A TBB worker prints and parses a decimal point while the process locale uses a comma", "[Thread]")
{
    // CLI::run prepares the main thread's arena, and the slicing thread its own. A worker stays prepared
    // for good, so a case checks its arena only while no earlier test in the process prepared the
    // workers: ctest runs every test case in a process of its own, and the main thread's case first.
    const bool on_main_thread = GENERATE(true, false);
    CAPTURE(on_main_thread);
    if (tbb::info::default_concurrency() < 2)
        SKIP("TBB starts no worker thread on a single-CPU machine");
    ScopedCommaDecimalLocale comma;
    if (!comma.active())
        SKIP("No locale with a decimal comma is installed");
    // A thread nobody prepared follows the process locale.
    REQUIRE(on_new_thread(format_half) == "0,5");

    const auto prepare_and_report = [] {
        name_tbb_thread_pool_threads_set_locale();
        return report_from_workers(1);
    };
    const std::vector<WorkerReport> reports = on_main_thread ? prepare_and_report() : on_new_thread(prepare_and_report);

    REQUIRE_FALSE(reports.empty());
    check_prepared(reports);
}

TEST_CASE("A TBB worker started after the preparation prints a decimal point too", "[Thread]")
{
    ScopedCommaDecimalLocale comma;
    if (!comma.active())
        SKIP("No locale with a decimal comma is installed");
    REQUIRE(on_new_thread(format_half) == "0,5");

    // Until the limit is raised, TBB runs at most default_concurrency() - 1 workers. An arena wider
    // than that, under a raised limit, has it start new ones.
    const size_t workers_before = size_t(tbb::info::default_concurrency() - 1);
    const int    wide           = tbb::info::default_concurrency() + 8;
    const std::vector<WorkerReport> reports = on_new_thread([wide, workers_before] {
        tbb::task_arena wider(wide);
        return wider.execute([&] {
            // The preparation observes the arena its caller is in.
            name_tbb_thread_pool_threads_set_locale();
            tbb::global_control more_workers(tbb::global_control::max_allowed_parallelism, size_t(wide));
            return report_from_workers(workers_before + 1);
        });
    });

    // More workers than TBB ran before the call: at least one of them started after it.
    REQUIRE(reports.size() > workers_before);
    check_prepared(reports);
}

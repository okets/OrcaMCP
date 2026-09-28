#include "OrcaMCPInstanceRegistry.hpp"
#include "MCPClientConfig.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/nowide/convert.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif
#ifdef __APPLE__
#include <libproc.h>
#endif

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

constexpr int         k_schema      = 1;
constexpr const char* k_entry_ext   = ".json";

std::string string_field(const json& entry, const char* key)
{
    const auto it = entry.find(key);
    return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool is_positive_integer(const json& entry, const char* key)
{
    const auto it = entry.find(key);
    return it != entry.end() && it->is_number_integer() && it->get<long long>() > 0;
}

// "pid 4242 on port 13619 (project "bracket")"
std::string describe(const InstanceIdentity& identity)
{
    return "pid " + std::to_string(identity.pid) + " on port " + std::to_string(identity.port) + " (project \"" +
           identity.project.name + "\")";
}

std::optional<std::string> wanted_instance(const json& params)
{
    const auto meta = params.find("_meta");
    if (meta == params.end() || !meta->is_object())
        return std::nullopt;
    const auto wanted = meta->find(instance_meta_key);
    if (wanted == meta->end() || !wanted->is_string())
        return std::nullopt;
    return wanted->get<std::string>();
}

std::mutex&            refresher_mutex() { static std::mutex mutex; return mutex; }
std::function<void()>& refresher() { static std::function<void()> refresh; return refresh; }

} // namespace

std::string mcp_url(Port port) { return "http://127.0.0.1:" + std::to_string(port) + "/mcp"; }

json to_json(const InstanceIdentity& identity)
{
    return json{
        {"schema", k_schema},
        {"instance_id", identity.instance_id},
        {"pid", identity.pid},
        {"port", identity.port},
        {"url", mcp_url(identity.port)},
        {"version", identity.version},
        {"executable", identity.executable},
        {"data_dir", identity.data_dir},
        {"started_at", identity.started_at},
        {"alone_at_start", identity.alone_at_start},
        {"project", {{"name", identity.project.name}, {"path", identity.project.path}, {"unsaved", identity.project.unsaved}}},
    };
}

std::optional<InstanceIdentity> identity_from_json(const json& entry)
{
    if (!entry.is_object())
        return std::nullopt;
    const auto pid  = entry.find("pid");
    const auto port = entry.find("port");
    if (string_field(entry, "instance_id").empty() || !is_positive_integer(entry, "pid") || !is_positive_integer(entry, "port"))
        return std::nullopt;

    InstanceIdentity identity;
    identity.instance_id = string_field(entry, "instance_id");
    identity.pid         = pid->get<unsigned>();
    identity.port        = static_cast<Port>(port->get<unsigned>());
    identity.version     = string_field(entry, "version");
    identity.executable  = string_field(entry, "executable");
    identity.data_dir    = string_field(entry, "data_dir");
    identity.started_at  = string_field(entry, "started_at");
    identity.alone_at_start = entry.value("alone_at_start", false);
    if (const auto project = entry.find("project"); project != entry.end() && project->is_object()) {
        identity.project.name    = string_field(*project, "name");
        identity.project.path    = string_field(*project, "path");
        identity.project.unsaved = project->value("unsaved", false);
    }
    return identity;
}

std::string new_instance_id()
{
    std::random_device                      device;
    std::uniform_int_distribution<unsigned> byte(0, 255);
    std::ostringstream                      out;
    for (int i = 0; i < 16; ++i)
        out << std::hex << std::setw(2) << std::setfill('0') << byte(device);
    return out.str();
}

std::string utc_timestamp(std::chrono::system_clock::time_point when)
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
    const auto        millis  = std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count() % 1000;
    std::tm           utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char text[32];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>(millis));
    return text;
}

fs::path default_instances_dir()
{
    if (const char* dir = std::getenv("ORCAMCP_INSTANCES_DIR"); dir != nullptr && *dir != '\0')
        return fs::path(dir);
    const std::string shared = MCPClientConfig::get_shared_scripts_dir();
    return shared.empty() ? fs::path() : fs::path(shared) / "instances";
}

bool process_is_alive(unsigned pid)
{
    if (pid == 0)
        return false;
#ifdef _WIN32
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
        return GetLastError() == ERROR_ACCESS_DENIED; // it runs, as another user
    DWORD      code  = 0;
    const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

std::string process_executable(unsigned pid)
{
    if (pid == 0)
        return {};
#ifdef _WIN32
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
        return {};
    wchar_t path[MAX_PATH * 4];
    DWORD   size = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    const bool read = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    return read ? boost::nowide::narrow(std::wstring(path, size)) : std::string();
#elif defined(__APPLE__)
    char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
    return proc_pidpath(static_cast<int>(pid), path, sizeof(path)) > 0 ? std::string(path) : std::string();
#else
    boost::system::error_code ec;
    const fs::path            path = fs::read_symlink("/proc/" + std::to_string(pid) + "/exe", ec);
    return ec ? std::string() : path.string();
#endif
}

bool instance_process_runs(const InstanceIdentity& entry)
{
    if (!process_is_alive(entry.pid))
        return false;
    const std::string running = process_executable(entry.pid);
    if (running.empty() || entry.executable.empty())
        return true; // cannot tell: never remove a live instance's entry for it
    const auto name = [](const std::string& path) { return fs::path(path).filename().string(); };
#ifdef _WIN32
    return boost::iequals(name(running), name(entry.executable));
#else
    return name(running) == name(entry.executable);
#endif
}

InstanceRegistry::InstanceRegistry(fs::path dir) : m_dir(std::move(dir)) {}

fs::path InstanceRegistry::entry_path(unsigned pid) const { return m_dir / (std::to_string(pid) + k_entry_ext); }

std::string InstanceRegistry::publish(const InstanceIdentity& identity, const IsAlive& is_alive)
{
    InstanceIdentity published = identity;
    if (!m_dir.empty()) {
        remove_stale_entries(identity, is_alive);
        const std::vector<InstanceIdentity> entries = read_entries();
        published.alone_at_start = std::none_of(entries.begin(), entries.end(), [&](const InstanceIdentity& entry) {
            return entry.pid != identity.pid && entry.executable == identity.executable && entry.data_dir == identity.data_dir &&
                   is_alive(entry);
        });
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_identity        = published;
        m_written_project = std::nullopt;
        m_withdrawn       = false;
    }
    if (m_dir.empty())
        return "no home folder to keep the instance entry in";
    boost::system::error_code ec;
    fs::create_directories(m_dir, ec);
    fs::permissions(m_dir, fs::owner_all, ec); // the user's alone
    return write_project(published);
}

bool InstanceRegistry::update_project(const ProjectInfo& project)
{
    InstanceIdentity current;
    bool             changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_identity || m_withdrawn)
            return false;
        changed             = m_identity->project != project;
        m_identity->project = project;
        if (m_written_project == project)
            return changed;
        current = *m_identity;
    }
    if (!m_dir.empty())
        if (const std::string failure = write_project(current); !failure.empty())
            BOOST_LOG_TRIVIAL(warning) << "OrcaMCP instance registry: " << failure << "; it is tried again in a second";
    return changed;
}

void InstanceRegistry::withdraw()
{
    unsigned pid = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_identity || m_withdrawn)
            return;
        m_withdrawn = true;
        pid         = m_identity->pid;
    }
    if (m_dir.empty())
        return;
    boost::system::error_code ec;
    fs::remove(entry_path(pid), ec);
}

// Writes the entry, and notes the project it holds once it does. Main thread only, as publish,
// update_project and withdraw are, so no write lands after a withdraw.
std::string InstanceRegistry::write_project(const InstanceIdentity& identity)
{
    const std::string failure = write_entry(identity);
    if (failure.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_written_project = identity.project;
    }
    return failure;
}

std::optional<InstanceIdentity> InstanceRegistry::identity() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_identity;
}

std::vector<InstanceIdentity> InstanceRegistry::others(const IsAlive& is_alive) const
{
    const std::optional<InstanceIdentity> self = identity();
    std::vector<InstanceIdentity>         out;
    for (InstanceIdentity& entry : read_entries())
        if ((!self || entry.instance_id != self->instance_id) && is_alive(entry))
            out.push_back(std::move(entry));
    return out;
}

std::vector<InstanceIdentity> InstanceRegistry::read_entries() const
{
    std::vector<InstanceIdentity> entries;
    boost::system::error_code     ec;
    if (m_dir.empty() || !fs::is_directory(m_dir, ec))
        return entries;
    for (fs::directory_iterator it(m_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != k_entry_ext)
            continue;
        try {
            boost::nowide::ifstream in(it->path().string());
            if (auto identity = identity_from_json(json::parse(in)))
                entries.push_back(std::move(*identity));
        } catch (const std::exception&) {
            // Unreadable, or half of a file another version wrote: not an entry.
        }
    }
    return entries;
}

// A crashed instance's entry names a process that is gone, or whose pid now runs another program.
// Another entry that claims this port is as stale: this instance listens on it, and made sure nothing
// else did first.
void InstanceRegistry::remove_stale_entries(const InstanceIdentity& identity, const IsAlive& is_alive) const
{
    for (const InstanceIdentity& entry : read_entries()) {
        if (entry.pid == identity.pid || (is_alive(entry) && entry.port != identity.port))
            continue;
        boost::system::error_code ec;
        fs::remove(entry_path(entry.pid), ec);
        BOOST_LOG_TRIVIAL(info) << "OrcaMCP instance registry: removed the stale entry of pid " << entry.pid << " (port "
                                << entry.port << ")";
    }
}

// Under a temporary name, then renamed over the entry, so a reader never sees half of one. The folder
// and the file are the user's alone.
std::string InstanceRegistry::write_entry(const InstanceIdentity& identity) const
{
    boost::system::error_code ec;
    const fs::path target    = entry_path(identity.pid);
    const fs::path temporary = target.string() + ".tmp";
    {
        boost::nowide::ofstream out(temporary.string(), std::ios::binary | std::ios::trunc);
        out << to_json(identity).dump(2);
        if (!out)
            return "cannot write " + temporary.string();
    }
    fs::permissions(temporary, fs::owner_read | fs::owner_write, ec);
    fs::rename(temporary, target, ec);
    if (ec) {
        boost::system::error_code ignored;
        fs::remove(temporary, ignored);
        return "cannot write " + target.string() + ": " + ec.message();
    }
    return {};
}

InstanceRegistry& instance_registry()
{
    static InstanceRegistry registry(default_instances_dir());
    return registry;
}

std::optional<std::string> wrong_instance_refusal(const json& params, const std::optional<InstanceIdentity>& self)
{
    const std::optional<std::string> wanted = wanted_instance(params);
    if (!wanted || (self && *wanted == self->instance_id))
        return std::nullopt;
    return "This call was meant for another OrcaMCP instance (" + *wanted + ") but reached " +
           (self ? describe(*self) : std::string("an OrcaMCP instance that has not published itself")) +
           ", so it was not run. Call list_instances to see which instances run, and select_instance to choose one.";
}

std::optional<std::string> other_port_note(Port port, const std::vector<InstanceIdentity>& others)
{
    if (port == first_mcp_port)
        return std::nullopt;
    const auto holder = std::find_if(others.begin(), others.end(),
                                     [](const InstanceIdentity& other) { return other.port == first_mcp_port; });
    const std::string why = holder != others.end() ? "port " + std::to_string(first_mcp_port) + " is used by another OrcaMCP window, " +
                                                         describe(*holder) :
                                                     "port " + std::to_string(first_mcp_port) + " is in use by another program";
    return "Agents reach this OrcaMCP window on port " + std::to_string(port) + ": " + why + ".";
}

std::string no_port_warning()
{
    return "Agents cannot reach this OrcaMCP window: ports " + std::to_string(first_mcp_port) + " to " +
           std::to_string(first_mcp_port + mcp_port_count - 1) +
           " are all in use. Close another OrcaMCP window and restart this one. Cloud sign-in cannot finish in "
           "this window either.";
}

std::optional<std::string> shared_data_folder_warning(const std::string& data_dir, const std::vector<InstanceIdentity>& others)
{
    std::string pids;
    for (const InstanceIdentity& other : others)
        if (!data_dir.empty() && other.data_dir == data_dir)
            pids += (pids.empty() ? "pid " : ", pid ") + std::to_string(other.pid);
    if (pids.empty())
        return std::nullopt;
    return "Another OrcaMCP window (" + pids + ") uses this data folder: the settings and presets either one saves can "
           "overwrite the other's.";
}

void set_project_refresher(std::function<void()> refresh)
{
    std::lock_guard<std::mutex> lock(refresher_mutex());
    refresher() = std::move(refresh);
}

void request_project_refresh()
{
    std::function<void()> refresh;
    {
        std::lock_guard<std::mutex> lock(refresher_mutex());
        refresh = refresher();
    }
    if (refresh)
        refresh();
}

}}} // namespace Slic3r::GUI::OrcaMCP

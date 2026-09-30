// src/slic3r/GUI/OrcaMCP/OrcaMCPInstanceRegistry.hpp
#pragma once
#include "OrcaMCPPortChoice.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

// Who each running OrcaMCP instance is, so an agent's bridge can list them and choose one.
//
// Several instances can run at once, each with its MCP server on its own port (OrcaMCPPortChoice.hpp).
// Each one publishes an entry, <pid>.json, in ~/.orcamcp/instances (ORCAMCP_INSTANCES_DIR moves it, for
// tests): one folder per user, whatever the build or the data folder, next to the bridge script. The
// folder and the files are the user's alone (0700 / 0600), and a file is written under a temporary
// name and renamed, so a reader never sees half of one. An entry holds the instance's identity and
// its open project (name, file, unsaved changes), never a credential; its instance_id is an identity,
// not a secret: GET /mcp tells it to any local client.
//
// A crashed instance leaves its entry behind. The next instance to publish removes entries whose
// process is gone, and any other entry claiming its own port; a reader trusts an entry only when its
// port answers GET /mcp with the entry's instance_id.
//
// Every call the bridge forwards names the instance it chose, in params._meta["orcamcp/instance"]; a
// call that reaches any other instance is refused with JSON-RPC -32004 before anything runs
// (wrong_instance_refusal), so no call lands in a window the agent did not choose.
//
// Unit-tested in tests/slic3rutils/test_instance_registry.cpp. The app side -- reading the project
// from the Plater, the notifications -- is OrcaMCPInstanceRegistryApp.cpp.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

struct ProjectInfo
{
    std::string name;          // as the window title shows it: "Untitled" for a new project
    std::string path;          // its file; empty until it is saved, or opened from one
    bool        unsaved = false;

    bool operator==(const ProjectInfo& other) const
    {
        return name == other.name && path == other.path && unsaved == other.unsaved;
    }
    bool operator!=(const ProjectInfo& other) const { return !(*this == other); }
};

struct InstanceIdentity
{
    std::string instance_id;   // random, new at every launch
    unsigned    pid  = 0;
    Port        port = 0;
    std::string version;
    std::string executable;
    std::string data_dir;
    std::string started_at;    // UTC, ISO 8601 with milliseconds: the bridge orders launches by it
    // No other instance of this program ran on this data folder when it started: only then can it be a
    // restart of one that quit, which the bridge follows. A second window opened beside it is not.
    bool        alone_at_start = false;
    // The token the bridge's start_orca gave this launch (ORCAMCP_LAUNCH_ID), by which it knows the
    // instance it launched whatever wrapper launched it; empty when no agent launched it.
    std::string launch_id;
    ProjectInfo project;
};

// Where an instance's MCP server answers: always 127.0.0.1, the one address it listens on.
std::string mcp_url(Port port);

nlohmann::json                  to_json(const InstanceIdentity& identity);
std::optional<InstanceIdentity> identity_from_json(const nlohmann::json& entry);

// 32 random hex digits.
std::string new_instance_id();
// The launch token in ORCAMCP_LAUNCH_ID, cleared from the environment so that a window this instance
// opens never carries it too; empty when it is not set.
std::string take_launch_id();
// e.g. 2026-09-28T09:15:03.123Z
std::string utc_timestamp(std::chrono::system_clock::time_point when);

// ORCAMCP_INSTANCES_DIR, else ~/.orcamcp/instances (%USERPROFILE%\.orcamcp\instances on Windows).
boost::filesystem::path default_instances_dir();

// Whether a process with this pid is running.
bool process_is_alive(unsigned pid);
// The program a running process runs, as a full path; empty when it cannot be read.
std::string process_executable(unsigned pid);
// Whether the instance an entry names still runs: its pid is alive and runs the entry's program (a
// crashed instance's pid can be reused by anything). A program that cannot be read counts as running.
bool instance_process_runs(const InstanceIdentity& entry);

class InstanceRegistry
{
public:
    using IsAlive = std::function<bool(const InstanceIdentity& entry)>;

    explicit InstanceRegistry(boost::filesystem::path dir);

    // Writes this instance's entry, after removing the entries of processes that are gone and any
    // other entry claiming its port, and notes whether another of its program ran on its data folder
    // (alone_at_start). The identity is kept even when the file cannot be written (GET /mcp and the call
    // check still use it), and the next update_project writes it. Empty on success, otherwise why not.
    std::string publish(const InstanceIdentity& identity, const IsAlive& is_alive = instance_process_runs);

    // The project as it is now. True when it changed. The entry is rewritten whenever it does not hold
    // it yet: a write that failed (Windows refuses the rename while the bridge reads the file) is tried
    // again on the next call, which comes once a second.
    bool update_project(const ProjectInfo& project);

    // Removes the entry: the instance is quitting, and no agent should choose it now. Its identity is
    // kept, since it answers calls until it is gone: one stamped for it gets "quitting" (-32002), not
    // "meant for another instance" (-32004). The entry is never written again.
    void withdraw();

    // This instance, once published. Safe from any thread: the HTTP thread reads it.
    std::optional<InstanceIdentity> identity() const;

    // The other entries in the folder whose process runs, e.g. to name who holds a port.
    std::vector<InstanceIdentity> others(const IsAlive& is_alive = instance_process_runs) const;

    boost::filesystem::path entry_path(unsigned pid) const;

private:
    std::vector<InstanceIdentity> read_entries() const;
    void                          remove_stale_entries(const InstanceIdentity& identity, const IsAlive& is_alive) const;
    std::string                   write_entry(const InstanceIdentity& identity) const;
    std::string                   write_project(const InstanceIdentity& identity);

    boost::filesystem::path         m_dir;
    mutable std::mutex              m_mutex;
    std::optional<InstanceIdentity> m_identity;
    std::optional<ProjectInfo>      m_written_project; // what the entry on disk holds, once written
    bool                            m_withdrawn = false;
};

// The app's registry, in default_instances_dir().
InstanceRegistry& instance_registry();

// Where the bridge names the instance a call is meant for, in params._meta.
constexpr const char* instance_meta_key = "orcamcp/instance";

// Why a tools/call must not run here: it names another instance than `self` in
// params._meta["orcamcp/instance"]. Nothing when it names none (an older bridge, curl) or this one.
std::optional<std::string> wrong_instance_refusal(const nlohmann::json& params, const std::optional<InstanceIdentity>& self);

// What the user is told about this instance (OrcaMCPInstanceRegistryApp.cpp shows it as a
// notification). Nothing for an instance on the first port.
std::optional<std::string> other_port_note(Port port, const std::vector<InstanceIdentity>& others);
// No port was free: the window runs without MCP, and without the cloud sign-in's callback, which the
// MCP server carries (OrcaMCPLoginServer.hpp).
std::string no_port_warning();
// Other instances on the same data folder: the settings and presets either one saves can overwrite
// the other's (upstream behaviour, kept). Nothing when there are none.
std::optional<std::string> shared_data_folder_warning(const std::string& data_dir, const std::vector<InstanceIdentity>& others);

// The app side, in OrcaMCPInstanceRegistryApp.cpp. After GUI_App has chosen the MCP server's port:
// logs the choice, publishes this instance and keeps its project current, or, with no port, warns.
void announce_mcp_server(const PortChoice& choice);
// Stops keeping the project current: the app is quitting.
void stop_project_watch();

// The project fields are refreshed after every tool call, so the bridge reads the agent's own changes
// at once. The app installs how (OrcaMCPInstanceRegistryApp.cpp); until it does, a request does nothing.
void set_project_refresher(std::function<void()> refresh);
void request_project_refresh();

}}} // namespace Slic3r::GUI::OrcaMCP

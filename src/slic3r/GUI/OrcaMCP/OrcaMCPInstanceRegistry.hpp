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
    ProjectInfo project;
};

// Where an instance's MCP server answers: always 127.0.0.1, the one address it listens on.
std::string mcp_url(Port port);

nlohmann::json                  to_json(const InstanceIdentity& identity);
std::optional<InstanceIdentity> identity_from_json(const nlohmann::json& entry);

// 32 random hex digits.
std::string new_instance_id();
// e.g. 2026-09-28T09:15:03.123Z
std::string utc_timestamp(std::chrono::system_clock::time_point when);

// ORCAMCP_INSTANCES_DIR, else ~/.orcamcp/instances (%USERPROFILE%\.orcamcp\instances on Windows).
boost::filesystem::path default_instances_dir();

// Whether a process with this pid is running.
bool process_is_alive(unsigned pid);

class InstanceRegistry
{
public:
    using IsAlive = std::function<bool(unsigned pid)>;

    explicit InstanceRegistry(boost::filesystem::path dir);

    // Writes this instance's entry, after removing the entries of processes that are gone and any
    // other entry claiming its port. The identity is kept even when the file cannot be written (GET /mcp
    // and the call check still use it). Empty on success, otherwise why the file was not written.
    std::string publish(const InstanceIdentity& identity, const IsAlive& is_alive = process_is_alive);

    // Rewrites the entry when `project` differs from the one it holds. True when it changed.
    bool update_project(const ProjectInfo& project);

    // Removes the entry and forgets the identity: the instance is quitting.
    void withdraw();

    // This instance, once published. Safe from any thread: the HTTP thread reads it.
    std::optional<InstanceIdentity> identity() const;

    // The other entries in the folder whose process runs, e.g. to name who holds a port.
    std::vector<InstanceIdentity> others(const IsAlive& is_alive = process_is_alive) const;

    boost::filesystem::path entry_path(unsigned pid) const;

private:
    std::vector<InstanceIdentity> read_entries() const;
    void                          remove_stale_entries(const InstanceIdentity& identity, const IsAlive& is_alive) const;
    std::string                   write_entry(const InstanceIdentity& identity) const;

    boost::filesystem::path         m_dir;
    mutable std::mutex              m_mutex;
    std::optional<InstanceIdentity> m_identity;
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

// src/slic3r/GUI/OrcaMCP/OrcaMCPInstanceRegistryApp.cpp
// The app's side of OrcaMCPInstanceRegistry.hpp: who this instance is, its open project as the Plater
// holds it, and what the user is told about its MCP server's port.
#include "OrcaMCPInstanceRegistry.hpp"
#include "OrcaMCPServer.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/stdpaths.h>
#include <wx/timer.h>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

// The window title's project: its name, its file, and the "*" of unsaved changes.
ProjectInfo current_project()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return {};
    return {into_u8(plater->get_project_name()), into_u8(plater->get_project_filename(".3mf")), plater->is_project_dirty()};
}

// On the main thread: the entry follows the project, rewritten only when it changed.
void refresh_project()
{
    if (!instance_registry().identity() || wxGetApp().is_closing())
        return;
    instance_registry().update_project(current_project());
}

// Once a second, for what the user does in the window; every tool call also refreshes it at once
// (request_project_refresh). Never destroyed, like the quit's turn timer: it can fire while the app
// object is torn down, when refresh_project does nothing.
class ProjectWatch : public wxTimer
{
    void Notify() override { refresh_project(); }
};

ProjectWatch& project_watch()
{
    static ProjectWatch* const watch = new ProjectWatch;
    return *watch;
}

constexpr int k_project_watch_ms = 1000;

// Paths as both instances and the bridge compare them.
std::string canonical(const std::string& path)
{
    boost::system::error_code ec;
    const boost::filesystem::path resolved = boost::filesystem::weakly_canonical(boost::filesystem::path(path), ec);
    return ec ? path : resolved.string();
}

InstanceIdentity this_instance(Port port)
{
    InstanceIdentity identity;
    identity.instance_id = new_instance_id();
    identity.pid         = get_current_pid();
    identity.port        = port;
    identity.version     = OrcaMCPServer::version();
    identity.executable  = canonical(into_u8(wxStandardPaths::Get().GetExecutablePath()));
    identity.data_dir    = canonical(data_dir());
    identity.started_at  = utc_timestamp(std::chrono::system_clock::now());
    identity.project     = current_project();
    return identity;
}

void notify(NotificationManager::NotificationLevel level, const std::string& text)
{
    if (Plater* plater = wxGetApp().plater())
        plater->get_notification_manager()->push_notification(NotificationType::CustomNotification, level, text);
}

void publish_this_instance(Port port)
{
    const InstanceIdentity identity = this_instance(port);
    if (const std::string failure = instance_registry().publish(identity); !failure.empty())
        BOOST_LOG_TRIVIAL(warning) << "OrcaMCP instance registry: " << failure << "; list_instances will not show this instance";
    else
        BOOST_LOG_TRIVIAL(info) << "OrcaMCP instance registry: published pid " << identity.pid << " on port " << port << " in "
                                << instance_registry().entry_path(identity.pid).string();

    set_project_refresher([] { wxGetApp().CallAfter([] { refresh_project(); }); });
    project_watch().Start(k_project_watch_ms);

    // Neither is in MCP's active_warnings: they are the window's state, which no tool can clear.
    const std::vector<InstanceIdentity> others = instance_registry().others();
    if (const auto note = other_port_note(port, others))
        notify(NotificationManager::NotificationLevel::RegularNotificationLevel, *note);
    if (const auto warning = shared_data_folder_warning(identity.data_dir, others))
        notify(NotificationManager::NotificationLevel::ImportantNotificationLevel, *warning);
}

} // namespace

void announce_mcp_server(const PortChoice& choice)
{
    if (choice.port) {
        BOOST_LOG_TRIVIAL(info) << "OrcaMCP: " << describe_port_choice(choice);
        publish_this_instance(*choice.port);
        return;
    }
    BOOST_LOG_TRIVIAL(warning) << "OrcaMCP: " << describe_port_choice(choice);
    // A cloud sign-in asks for the server again on every message it sends: say it once.
    static bool warned = false;
    if (!warned) {
        warned = true;
        notify(NotificationManager::NotificationLevel::WarningNotificationLevel, no_port_warning());
    }
}

void stop_project_watch()
{
    project_watch().Stop();
    set_project_refresher(nullptr);
}

}}} // namespace Slic3r::GUI::OrcaMCP

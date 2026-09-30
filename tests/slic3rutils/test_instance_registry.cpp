#include "slic3r/GUI/OrcaMCP/OrcaMCPInstanceRegistry.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPServer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <regex>
#include <sstream>
#include <set>
#include <string>

#include "test_utils.hpp"

// Who each running OrcaMCP instance is (OrcaMCPInstanceRegistry.hpp): the entry it publishes for the
// bridge's list_instances, how stale entries go, and the check that refuses a call meant for another
// instance. Every test keeps its entries in a folder of its own, never in ~/.orcamcp.

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;
namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

InstanceIdentity identity(unsigned pid, Port port, const std::string& project = "bracket")
{
    InstanceIdentity identity;
    identity.instance_id = "id-" + std::to_string(pid);
    identity.pid         = pid;
    identity.port        = port;
    identity.version     = "2.5.0.6-dev";
    identity.executable  = "/Applications/OrcaMCP.app/Contents/MacOS/OrcaSlicer";
    identity.data_dir    = "/data/OrcaMCP";
    identity.started_at  = "2026-09-28T09:15:03.123Z";
    identity.project     = {project, "/prints/" + project + ".3mf", false};
    return identity;
}

json read_entry(const fs::path& path)
{
    boost::nowide::ifstream in(path.string());
    return json::parse(in);
}

void write_entry(const fs::path& dir, const InstanceIdentity& entry)
{
    boost::nowide::ofstream out((dir / (std::to_string(entry.pid) + ".json")).string());
    out << to_json(entry).dump();
}

// The entries whose pid is in `alive` still run; any other is gone.
InstanceRegistry::IsAlive running(std::set<unsigned> alive)
{
    return [alive](const InstanceIdentity& entry) { return alive.count(entry.pid) != 0; };
}

// The JSON a response carries, after its headers.
json reply_body(Slic3r::GUI::HttpServer::Response& response)
{
    std::stringstream written;
    response.write_response(written);
    const std::string text = written.str();
    return json::parse(text.substr(text.find("\n\n") + 2));
}

std::set<std::string> entry_files(const fs::path& dir)
{
    std::set<std::string> names;
    for (fs::directory_iterator it(dir), end; it != end; ++it)
        names.insert(it->path().filename().string());
    return names;
}

} // namespace

TEST_CASE("an instance's entry holds its identity and its open project", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    InstanceRegistry   registry(dir.path());
    CHECK(registry.publish(identity(4242, 13619), running({4242})).empty());

    const json entry = read_entry(registry.entry_path(4242));
    CHECK(entry.at("schema") == 1);
    CHECK(entry.at("instance_id") == "id-4242");
    CHECK(entry.at("pid") == 4242);
    CHECK(entry.at("port") == 13619);
    CHECK(entry.at("url") == "http://127.0.0.1:13619/mcp");
    CHECK(entry.at("version") == "2.5.0.6-dev");
    CHECK(entry.at("executable") == "/Applications/OrcaMCP.app/Contents/MacOS/OrcaSlicer");
    CHECK(entry.at("data_dir") == "/data/OrcaMCP");
    CHECK(entry.at("started_at") == "2026-09-28T09:15:03.123Z");
    CHECK(entry.at("project") == json{{"name", "bracket"}, {"path", "/prints/bracket.3mf"}, {"unsaved", false}});
    CHECK(identity_from_json(entry)->project == identity(4242, 13619).project);
}

TEST_CASE("an agent's launch token is in the entry, and read back", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    InstanceRegistry   registry(dir.path());
    InstanceIdentity   launched = identity(4242, 13619);
    launched.launch_id          = "0b6f3c1e-5d52-4c1a-9f0e-2a7d8c4b1e90";
    REQUIRE(registry.publish(launched, running({4242})).empty());

    const json entry = read_entry(registry.entry_path(4242));
    CHECK(entry.at("launch_id") == launched.launch_id);
    CHECK(identity_from_json(entry)->launch_id == launched.launch_id);
}

TEST_CASE("an instance no agent launched says so with an empty launch token", "[InstanceRegistry]")
{
    // The key's presence tells the bridge this build records tokens: it then never takes this window,
    // opened by the user, for the one its start_orca launched.
    const json entry = to_json(identity(4242, 13619));
    REQUIRE(entry.contains("launch_id"));
    CHECK(entry.at("launch_id") == "");
}

namespace {
void set_launch_id_variable(const char* value)
{
#ifdef _WIN32
    _putenv_s("ORCAMCP_LAUNCH_ID", value);
#else
    ::setenv("ORCAMCP_LAUNCH_ID", value, 1);
#endif
}
} // namespace

TEST_CASE("the launch token is taken from the environment once, and cleared from it", "[InstanceRegistry]")
{
    set_launch_id_variable("token-of-this-launch");
    CHECK(take_launch_id() == "token-of-this-launch");
    // A window this instance opens later inherits its environment: it must not carry the token too.
    CHECK(std::getenv("ORCAMCP_LAUNCH_ID") == nullptr);
    CHECK(take_launch_id().empty());
}

TEST_CASE("an entry is written whole, with nothing left beside it", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    InstanceRegistry   registry(dir.path());
    REQUIRE(registry.publish(identity(4242, 13618), running({4242})).empty());
    REQUIRE(registry.update_project({"bracket", "/prints/bracket.3mf", true}));
    CHECK(entry_files(dir.path()) == std::set<std::string>{"4242.json"}); // no .tmp left over
}

#ifndef _WIN32 // Windows keeps the user's profile to the user by its ACLs
TEST_CASE("the entries are the user's alone", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    const fs::path     folder = dir.path() / "instances"; // created by the registry, as ~/.orcamcp/instances is
    InstanceRegistry   registry(folder);
    REQUIRE(registry.publish(identity(4242, 13618), running({4242})).empty());
    CHECK((fs::status(folder).permissions() & fs::all_all) == fs::owner_all);
    CHECK((fs::status(registry.entry_path(4242)).permissions() & fs::all_all) == (fs::owner_read | fs::owner_write));
}
#endif

TEST_CASE("the project in the entry follows the project in the window, rewritten only when it changes", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    InstanceRegistry   registry(dir.path());
    REQUIRE(registry.publish(identity(4242, 13618, "Untitled"), running({4242})).empty());

    CHECK_FALSE(registry.update_project({"Untitled", "/prints/Untitled.3mf", false}));
    CHECK(registry.update_project({"bracket", "/prints/bracket.3mf", true}));
    CHECK(read_entry(registry.entry_path(4242)).at("project") ==
          json{{"name", "bracket"}, {"path", "/prints/bracket.3mf"}, {"unsaved", true}});
    CHECK(registry.identity()->project.unsaved);
}

TEST_CASE("nothing is written before the instance is published", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    InstanceRegistry   registry(dir.path());
    CHECK_FALSE(registry.update_project({"bracket", "", true}));
    CHECK_FALSE(registry.identity());
    CHECK(entry_files(dir.path()).empty());
}

TEST_CASE("a quitting instance's entry is removed, and it still answers as itself", "[InstanceRegistry]")
{
    // Until it is gone it answers calls: a call stamped for it must get "quitting" (-32002), not "meant
    // for another instance" (-32004), and GET /mcp must not make it look like an older OrcaMCP.
    ScopedTemporaryDir dir("orcamcp-instances");
    InstanceRegistry   registry(dir.path());
    REQUIRE(registry.publish(identity(4242, 13618), running({4242})).empty());
    registry.withdraw();
    CHECK_FALSE(fs::exists(registry.entry_path(4242)));
    REQUIRE(registry.identity());
    CHECK(registry.identity()->instance_id == "id-4242");
    CHECK_FALSE(wrong_instance_refusal(json{{"_meta", {{instance_meta_key, "id-4242"}}}}, registry.identity()));
    CHECK_FALSE(registry.update_project({"later", "", true})); // withdrawn: the entry never comes back
    CHECK_FALSE(fs::exists(registry.entry_path(4242)));
}

#ifndef _WIN32 // a folder's permissions do not keep Windows from writing in it
TEST_CASE("a project change the entry could not take is written on the next refresh", "[InstanceRegistry]")
{
    // On Windows the rename fails while the bridge has the file open; the change must not be lost.
    ScopedTemporaryDir dir("orcamcp-instances");
    InstanceRegistry   registry(dir.path());
    REQUIRE(registry.publish(identity(4242, 13618, "Untitled"), running({4242})).empty());
    const ProjectInfo saved{"bracket", "/prints/bracket.3mf", false};

    fs::permissions(dir.path(), fs::owner_read | fs::owner_exe); // no writing in the folder
    registry.update_project(saved);
    fs::permissions(dir.path(), fs::owner_all);
    CHECK(read_entry(registry.entry_path(4242)).at("project").at("name") == "Untitled"); // not written yet

    registry.update_project(saved); // the next refresh, nothing changed since
    CHECK(read_entry(registry.entry_path(4242)).at("project").at("name") == "bracket");
}
#endif

TEST_CASE("an instance knows whether it started beside another of its program on its data folder", "[InstanceRegistry]")
{
    // A restart has none: only then may the bridge take it for the instance that quit, and not for a
    // second window someone opened while the first ran.
    ScopedTemporaryDir dir("orcamcp-instances");
    InstanceIdentity   other_folder = identity(1003, 13620);
    other_folder.data_dir           = "/copies/OrcaMCP";
    write_entry(dir.path(), other_folder);
    InstanceRegistry alone(dir.path());
    REQUIRE(alone.publish(identity(4242, 13618), running({1003, 4242})).empty());
    CHECK(alone.identity()->alone_at_start);
    CHECK(read_entry(alone.entry_path(4242)).at("alone_at_start") == true);

    write_entry(dir.path(), identity(1002, 13619)); // the same program on the same data folder, running
    InstanceRegistry beside(dir.path());
    REQUIRE(beside.publish(identity(4243, 13621), running({1002, 1003, 4243})).empty());
    CHECK_FALSE(beside.identity()->alone_at_start);
}

TEST_CASE("an entry whose pid now runs another program is stale", "[InstanceRegistry]")
{
    // A crashed instance's pid can be reused by anything: the entry is kept only while that pid runs the
    // program the entry names.
    InstanceIdentity self = identity(Slic3r::get_current_pid(), 13618);
    self.executable       = boost::dll::program_location().string();
    CHECK(instance_process_runs(self));
    self.executable = "/Applications/SomethingElse.app/Contents/MacOS/SomethingElse";
    CHECK_FALSE(instance_process_runs(self));
    CHECK_FALSE(instance_process_runs(identity(0, 13618)));
}

TEST_CASE("publishing clears the entries of crashed instances and keeps the running ones", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    write_entry(dir.path(), identity(1001, 13619, "crashed"));  // its process is gone
    write_entry(dir.path(), identity(1002, 13620, "running"));  // runs
    write_entry(dir.path(), identity(1003, 13618, "reused"));   // claims the port this instance now holds
    InstanceRegistry registry(dir.path());
    REQUIRE(registry.publish(identity(4242, 13618), running({1002, 1003, 4242})).empty());

    CHECK(entry_files(dir.path()) == std::set<std::string>{"1002.json", "4242.json"});
    const auto others = registry.others(running({1002, 4242}));
    REQUIRE(others.size() == 1);
    CHECK(others[0].pid == 1002);
    CHECK(others[0].project.name == "running");
}

TEST_CASE("a file that is not an entry is neither read nor removed", "[InstanceRegistry]")
{
    ScopedTemporaryDir dir("orcamcp-instances");
    {
        boost::nowide::ofstream out((dir.path() / "999.json").string());
        out << "{\"pid\": 999"; // half a file, from something else
    }
    InstanceRegistry registry(dir.path());
    REQUIRE(registry.publish(identity(4242, 13618), running({4242})).empty());
    CHECK(registry.others(running({999, 4242})).empty());
    CHECK(fs::exists(dir.path() / "999.json"));
}

TEST_CASE("an entry without an instance id, pid or port is not an instance", "[InstanceRegistry]")
{
    const json whole = to_json(identity(4242, 13618));
    CHECK(identity_from_json(whole));
    for (const char* key : {"instance_id", "pid", "port"}) {
        json partial = whole;
        partial.erase(key);
        CHECK_FALSE(identity_from_json(partial));
    }
    CHECK_FALSE(identity_from_json(json::array()));
}

TEST_CASE("every instance id is new, and 32 hex digits", "[InstanceRegistry]")
{
    const std::string first = new_instance_id();
    CHECK(std::regex_match(first, std::regex("[0-9a-f]{32}")));
    CHECK(new_instance_id() != first);
}

TEST_CASE("start times are UTC with milliseconds, so launches a second apart order", "[InstanceRegistry]")
{
    const auto when = std::chrono::system_clock::time_point(std::chrono::milliseconds(1759050903123LL));
    CHECK(utc_timestamp(when) == "2025-09-28T09:15:03.123Z");
}

TEST_CASE("an instance answers at 127.0.0.1, the one address it listens on", "[InstanceRegistry]")
{
    CHECK(mcp_url(13619) == "http://127.0.0.1:13619/mcp");
}

TEST_CASE("a running process is alive and a finished one is not", "[InstanceRegistry]")
{
    CHECK(process_is_alive(Slic3r::get_current_pid()));
    CHECK_FALSE(process_is_alive(0));
}

TEST_CASE("a call meant for another instance is refused, and one meant for this or none runs", "[InstanceRegistry]")
{
    const InstanceIdentity self = identity(4242, 13619);
    auto stamped = [](const std::string& id) { return json{{"name", "new_project"}, {"_meta", {{instance_meta_key, id}}}}; };

    CHECK_FALSE(wrong_instance_refusal(stamped("id-4242"), self));
    CHECK_FALSE(wrong_instance_refusal(json{{"name", "new_project"}}, self)); // an older bridge, or curl
    CHECK_FALSE(wrong_instance_refusal(json{{"_meta", {{"orcamcp/wait_cap_s", 10}}}}, self));

    const auto refusal = wrong_instance_refusal(stamped("id-1001"), self);
    REQUIRE(refusal);
    CHECK(refusal->find("id-1001") != std::string::npos);
    CHECK(refusal->find("pid 4242 on port 13619") != std::string::npos);
    CHECK(refusal->find("not run") != std::string::npos);
    // An older OrcaMCP ignores the stamp, so the bridge names one "legacy": a new instance refuses it.
    CHECK(wrong_instance_refusal(stamped("legacy"), self));
    CHECK(wrong_instance_refusal(stamped("id-4242"), std::nullopt));
}

TEST_CASE("the server refuses a call meant for another instance with -32004 before running it", "[InstanceRegistry][orcamcp]")
{
    // No instance is published in the test process, so every stamped call is someone else's. The tool
    // does not exist: were the check missing, the call would be refused for that (-32602), not run.
    const json request = {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/call"},
                          {"params", {{"name", "no_such_tool"}, {"arguments", json::object()}, {"_meta", {{instance_meta_key, "id-1001"}}}}}};
    const json reply = reply_body(*OrcaMCPServer::handle_request("POST", "/mcp", request.dump()));
    CHECK(reply.at("id") == 7);
    CHECK(reply.at("error").at("code") == WrongInstance::error_code);
    CHECK(reply.at("error").at("code") == -32004);
}

TEST_CASE("the user is told when this instance is not on the first port, and why", "[InstanceRegistry]")
{
    CHECK_FALSE(other_port_note(13618, {}));

    const auto held = other_port_note(13619, {identity(1002, 13618, "benchy")});
    REQUIRE(held);
    CHECK(held->find("port 13619") != std::string::npos);
    CHECK(held->find("pid 1002") != std::string::npos);
    CHECK(held->find("benchy") != std::string::npos);

    const auto foreign = other_port_note(13619, {});
    REQUIRE(foreign);
    CHECK(foreign->find("another program") != std::string::npos);

    CHECK(no_port_warning().find("13618 to 13627") != std::string::npos);
}

TEST_CASE("instances sharing a data folder are warned about", "[InstanceRegistry]")
{
    InstanceIdentity elsewhere = identity(1003, 13620);
    elsewhere.data_dir         = "/copies/OrcaMCP";
    CHECK_FALSE(shared_data_folder_warning("/data/OrcaMCP", {elsewhere}));

    const auto shared = shared_data_folder_warning("/data/OrcaMCP", {identity(1002, 13618), elsewhere});
    REQUIRE(shared);
    CHECK(shared->find("pid 1002") != std::string::npos);
    CHECK(shared->find("1003") == std::string::npos);
}

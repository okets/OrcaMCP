"""start_orca launches only the installed OrcaMCP, or ORCAMCP_APP_PATH: never a build found in the source
folder, which would run on the user's real data folder (the user, 2026-09-28).

Run from the repo root:  python3 -m unittest discover -s scripts/tests -t scripts
"""

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bridge_test_support import REPO_ROOT, load_bridge  # noqa: E402


class LaunchTargetTests(unittest.TestCase):
    def test_start_orca_never_launches_a_build_in_a_source_folder(self):
        """A build found in the repo would run on the user's real data folder (the user, 2026-09-28)."""
        bridge = load_bridge()
        with mock.patch.object(bridge.os.path, "isfile", side_effect=lambda path: path.startswith(REPO_ROOT)):
            self.assertIsNone(bridge.get_orcamcp_executable())

    def test_orcamcp_app_path_is_launched_when_set(self):
        bridge = load_bridge(ORCAMCP_APP_PATH="/opt/test/OrcaSlicer")
        with mock.patch.object(bridge.os.path, "isfile", return_value=True):
            self.assertEqual(bridge.get_orcamcp_executable(), "/opt/test/OrcaSlicer")

    def test_on_linux_the_installed_program_is_orca_mcp(self):
        """The Linux binary is named orca-mcp (SLIC3R_APP_CMD, version.inc): the search found no install."""
        bridge = load_bridge()
        with mock.patch("platform.system", return_value="Linux"), \
                mock.patch.object(bridge.os.path, "isfile", side_effect=lambda path: path == "/usr/bin/orca-mcp"):
            self.assertEqual(bridge.get_orcamcp_executable(), "/usr/bin/orca-mcp")


class FakeWinreg:
    """winreg as the bridge reads it, holding only the install folders a test names, by (hive, view)."""
    HKEY_CURRENT_USER = "HKCU"
    HKEY_LOCAL_MACHINE = "HKLM"
    KEY_READ = 0x20019
    KEY_WOW64_64KEY = 0x0100
    KEY_WOW64_32KEY = 0x0200

    def __init__(self, folders):
        self.folders = folders  # {(hive, view): folder}

    def OpenKey(self, hive, sub_key, reserved, access):
        view = access & (self.KEY_WOW64_64KEY | self.KEY_WOW64_32KEY)
        if sub_key != r"Software\OrcaMCP\OrcaMCP" or (hive, view) not in self.folders:
            raise FileNotFoundError(sub_key)
        return FakeKey(self.folders[(hive, view)])

    def QueryValueEx(self, key, name):
        if name != "":
            raise FileNotFoundError(name)
        return key.folder, 1  # 1: REG_SZ


class FakeKey:
    def __init__(self, folder):
        self.folder = folder

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class WindowsInstallFolderTests(unittest.TestCase):
    """The Windows installer lets the user choose the folder; it records it in the registry (CPack's NSIS
    template: the default value of Software\\OrcaMCP\\OrcaMCP). A review, 2026-11-18: start_orca found no app
    installed anywhere but the default folder."""

    def executable(self, folders, installed):
        bridge = load_bridge()
        with mock.patch.dict(sys.modules, {"winreg": FakeWinreg(folders)}), \
                mock.patch("platform.system", return_value="Windows"), \
                mock.patch.object(bridge.os.path, "isfile", side_effect=lambda path: path == installed):
            return bridge.get_orcamcp_executable()

    def test_an_install_for_all_users_in_another_folder_is_found(self):
        """A 32-bit installer's write to HKLM lands in the registry's 32-bit view."""
        folders = {(FakeWinreg.HKEY_LOCAL_MACHINE, FakeWinreg.KEY_WOW64_32KEY): "D:\\Apps\\OrcaMCP"}
        self.assertEqual(self.executable(folders, "D:\\Apps\\OrcaMCP\\orca-mcp.exe"), "D:\\Apps\\OrcaMCP\\orca-mcp.exe")

    def test_an_install_for_one_user_in_another_folder_is_found(self):
        folders = {(FakeWinreg.HKEY_CURRENT_USER, FakeWinreg.KEY_WOW64_64KEY): "E:\\Tools\\Orca"}
        self.assertEqual(self.executable(folders, "E:\\Tools\\Orca\\orca-mcp.exe"), "E:\\Tools\\Orca\\orca-mcp.exe")

    def test_a_folder_the_registry_names_but_that_holds_no_app_is_passed_over(self):
        """A folder deleted by hand leaves its registry value behind."""
        folders = {(FakeWinreg.HKEY_LOCAL_MACHINE, FakeWinreg.KEY_WOW64_32KEY): "D:\\Gone"}
        default = "C:\\Program Files\\OrcaMCP\\orca-mcp.exe"
        bridge = load_bridge(ProgramFiles="C:\\Program Files")
        with mock.patch.dict(sys.modules, {"winreg": FakeWinreg(folders)}), \
                mock.patch("platform.system", return_value="Windows"), \
                mock.patch.dict(os.environ, {"ProgramFiles": "C:\\Program Files"}), \
                mock.patch.object(bridge.os.path, "isfile", side_effect=lambda path: path == default):
            self.assertEqual(bridge.get_orcamcp_executable(), default)

    def test_when_nothing_is_found_the_answer_names_every_place_it_looked(self):
        """The agent, or its user, can then see which folder the installer recorded, and set ORCAMCP_APP_PATH."""
        folders = {(FakeWinreg.HKEY_LOCAL_MACHINE, FakeWinreg.KEY_WOW64_32KEY): "D:\\Gone"}
        bridge = load_bridge()
        with mock.patch.dict(sys.modules, {"winreg": FakeWinreg(folders)}), \
                mock.patch("platform.system", return_value="Windows"), \
                mock.patch.object(bridge.os.path, "isfile", return_value=False):
            answer = bridge.launch_orcamcp()
        self.assertEqual(answer["report"]["status"], "not_started")
        self.assertIn("D:\\Gone\\orca-mcp.exe", answer["message"])
        self.assertIn("ORCAMCP_APP_PATH", answer["message"])


class LogFolderTests(unittest.TestCase):
    """Where the app writes its log, which start_orca names when a launch does not answer: the log folder of
    the data folder the app takes by default (GUI_App.cpp; CLAUDE.md, "Where the app's data lives")."""

    def folder(self, system, **environ):
        bridge = load_bridge(**environ)
        return bridge.default_log_folder(system)

    def test_linux_follows_xdg_config_home(self):
        self.assertEqual(self.folder("Linux", XDG_CONFIG_HOME="/home/me/.cfg"), "/home/me/.cfg/OrcaMCP/log")

    def test_linux_without_xdg_config_home_uses_dot_config(self):
        bridge = load_bridge()
        bridge.ENV.pop("XDG_CONFIG_HOME", None)
        self.assertEqual(bridge.default_log_folder("Linux"), os.path.expanduser("~/.config/OrcaMCP/log"))

    def test_macos(self):
        self.assertEqual(self.folder("Darwin"), os.path.expanduser("~/Library/Application Support/OrcaMCP/log"))

    def test_windows_uses_appdata(self):
        self.assertEqual(self.folder("Windows", APPDATA="C:\\Users\\me\\AppData\\Roaming"),
                         os.path.join("C:\\Users\\me\\AppData\\Roaming", "OrcaMCP", "log"))


DEBIAN_BUNDLE = "/etc/ssl/certs/ca-certificates.crt"


class AgentLaunchEnvironmentTests(unittest.TestCase):
    """What an agent's launch adds to the environment OrcaMCP starts in."""

    def variables(self, environ=None, system="Linux", bundles=(DEBIAN_BUNDLE,)):
        bridge = load_bridge()
        with mock.patch.object(bridge.os.path, "isfile", side_effect=lambda path: path in bundles):
            return bridge.agent_launch_variables("launch-token", environ or {}, system)

    def test_the_launch_says_it_is_an_agents_and_carries_its_token(self):
        variables = self.variables(system="Darwin")
        self.assertEqual(variables, {"ORCAMCP_SKIP_CLOUD_LOGIN": "1", "ORCAMCP_LAUNCH_ID": "launch-token"})

    def test_on_linux_the_system_certificate_bundle_is_named_when_none_is(self):
        """Without it the Linux build asks, before its MCP server starts, whether to use the system's."""
        self.assertEqual(self.variables()["SSL_CERT_FILE"], DEBIAN_BUNDLE)

    def test_the_first_bundle_the_app_would_take_is_named(self):
        """The app's own order (Http.cpp, CA_BUNDLES): Fedora's before Debian's."""
        fedora = "/etc/pki/tls/certs/ca-bundle.crt"
        self.assertEqual(self.variables(bundles=(DEBIAN_BUNDLE, fedora))["SSL_CERT_FILE"], fedora)

    def test_a_certificate_bundle_already_set_is_kept(self):
        self.assertNotIn("SSL_CERT_FILE", self.variables(environ={"SSL_CERT_FILE": "/home/me/ca.pem"}))

    def test_no_bundle_is_named_when_none_is_there(self):
        self.assertNotIn("SSL_CERT_FILE", self.variables(bundles=()))

    def test_macos_and_windows_are_given_no_bundle(self):
        """Only the Linux build asks about certificates."""
        for system in ("Darwin", "Windows"):
            with self.subTest(system=system):
                self.assertNotIn("SSL_CERT_FILE", self.variables(system=system))


if __name__ == "__main__":
    unittest.main()

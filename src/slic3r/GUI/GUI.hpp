#ifndef slic3r_GUI_hpp_
#define slic3r_GUI_hpp_

namespace boost { class any; }
namespace boost::filesystem { class path; }

#include <wx/string.h>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"

class wxWindow;
class wxMenuBar;
class wxComboCtrl;
class wxFileDialog;
class wxTopLevelWindow;

namespace Slic3r { 

class AppConfig;
class DynamicPrintConfig;
class Print;

namespace GUI {

void disable_screensaver();
void enable_screensaver();
bool debugged();
void break_to_debugger();

// Platform specific Ctrl+/Alt+ (Windows, Linux) vs. ⌘/⌥ (OSX) prefixes 
extern const std::string& shortkey_ctrl_prefix();
extern const std::string& shortkey_alt_prefix();
extern const std::string& shortkey_shift_prefix(); // Shift is the same on all platforms, but we provide a function for consistency with Ctrl/Alt prefixes

extern AppConfig* get_app_config();

extern void add_menus(wxMenuBar *menu, int event_preferences_changed, int event_language_change);

// Change option value in config
void change_opt_value(DynamicPrintConfig& config, const t_config_option_key& opt_key, const boost::any& value, int opt_index = 0);

// If has_code_excerpts is true, code excerpts (a source line and the caret line below it) render
// monospaced so the caret aligns. Used for placeholder-parser errors.
void show_error(wxWindow* parent, const wxString& message, bool has_code_excerpts = false);
void show_error(wxWindow* parent, const char* message, bool has_code_excerpts = false);
inline void show_error(wxWindow* parent, const std::string& message, bool has_code_excerpts = false) { show_error(parent, message.c_str(), has_code_excerpts); }
void show_error_id(int id, const std::string& message);   // For Perl
void show_info(wxWindow* parent, const wxString& message, const wxString& title = wxString());
void show_info(wxWindow* parent, const char* message, const char* title = nullptr);
inline void show_info(wxWindow* parent, const std::string& message,const std::string& title = std::string()) { show_info(parent, message.c_str(), title.c_str()); }
void warning_catcher(wxWindow* parent, const wxString& message);

// MCP dialog suppression - captures info messages instead of showing dialogs
void set_mcp_dialog_suppression(bool suppress);
bool is_mcp_dialog_suppression_enabled();
std::vector<std::string> get_mcp_suppressed_messages();
void add_mcp_suppressed_message(const std::string& msg);
void clear_mcp_suppressed_messages();
// A suppressed prompt that offered a choice is recorded with the answer automation gave it, as
// "<prompt> (auto-answered <answer>)", so an agent can tell a merge or a rescale from a notice.
std::string mcp_answered_prompt(const std::string& prompt, const std::string& answer);
void add_mcp_suppressed_answer(const std::string& prompt, const std::string& answer);
// How MsgDialog answers a suppressed prompt with the given wx button style: Yes when it has a Yes or
// No button, OK otherwise. offers_choice is false for an OK-only box, which records its text alone.
int mcp_default_answer(long style);
bool mcp_prompt_offers_choice(long style);
std::string mcp_answer_label(int answer_id);
// "a, b, c and 4 more": at most max_items of `items`, for naming what a suppressed prompt affected.
std::string mcp_list_summary(const std::vector<std::string>& items, size_t max_items);
// Per-prompt answers. A tool that knows how one prompt should be answered sets it by key for the
// rest of its McpDialogSuppressionGuard, optionally with a note for the agent (how to get the other
// outcome); a MsgDialog tagged with that key (set_mcp_prompt_key) then takes it instead of
// mcp_default_answer. mcp_answer_for is the answer a suppressed MsgDialog gives, and its text as
// recorded: "Yes", or "Yes; <note>".
inline constexpr const char* MCP_PROMPT_MULTIPART = "multipart"; // load a file's objects as one object's parts?
struct McpAnswer
{
    int         id;
    std::string text;
};
void set_mcp_prompt_answer(const std::string& key, int answer_id, const std::string& note = std::string());
void clear_mcp_prompt_answers();
McpAnswer mcp_answer_for(long style, const std::string& prompt_key);
// True when the app was launched for an agent: the bridge's start_orca sets ORCAMCP_SKIP_CLOUD_LOGIN
// (to anything but "" or "0"). Such a launch has nobody at the screen, so startup must not wait on
// anything a person has to answer, such as a keychain or macOS privacy prompt.
bool is_agent_launch();
void show_substitutions_info(const PresetsConfigSubstitutions& presets_config_substitutions);
void show_substitutions_info(const ConfigSubstitutions& config_substitutions, const std::string& filename);

// Creates a wxCheckListBoxComboPopup inside the given wxComboCtrl, filled with the given text and items.
// Items data must be separated by '|', and contain the item name to be shown followed by its initial value (0 for false, 1 for true).
// For example "Item1|0|Item2|1|Item3|0", and so on.
void create_combochecklist(wxComboCtrl* comboCtrl, const std::string& text, const std::string& items);

// Returns the current state of the items listed in the wxCheckListBoxComboPopup contained in the given wxComboCtrl,
// encoded inside an unsigned int.
unsigned int combochecklist_get_flags(wxComboCtrl* comboCtrl);

// Sets the current state of the items listed in the wxCheckListBoxComboPopup contained in the given wxComboCtrl,
// with the flags encoded in the given unsigned int.
void combochecklist_set_flags(wxComboCtrl* comboCtrl, unsigned int flags);

// wxString conversions:

// wxString from std::string in UTF8
wxString	from_u8(const std::string &str);
// std::string in UTF8 from wxString
std::string	into_u8(const wxString &str);
// wxString from boost path
wxString	from_path(const boost::filesystem::path &path);
// boost path from wxString
boost::filesystem::path	into_path(const wxString &str);

// Display an About dialog
extern void about();
// Ask the destop to open the datadir using the default file explorer.
extern void desktop_open_datadir_folder();
// Ask the destop to open one folder
extern void desktop_open_any_folder(const std::string& path);
} // namespace GUI
} // namespace Slic3r

#endif

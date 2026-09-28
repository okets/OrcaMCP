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
// Clears the errors too: they are messages.
void clear_mcp_suppressed_messages();
// An error show_error captured instead of showing: recorded as a message, as a synchronous
// ErrorDialog would have been, and as an error, so a tool can fail with its words.
void add_mcp_suppressed_error(const std::string& msg);
std::vector<std::string> get_mcp_suppressed_errors();
// Whether show_error captures its message instead of showing it. Its dialog is deferred
// (CallAfter), so under MCP it opened only after the call had returned and suppression was over:
// captured under suppression on the GUI thread, which owns the capture lists; deferred as before
// otherwise.
bool mcp_captures_error(bool suppression_enabled, bool on_main_thread);
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
// Clears the answers, and which keyed prompts were asked.
void clear_mcp_prompt_answers();
// Which keyed prompts a suppressed MsgDialog asked since the outermost guard began: a tool that set an
// answer can tell whether the app asked at all, whatever language its text is in.
void record_mcp_prompt_asked(const std::string& key);
bool was_mcp_prompt_asked(const std::string& key);
McpAnswer mcp_answer_for(long style, const std::string& prompt_key);
inline constexpr const char* MCP_PROMPT_SPLIT_FLOATING = "split_floating"; // split to objects: keep floating pieces' height?
// Under suppression, a STEP import's StepMeshDialog is not opened: the configured deflection answers it
// (recorded when the dialog would have shown) and the out parameters are filled. False without
// suppression: open it. Defined in Plater.cpp; the object list's part loader asks it too.
bool mcp_skip_step_mesh_dialog(double linear, double angle, bool split_compound,
                               double& linear_value, double& angle_value, bool& is_split);
// Per-call file and folder answers, for the native file and folder dialogs suppression cannot catch
// (a modal opened inside a tool call blocks it for good). A tool that has the path such a dialog would
// ask for sets it for the rest of its McpDialogSuppressionGuard (answer_file, answer_files,
// answer_folder), and the dialog's call site reads it with mcp_answer_path_dialog instead of opening
// the dialog. With no answer set the site behaves as if the dialog were cancelled. Files and folders
// are kept apart: a file answer never answers a folder dialog, nor the other way round.
enum class McpPathDialog { file, folder };
void set_mcp_path_answer(McpPathDialog kind, const std::vector<std::string>& paths);
void clear_mcp_path_answers();
// For the call site of a native file or folder dialog titled `title`. Under suppression: `paths` gets
// the answer set for `kind` (empty: Cancel), the answer is recorded as "<title> (auto-answered <paths>)"
// or "(auto-answered Cancel)", and it returns true -- the site must not open its dialog. Without
// suppression it returns false, and the site opens its dialog as usual.
bool mcp_answer_path_dialog(const std::string& title, McpPathDialog kind, std::vector<std::string>& paths);
// The fallback for a modal DPIDialog that no specific handler answered (see DPIAware::ShowModal):
// under suppression it is not shown, answers Cancel and is recorded as
// "<title> was suppressed (auto-answered Cancel)". Not suppressed: shown as usual.
struct McpUnhandledModal
{
    bool        suppress;
    int         answer;
    std::string message;
};
McpUnhandledModal mcp_unhandled_modal(bool suppression_enabled, const std::string& title);
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

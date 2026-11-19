#include "OrcaMCPCodexToml.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#endif
#define TOML_ENABLE_FORMATTERS 0     // the text is edited where it stands, never written from a tree
#define TOML_ENABLE_WINDOWS_COMPAT 0 // no wide-string overloads: the config is read as UTF-8 bytes
#include <toml.hpp>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

const char* const SERVERS_KEY = "mcp_servers";
const char* const COMMAND_KEY = "command";
const char* const ARGS_KEY    = "args";
const char* const STARTUP_KEY = "startup_timeout_sec";
const char* const TOOL_KEY    = "tool_timeout_sec";

// The UTF-8 BOM a file may start with: the parser skips it, and no region ever includes it.
size_t bom_length(const std::string& text)
{
    return text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                   static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF
               ? 3
               : 0;
}

// Where each line of a text starts, for the parser's 1-based line numbers.
class LineIndex
{
public:
    explicit LineIndex(const std::string& text) : m_size(text.size())
    {
        m_starts.push_back(0);
        for (size_t pos = 0; pos < text.size(); ++pos)
            if (text[pos] == '\n')
                m_starts.push_back(pos + 1);
    }

    size_t count() const { return m_starts.size(); }
    // The offset of the line's first byte.
    size_t begin_of(size_t line) const { return line >= 1 && line <= m_starts.size() ? m_starts[line - 1] : m_size; }
    // The offset just past the line's newline (the text's end for the last line).
    size_t end_of(size_t line) const { return line < m_starts.size() ? m_starts[line] : m_size; }

private:
    std::vector<size_t> m_starts;
    size_t              m_size;
};

// The text parsed, or nullopt with `error` (when given) naming where and why it is not TOML.
std::optional<toml::table> parse_toml(const std::string& text, std::string* error)
{
    try {
        return toml::parse(text);
    } catch (const toml::parse_error& e) {
        if (error != nullptr)
            *error = "line " + std::to_string(e.source().begin.line) + ", column " +
                     std::to_string(e.source().begin.column) + ": " + std::string(e.description());
        return std::nullopt;
    }
}

// An array that [[...]] headers built: every element a table that is not inline. Its tables hold headers;
// any other array is a value written in place.
bool is_array_of_header_tables(const toml::array& array)
{
    if (array.empty())
        return false;
    return std::all_of(array.begin(), array.end(), [](const toml::node& element) {
        const toml::table* table = element.as_table();
        return table != nullptr && !table->is_inline();
    });
}

// The source of every value written in place -- a string, number, array or inline table -- wherever it
// sits: no table header can stand inside one.
void collect_value_regions(const toml::node& node, std::vector<toml::source_region>& regions)
{
    if (const toml::table* table = node.as_table()) {
        if (table->is_inline()) {
            regions.push_back(table->source());
            return;
        }
        for (auto&& [key, child] : *table)
            collect_value_regions(child, regions);
        return;
    }
    if (const toml::array* array = node.as_array()) {
        if (is_array_of_header_tables(*array)) {
            for (const toml::node& element : *array)
                collect_value_regions(element, regions);
            return;
        }
    }
    regions.push_back(node.source());
}

// Whether a '[' at the start of `line` (after blanks only, at `column`) lies inside a value that began on
// an earlier line: a multi-line string's or array's own line, not a header.
bool inside_value(const std::vector<toml::source_region>& regions, size_t line, size_t column)
{
    return std::any_of(regions.begin(), regions.end(), [line, column](const toml::source_region& region) {
        return region.begin.line < line &&
               (region.end.line > line || (region.end.line == line && region.end.column > column));
    });
}

// One table header of the text: where its line starts and what it names.
struct Header
{
    size_t                   offset = 0; // the line's first byte (after the BOM on the first line)
    size_t                   line   = 0;
    std::vector<std::string> path;
    bool                     array = false; // [[...]]
};

// What a header line names, read by the parser from the line alone: the keys of the table it opens, each
// unquoted and unescaped, and whether it opens an array of tables.
std::optional<Header> read_header_line(const std::string& line_text)
{
    const std::optional<toml::table> doc = parse_toml(line_text, nullptr);
    if (!doc)
        return std::nullopt;
    Header             header;
    const toml::table* table = &*doc;
    while (table->size() == 1) {
        auto&& [key, child] = *table->begin();
        header.path.emplace_back(key.str());
        if (const toml::table* inner = child.as_table()) {
            table = inner;
            continue;
        }
        if (child.is_array())
            header.array = true;
        break;
    }
    if (header.path.empty())
        return std::nullopt;
    return header;
}

// Every table header of a text the parser read, in order. A line counts when its first non-blank
// character is '[' and no value written in place covers it.
std::vector<Header> table_headers(const std::string& text, const toml::table& root)
{
    std::vector<toml::source_region> values;
    collect_value_regions(root, values);

    const LineIndex     lines(text);
    std::vector<Header> headers;
    for (size_t line = 1; line <= lines.count(); ++line) {
        const size_t line_begin = lines.begin_of(line) + (line == 1 ? bom_length(text) : 0);
        const size_t line_end   = lines.end_of(line);
        size_t       pos        = line_begin;
        while (pos < line_end && (text[pos] == ' ' || text[pos] == '\t'))
            ++pos;
        if (pos >= line_end || text[pos] != '[')
            continue;
        if (inside_value(values, line, pos - line_begin + 1))
            continue;
        std::optional<Header> header = read_header_line(text.substr(pos, line_end - pos));
        if (!header)
            continue;
        header->offset = line_begin;
        header->line   = line;
        headers.push_back(std::move(*header));
    }
    return headers;
}

bool names_server(const Header& header, const std::string& server_name)
{
    return header.path.size() >= 2 && header.path[0] == SERVERS_KEY && header.path[1] == server_name;
}

bool is_server_table(const Header& header, const std::string& server_name)
{
    return names_server(header, server_name) && header.path.size() == 2 && !header.array;
}

toml::node* server_node(toml::table& root, const std::string& server_name)
{
    return root[SERVERS_KEY][server_name].node();
}

// The byte range of one key's statement in `table`: its key's line through the line its value ends on,
// newline included. TOML ends every statement with its line, so no other statement shares those lines.
std::optional<TomlSpan> statement_span(const toml::table& table, const char* key, const LineIndex& lines)
{
    const auto entry = table.find(key);
    if (entry == table.end())
        return std::nullopt;
    return TomlSpan{lines.begin_of(entry->first.source().begin.line), lines.end_of(entry->second.source().end.line)};
}

std::string join_path(const std::string& path, std::string_view key)
{
    return path.empty() ? std::string(key) : path + "." + std::string(key);
}

// Whether two values the parser read are the same value. A float NaN equals a NaN: a config holding
// nan must not read as changed when nothing touched it.
bool same_leaf(const toml::node& a, const toml::node& b)
{
    switch (a.type()) {
    case toml::node_type::string: return a.as_string()->get() == b.as_string()->get();
    case toml::node_type::integer: return a.as_integer()->get() == b.as_integer()->get();
    case toml::node_type::boolean: return a.as_boolean()->get() == b.as_boolean()->get();
    case toml::node_type::date: return a.as_date()->get() == b.as_date()->get();
    case toml::node_type::time: return a.as_time()->get() == b.as_time()->get();
    case toml::node_type::date_time: return a.as_date_time()->get() == b.as_date_time()->get();
    case toml::node_type::floating_point: {
        const double x = a.as_floating_point()->get();
        const double y = b.as_floating_point()->get();
        return x == y || (std::isnan(x) && std::isnan(y));
    }
    default: return false;
    }
}

// The first key path at which two parsed documents differ, "" when they read the same. Inline or not,
// and the order of keys, are layout, never a difference.
std::string first_difference(const toml::node& a, const toml::node& b, const std::string& path)
{
    if (a.type() != b.type())
        return path;
    if (const toml::table* table_a = a.as_table()) {
        const toml::table& table_b = *b.as_table();
        for (auto&& [key, child] : *table_a) {
            const std::string  child_path = join_path(path, key.str());
            const toml::node*  other      = table_b.get(key.str());
            if (other == nullptr)
                return child_path;
            std::string difference = first_difference(child, *other, child_path);
            if (!difference.empty())
                return difference;
        }
        for (auto&& [key, child] : table_b)
            if (!table_a->contains(key.str()))
                return join_path(path, key.str());
        return {};
    }
    if (const toml::array* array_a = a.as_array()) {
        const toml::array& array_b = *b.as_array();
        if (array_a->size() != array_b.size())
            return path;
        for (size_t i = 0; i < array_a->size(); ++i) {
            std::string difference = first_difference((*array_a)[i], array_b[i], path + "[" + std::to_string(i) + "]");
            if (!difference.empty())
                return difference;
        }
        return {};
    }
    return same_leaf(a, b) ? std::string() : path;
}

// "" when the two documents read the same, else the words a refusal ends with.
std::string difference_in_words(const toml::table& before, const toml::table& after)
{
    const std::string path = first_difference(before, after, "");
    return path.empty() ? std::string() : path + " would change too";
}

// `root` without mcp_servers.<name>; an mcp_servers table left empty goes too when `other` has none,
// since only that entry made it (a [mcp_servers.<name>] header defines its parent implicitly).
void drop_server(toml::table& root, const toml::table& other, const std::string& server_name)
{
    toml::table* servers = root[SERVERS_KEY].as_table();
    if (servers == nullptr)
        return;
    servers->erase(server_name);
    if (servers->empty() && !other.contains(SERVERS_KEY))
        root.erase(SERVERS_KEY);
}

// What a timeout must read after Connect, when Connect may write it: the minimum, or the user's integer
// when it is at least that. nullopt when the user's value is of another type and must stay as it is.
std::optional<int64_t> timeout_after_connect(const toml::table* server_before, const char* key, int64_t minimum)
{
    const toml::node* before = server_before != nullptr ? server_before->get(key) : nullptr;
    if (before == nullptr)
        return minimum;
    if (const toml::value<int64_t>* integer = before->as_integer())
        return (std::max)(integer->get(), minimum); // parenthesized: windows.h may define max
    return std::nullopt;
}

} // namespace

CodexServerRead read_codex_server(const std::string& content, const std::string& server_name)
{
    CodexServerRead           read;
    std::optional<toml::table> root = parse_toml(content, &read.parse_error);
    if (!root)
        return read;

    const toml::node*  servers = (*root)[SERVERS_KEY].node();
    const toml::table* servers_table = servers != nullptr ? servers->as_table() : nullptr;
    read.servers_inline = servers != nullptr && (servers_table == nullptr || servers_table->is_inline());

    const toml::node* server = server_node(*root, server_name);
    read.named               = server != nullptr;
    if (server == nullptr)
        return read;
    if (const toml::table* table = server->as_table())
        if (const toml::array* args = (*table)[ARGS_KEY].as_array())
            for (const toml::node& arg : *args)
                if (const toml::value<std::string>* text = arg.as_string())
                    read.args.push_back(text->get());

    const std::vector<Header> headers = table_headers(content, *root);
    for (size_t i = 0; i < headers.size(); ++i) {
        if (!names_server(headers[i], server_name))
            continue;
        const TomlSpan span{headers[i].offset, i + 1 < headers.size() ? headers[i + 1].offset : content.size()};
        if (is_server_table(headers[i], server_name))
            read.table = span;
        else if (headers[i].path.size() == 2 && headers[i].array)
            read.array_form = true;
        read.regions.push_back(span);
    }
    return read;
}

std::optional<std::string> update_codex_table(const std::string& table_text, const std::string& server_name,
                                              const std::string& command_lines, const CodexTimeouts& minimum)
{
    std::optional<toml::table> root = parse_toml(table_text, nullptr);
    if (!root)
        return std::nullopt;
    const std::vector<Header> headers = table_headers(table_text, *root);
    if (headers.empty() || headers.front().line != 1 || !is_server_table(headers.front(), server_name))
        return std::nullopt;
    const toml::table* table = (*root)[SERVERS_KEY][server_name].as_table();
    if (table == nullptr)
        return std::nullopt;

    const LineIndex lines(table_text);
    const size_t    body     = lines.end_of(1); // just past the header line
    const size_t    main_end = headers.size() > 1 ? headers[1].offset : table_text.size();

    std::vector<TomlSpan> removed;
    std::string           written = command_lines;
    for (const char* key : {COMMAND_KEY, ARGS_KEY})
        if (const std::optional<TomlSpan> span = statement_span(*table, key, lines))
            removed.push_back(*span);
    for (const auto& [key, least] : {std::pair<const char*, int64_t>{STARTUP_KEY, minimum.startup_sec},
                                     std::pair<const char*, int64_t>{TOOL_KEY, minimum.tool_sec}}) {
        const toml::node* value = table->get(key);
        if (value != nullptr && !(value->is_integer() && value->as_integer()->get() < least))
            continue; // the user's own value: at least ours already, or not ours to reinterpret
        if (value != nullptr)
            removed.push_back(*statement_span(*table, key, lines));
        written += std::string(key) + " = " + std::to_string(least) + "\n";
    }
    for (const TomlSpan& span : removed)
        if (span.begin < body || span.end > main_end)
            return std::nullopt; // a statement outside the main table's lines cannot be the main table's
    std::sort(removed.begin(), removed.end(), [](const TomlSpan& a, const TomlSpan& b) { return a.begin < b.begin; });

    std::string out = table_text.substr(0, body);
    const bool  crlf = out.size() >= 2 && out.compare(out.size() - 2, 2, "\r\n") == 0;
    if (out.empty() || out.back() != '\n')
        out += '\n';
    if (crlf) // the file's own line endings for the lines Connect writes
        for (size_t pos = written.find('\n'); pos != std::string::npos; pos = written.find('\n', pos + 2))
            written.insert(pos, 1, '\r');
    out += written;
    size_t pos = body;
    for (const TomlSpan& span : removed) {
        out += table_text.substr(pos, span.begin - pos);
        pos = span.end;
    }
    out += table_text.substr(pos);
    return out;
}

std::string codex_connect_difference(const std::string& before, const std::string& after,
                                     const std::string& server_name, const std::string& command,
                                     const std::vector<std::string>& args, const CodexTimeouts& minimum)
{
    std::string                error;
    std::optional<toml::table> old_root = parse_toml(before, &error);
    if (!old_root)
        return "the file is not TOML (" + error + ")";
    std::optional<toml::table> new_root = parse_toml(after, &error);
    if (!new_root)
        return "the edited file would not parse (" + error + ")";

    const std::string entry      = std::string(SERVERS_KEY) + "." + server_name;
    toml::node*       old_server = server_node(*old_root, server_name);
    if (old_server != nullptr && !old_server->is_table())
        return entry + " is not a table";
    toml::table* old_table = old_server != nullptr ? old_server->as_table() : nullptr;
    toml::node*  new_node  = server_node(*new_root, server_name);
    toml::table* new_table = new_node != nullptr ? new_node->as_table() : nullptr;
    if (new_table == nullptr)
        return entry + " would not be a table";

    // What Connect writes, exactly.
    const toml::value<std::string>* new_command = (*new_table)[COMMAND_KEY].as_string();
    if (new_command == nullptr || new_command->get() != command)
        return entry + ".command would not be the bridge's interpreter";
    const toml::array* new_args = (*new_table)[ARGS_KEY].as_array();
    bool               args_ok  = new_args != nullptr && new_args->size() == args.size();
    for (size_t i = 0; args_ok && i < args.size(); ++i) {
        const toml::value<std::string>* arg = (*new_args)[i].as_string();
        args_ok                             = arg != nullptr && arg->get() == args[i];
    }
    if (!args_ok)
        return entry + ".args would not run the bridge";
    for (const auto& [key, least] : {std::pair<const char*, int64_t>{STARTUP_KEY, minimum.startup_sec},
                                     std::pair<const char*, int64_t>{TOOL_KEY, minimum.tool_sec}}) {
        const std::optional<int64_t> expected = timeout_after_connect(old_table, key, least);
        if (!expected)
            continue; // the user's own value of another type: the comparison below holds it unchanged
        const toml::value<int64_t>* now = (*new_table)[key].as_integer();
        if (now == nullptr || now->get() != *expected)
            return entry + "." + key + " would not be " + std::to_string(*expected);
        new_table->erase(key);
        if (old_table != nullptr)
            old_table->erase(key);
    }

    // Everything else reads as it did.
    for (const char* key : {COMMAND_KEY, ARGS_KEY}) {
        new_table->erase(key);
        if (old_table != nullptr)
            old_table->erase(key);
    }
    if (old_table == nullptr) {
        if (!new_table->empty())
            return entry + " would hold more than Connect writes";
        drop_server(*new_root, *old_root, server_name);
    }
    return difference_in_words(*old_root, *new_root);
}

std::string codex_disconnect_difference(const std::string& before, const std::string& after,
                                        const std::string& server_name)
{
    std::string                error;
    std::optional<toml::table> old_root = parse_toml(before, &error);
    if (!old_root)
        return "the file is not TOML (" + error + ")";
    std::optional<toml::table> new_root = parse_toml(after, &error);
    if (!new_root)
        return "the edited file would not parse (" + error + ")";
    if (server_node(*new_root, server_name) != nullptr)
        return std::string(SERVERS_KEY) + "." + server_name + " would still be defined";
    drop_server(*old_root, *new_root, server_name);
    return difference_in_words(*old_root, *new_root);
}

}}} // namespace Slic3r::GUI::OrcaMCP

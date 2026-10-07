#include "mcp-help.h"

#include "../../detail/module-path.h"
#include "mapget/log.h"

#include <picosha2.h>
#include <sqlite3.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mapget::detail
{
namespace fs = std::filesystem;
using Json = nlohmann::json;

McpHelp::McpHelp(fs::path bundle, std::vector<fs::path> extra)
    : bundle_(std::move(bundle)), extra_(std::move(extra))
{
    try {
        if (sqlite3_open_v2(
                ":memory:",
                &database_,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                nullptr) != SQLITE_OK)
            throw std::runtime_error("Cannot open MCP help index");
        execute(
            "CREATE VIRTUAL TABLE help USING fts5(title, keywords, content, source UNINDEXED, "
            "tokenize='unicode61')");
    }
    catch (...) {
        sqlite3_close(database_);
        throw;
    }
}

McpHelp::~McpHelp()
{
    sqlite3_close(database_);
}

fs::path McpHelp::defaultDirectory()
{
    static int anchor;
    return moduleDirectory(&anchor) / "mcp-help";
}

void McpHelp::execute(char const* sql)
{
    if (sqlite3_exec(database_, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database_));
}

McpHelp::Statement McpHelp::prepare(char const* sql)
{
    sqlite3_stmt* raw = nullptr;
    auto code = sqlite3_prepare_v2(database_, sql, -1, &raw, nullptr);
    Statement result(raw, sqlite3_finalize);
    if (code != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database_));
    return result;
}

void McpHelp::check(std::function<bool()> const& keepGoing)
{
    if (keepGoing && !keepGoing())
        throw std::length_error("MCP help cancelled or budget exceeded");
}

std::map<std::string, std::string> McpHelp::readDocuments(std::function<bool()> const& keepGoing)
{
    std::map<std::string, fs::path> roots;
    if (fs::is_directory(bundle_)) {
        for (auto const& entry : fs::directory_iterator(bundle_))
            if (entry.is_directory())
                roots.emplace(entry.path().filename().string(), entry.path());
    }
    // Only development builds ship this sibling registry. Installed bundles are relocatable
    // and cannot accidentally discover a developer's checkout from a compiled absolute path.
    auto registry = bundle_.parent_path() / ".mcp-help-sources.json";
    if (fs::exists(registry)) {
        if (fs::file_size(registry) > 64 * 1024)
            throw std::runtime_error("MCP help source registry exceeds 64 KiB");
        std::ifstream input(registry);
        auto sources = Json::parse(input);
        if (!sources.is_object() || sources.size() > 64)
            throw std::runtime_error("Invalid MCP help source registry");
        for (auto const& [name, path] : sources.items()) {
            auto source = fs::path(path.get<std::string>());
            if (name.empty() ||
                name.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                    std::string::npos)
                throw std::runtime_error("Invalid MCP help component name");
            // Choose one complete root: a deleted source file must not fall back to its bundle.
            if (fs::is_directory(source))
                roots[name] = std::move(source);
        }
    }
    std::vector<std::pair<std::string, fs::path>> ordered(roots.begin(), roots.end());
    for (size_t i = 0; i < extra_.size(); ++i)
        ordered.emplace_back("extra-" + std::to_string(i + 1), extra_[i]);

    std::map<std::string, std::string> documents;
    std::set<fs::path> seen;
    size_t bytes = 0;
    size_t visited = 0;
    for (auto const& [name, root] : ordered) {
        check(keepGoing);
        std::vector<fs::path> files;
        if (fs::is_directory(root)) {
            // Do not follow nested directory symlinks; an explicitly registered root may be one.
            for (auto const& entry : fs::recursive_directory_iterator(root)) {
                check(keepGoing);
                if (++visited > 100000)
                    throw std::runtime_error("MCP help folders exceed 100000 directory entries");
                if (!entry.is_symlink() && entry.is_regular_file() &&
                    entry.path().extension() == ".md")
                    files.push_back(entry.path());
            }
        }
        else if (fs::is_regular_file(root) && root.extension() == ".md")
            files.push_back(root);
        // Missing optional roots are scanned again, allowing a webapp help folder to appear later.
        std::sort(files.begin(), files.end());
        for (auto const& file : files) {
            check(keepGoing);
            if (!seen.insert(fs::canonical(file)).second)
                continue;
            if (documents.size() >= 4096 || fs::file_size(file) > 1024 * 1024)
                throw std::runtime_error("MCP help exceeds 4096 files or 1 MiB per file");
            std::ifstream input(file, std::ios::binary);
            if (!input)
                throw std::runtime_error("Cannot read MCP help file: " + file.string());
            std::string text;
            std::array<char, 16384> buffer;
            while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
                check(keepGoing);
                bytes += static_cast<size_t>(input.gcount());
                if (bytes > 16 * 1024 * 1024 || text.size() + input.gcount() > 1024 * 1024)
                    throw std::runtime_error("MCP help exceeds 16 MiB corpus or 1 MiB file limit");
                text.append(buffer.data(), static_cast<size_t>(input.gcount()));
            }
            if (input.bad() || text.find('\0') != std::string::npos)
                throw std::runtime_error("Unreadable/non-text MCP help file: " + file.string());
            auto relative = file == root ? file.filename() : file.lexically_relative(root);
            documents.emplace(name + "/" + relative.generic_string(), std::move(text));
        }
    }
    return documents;
}

void McpHelp::indexDocument(
    std::string const& source,
    std::string const& text,
    size_t& sectionBudget,
    std::function<bool()> const& keepGoing)
{
    auto insert = prepare("INSERT INTO help(title,keywords,content,source) VALUES(?1,?2,?3,?4)");
    std::vector<Section> active;
    std::vector<std::pair<size_t, std::string>> headings;
    std::optional<YAML::Node> pending;
    std::istringstream input(text);
    std::string line;
    size_t lineNumber = 0;
    char fence = 0;
    size_t fenceSize = 0;
    bool comment = false;
    auto error = [&](std::string const& message)
    {
        throw std::runtime_error(source + ":" + std::to_string(lineNumber) + ": " + message);
    };
    auto finish = [&]
    {
        if (!sectionBudget)
            error("MCP help exceeds 4096 indexed sections");
        --sectionBudget;
        auto& section = active.back();
        if (!section.hint.empty())
            section.content += "\nMCP guidance:\n" + section.hint + "\n";
        std::array<std::string const*, 4>
            values{&section.title, &section.keywords, &section.content, &source};
        sqlite3_reset(insert.get());
        for (size_t i = 0; i < values.size(); ++i)
            if (sqlite3_bind_text(
                    insert.get(),
                    static_cast<int>(i + 1),
                    values[i]->c_str(),
                    static_cast<int>(values[i]->size()),
                    SQLITE_TRANSIENT) != SQLITE_OK)
                throw std::runtime_error(sqlite3_errmsg(database_));
        if (sqlite3_step(insert.get()) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(database_));
        active.pop_back();
    };
    while (std::getline(input, line)) {
        ++lineNumber;
        check(keepGoing);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        auto first = line.find_first_not_of(" ");
        auto visible = first == std::string::npos ?
            std::string_view{} :
            std::string_view(line).substr(first);
        // A fence closes only with the same marker, sufficient length and no trailing content.
        if (!comment && first <= 3 && !visible.empty() &&
            (visible.front() == '`' || visible.front() == '~'))
        {
            size_t count = visible.find_first_not_of(visible.front());
            if (count == std::string_view::npos)
                count = visible.size();
            if (fence) {
                for (auto& section : active)
                    section.content += line + "\n";
                if (visible.front() == fence && count >= fenceSize &&
                    visible.substr(count).find_first_not_of(" \t") == std::string_view::npos)
                    fence = 0;
                continue;
            }
            if (count >= 3) {
                if (pending)
                    error("MCP annotation must precede a heading");
                fence = visible.front();
                fenceSize = count;
            }
        }
        if (fence) {
            for (auto& section : active)
                section.content += line + "\n";
            continue;
        }
        if (!comment && first <= 3 && visible.starts_with("<!-- mcp:")) {
            if (pending)
                error("MCP annotation has no heading");
            auto annotation = std::string(visible.substr(9));
            while (annotation.find("-->") == std::string::npos && std::getline(input, line)) {
                ++lineNumber;
                check(keepGoing);
                annotation += "\n" + line;
            }
            auto end = annotation.find("-->");
            if (end == std::string::npos ||
                annotation.substr(end + 3).find_first_not_of(" \t\r") != std::string::npos)
                error("Unterminated MCP annotation or content after its closing marker");
            try {
                auto metadata = YAML::Load(annotation.substr(0, end));
                if (!metadata.IsNull() && !metadata.IsMap())
                    error("MCP annotation must be a YAML mapping");
                std::set<std::string> keys;
                for (auto const& field : metadata) {
                    auto key = field.first.as<std::string>();
                    if ((key != "title" && key != "keywords" && key != "hint") ||
                        !keys.insert(key).second)
                        error("Unknown or repeated MCP annotation field: " + key);
                    if (key == "keywords") {
                        if (!field.second.IsSequence())
                            error("MCP keywords must be a string list");
                        for (auto const& keyword : field.second)
                            if (!keyword.IsScalar())
                                error("MCP keyword must be a string");
                    }
                    else if (!field.second.IsScalar())
                        error("MCP title/hint must be a string");
                }
                pending = metadata;
            }
            catch (YAML::Exception const& e) {
                error(std::string("Invalid MCP annotation: ") + e.what());
            }
            continue;
        }
        // Ordinary HTML comments and portal include directives are not useful help content.
        for (size_t pos = 0; pos < line.size();) {
            if (comment) {
                auto end = line.find("-->", pos);
                line.erase(pos, end == std::string::npos ? std::string::npos : end + 3 - pos);
                if (end == std::string::npos)
                    break;
                comment = false;
            }
            else {
                // Inline code may document the annotation syntax itself; it is not an HTML comment.
                if (line[pos] == '`') {
                    auto end = line.find_first_not_of('`', pos);
                    auto width = (end == std::string::npos ? line.size() : end) - pos;
                    auto close = line.find(std::string(width, '`'), pos + width);
                    pos = close == std::string::npos ? pos + width : close + width;
                }
                else if (line.compare(pos, 4, "<!--") == 0)
                    comment = true;
                else
                    ++pos;
            }
        }
        first = line.find_first_not_of(" ");
        visible = first == std::string::npos ?
            std::string_view{} :
            std::string_view(line).substr(first);
        auto level = visible.find_first_not_of('#');
        bool heading = first <= 3 && level >= 1 && level <= 6 && level < visible.size() &&
            (visible[level] == ' ' || visible[level] == '\t');
        if (heading) {
            while (!active.empty() && active.back().level >= level)
                finish();
            auto title = std::string(visible.substr(level + 1));
            auto end = title.find_last_not_of(" \t");
            title = end == std::string::npos ? "" : title.substr(0, end + 1);
            auto hashes = title.find_last_not_of('#');
            if (hashes != std::string::npos && hashes + 1 < title.size() &&
                std::isspace(static_cast<unsigned char>(title[hashes])))
                title.erase(hashes);
            end = title.find_last_not_of(" \t");
            title = end == std::string::npos ? "" : title.substr(0, end + 1);
            auto start = title.find_first_not_of(" \t");
            title = start == std::string::npos ? "" : title.substr(start);
            while (!headings.empty() && headings.back().first >= level)
                headings.pop_back();
            headings.emplace_back(level, title);
            if (pending) {
                auto qualified = source;
                for (auto const& [_, ancestor] : headings)
                    qualified += " / " + ancestor;
                if ((*pending)["title"]) {
                    auto override = (*pending)["title"].as<std::string>();
                    if (override.find_first_not_of(" \t\r\n") == std::string::npos)
                        error("MCP title must not be empty");
                    qualified = source + " / " + override;
                }
                std::string keywords;
                for (auto const& keyword : (*pending)["keywords"])
                    keywords += keyword.as<std::string>() + " ";
                if (title.empty() || qualified.size() > 2048 || keywords.size() > 4096)
                    error("MCP heading/title/keywords are empty or too large");
                auto hint = (*pending)["hint"] ?
                    (*pending)["hint"].as<std::string>() :
                    std::string{};
                if (hint.size() > 16384)
                    error("MCP hint exceeds 16 KiB");
                active.push_back(
                    {level, std::move(qualified), std::move(keywords), std::move(hint), {}});
                pending.reset();
            }
        }
        else if (pending && line.find_first_not_of(" \t\r") != std::string::npos)
            error("MCP annotation must precede an ATX heading");
        if (!visible.starts_with("--8<--"))
            for (auto& section : active)
                section.content += line + "\n";
    }
    if (pending)
        error("MCP annotation has no heading");
    while (!active.empty())
        finish();
}

void McpHelp::refreshLocked(std::function<bool()> const& keepGoing)
{
    try {
        auto documents = readDocuments(keepGoing);
        picosha2::hash256_one_by_one hash;
        std::string const separator(1, '\0');
        for (auto const& [source, content] : documents) {
            hash.process(source.begin(), source.end());
            hash.process(separator.begin(), separator.end());
            hash.process(content.begin(), content.end());
            hash.process(separator.begin(), separator.end());
        }
        hash.finish();
        auto next = picosha2::get_hash_hex_string(hash);
        if (next != revision_) {
            execute("BEGIN");
            try {
                execute("DELETE FROM help");
                size_t sectionBudget = 4096;
                for (auto const& [source, content] : documents)
                    indexDocument(source, content, sectionBudget, keepGoing);
                auto duplicate =
                    prepare("SELECT title FROM help GROUP BY title HAVING count(*) > 1 LIMIT 1");
                if (sqlite3_step(duplicate.get()) == SQLITE_ROW)
                    throw std::runtime_error(
                        "Duplicate MCP help title: " +
                        std::string(reinterpret_cast<char const*>(
                            sqlite3_column_text(duplicate.get(), 0))));
                execute("COMMIT");
            }
            catch (...) {
                sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
                throw;
            }
            revision_ = std::move(next);
            log().info(
                "Reloaded MCP help ({} Markdown files, revision {}).",
                documents.size(),
                revision_.substr(0, 12));
        }
        error_.clear();
    }
    catch (std::length_error const&) {
        throw;  // Invocation cancellation/budgets must not become a persistent corpus error.
    }
    catch (std::exception const& e) {
        if (error_ != e.what())
            log().warn("MCP help refresh failed; retaining the last valid index: {}", e.what());
        error_ = e.what();
    }
}

void McpHelp::refresh()
{
    std::lock_guard lock(mutex_);
    refreshLocked({});
}

std::string McpHelp::matchExpression(std::string const& text)
{
    std::set<std::string> terms;
    std::string word;
    auto flush = [&]
    {
        if (!word.empty())
            terms.insert(std::exchange(word, {}));
    };
    for (unsigned char c : text) {
        if (std::isalnum(c) || c >= 128)
            word += static_cast<char>(std::tolower(c));
        else
            flush();
    }
    flush();
    auto useful = terms;
    for (auto stop :
         {"a",
          "an",
          "and",
          "are",
          "for",
          "how",
          "i",
          "in",
          "is",
          "of",
          "or",
          "the",
          "to",
          "what",
          "with"})
        useful.erase(stop);
    if (!useful.empty())
        terms = std::move(useful);
    std::string result;
    size_t count = 0;
    for (auto const& term : terms) {
        if (++count > 32)
            break;
        if (!result.empty())
            result += " OR ";
        result += '"' + term + '"';
    }
    return result;
}

Json McpHelp::query(
    std::string const& text,
    std::string const& title,
    size_t limit,
    std::function<bool()> const& keepGoing)
{
    if (text.size() > 4096 || title.size() > 2048 || (!text.empty() && !title.empty()))
        throw std::invalid_argument("Expected a help query or exact returned title");
    std::lock_guard lock(mutex_);
    refreshLocked(keepGoing);
    Json result{
        {"items", Json::array()},
        {"revision", revision_.empty() ? Json(nullptr) : Json(revision_)},
        {"complete", error_.empty()},
        {"reason", error_.empty() ? Json(nullptr) : Json("docs_reload_failed")},
        {"issues", Json::array()},
        {"traces", Json::object()}};
    if (!error_.empty())
        result["issues"].push_back(
            {{"message",
              "Documentation refresh failed; results use the last valid corpus. See protected "
              "server logs."}});
    auto expression = matchExpression(text);
    if (!text.empty() && expression.empty())
        return result;
    auto statement = prepare(
        !title.empty() ?
            "SELECT title,content,source FROM help WHERE title = ?1 ORDER BY title LIMIT ?2" :
            !text.empty() ?
            "SELECT title,content,source FROM help WHERE help MATCH ?1 ORDER BY "
            "bm25(help,8.0,4.0,1.0),title LIMIT ?2" :
            "SELECT title,content,source FROM help ORDER BY title LIMIT ?2");
    auto const& parameter = title.empty() ? expression : title;
    sqlite3_bind_text(
        statement.get(),
        1,
        parameter.c_str(),
        static_cast<int>(parameter.size()),
        SQLITE_TRANSIENT);
    sqlite3_bind_int(statement.get(), 2, static_cast<int>(std::min<size_t>(limit, 8)));
    int code;
    while ((code = sqlite3_step(statement.get())) == SQLITE_ROW) {
        check(keepGoing);
        Json item{
            {"title", reinterpret_cast<char const*>(sqlite3_column_text(statement.get(), 0))}};
        if (result["items"].size() < 3) {
            item["content"] =
                reinterpret_cast<char const*>(sqlite3_column_text(statement.get(), 1));
            item["source"] = reinterpret_cast<char const*>(sqlite3_column_text(statement.get(), 2));
        }
        result["items"].push_back(std::move(item));
    }
    if (code != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(database_));
    return result;
}

}  // namespace mapget::detail

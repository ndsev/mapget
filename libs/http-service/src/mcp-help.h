#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace mapget::detail
{

/** Source-backed Markdown sections and their disposable, mutex-protected FTS5 index. */
class McpHelp
{
public:
    /** Prefer registered checkout folders over bundles; extra folders/files are additive. */
    McpHelp(std::filesystem::path bundle, std::vector<std::filesystem::path> extra = {});
    /** Close the index after its owning native-tool service has drained worker callbacks. */
    ~McpHelp();
    McpHelp(McpHelp const&) = delete;
    McpHelp& operator=(McpHelp const&) = delete;

    /** Find the relocatable bundle beside this binary module, not the Python interpreter. */
    static std::filesystem::path defaultDirectory();
    /** Reconcile source additions/edits/deletions; failed refreshes retain the last transaction. */
    void refresh();
    /** Refresh before searching; return three full sections and up to five title suggestions. */
    nlohmann::json query(
        std::string const& text,
        std::string const& title,
        size_t limit,
        std::function<bool()> const& keepGoing = {});

private:
    using Statement = std::unique_ptr<sqlite3_stmt, int (*)(sqlite3_stmt*)>;
    /** One annotated heading, including independently annotated subsections in its Markdown. */
    struct Section
    {
        size_t level;
        std::string title;
        std::string keywords;
        std::string hint;
        std::string content;
    };

    std::filesystem::path bundle_;
    std::vector<std::filesystem::path> extra_;
    std::mutex mutex_;
    sqlite3* database_ = nullptr;
    std::string revision_;
    std::string error_;

    /** Collect bounded, deterministic portable filenames and content, deduplicating real files. */
    std::map<std::string, std::string> readDocuments(std::function<bool()> const& keepGoing);
    /** Parse annotated ATX sections, ignoring Markdown fences and ordinary HTML comments. */
    void indexDocument(
        std::string const& source,
        std::string const& text,
        size_t& sectionBudget,
        std::function<bool()> const& keepGoing);
    /** Replace rows in one transaction only when the corpus fingerprint changes; mutex held. */
    void refreshLocked(std::function<bool()> const& keepGoing);
    /** Run internal SQL and surface failures without leaving a partially rebuilt index. */
    void execute(char const* sql);
    /** Own a prepared statement through all parse/search error paths. */
    Statement prepare(char const* sql);
    /** Cooperatively stop document scanning and parsing on invocation cancellation/budgets. */
    static void check(std::function<bool()> const& keepGoing);
    /** Convert plain words into quoted OR terms, never exposing the FTS query language. */
    static std::string matchExpression(std::string const& text);
};

}  // namespace mapget::detail

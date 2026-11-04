#include "../../libs/http-service/src/mcp-help.h"

#include <drogon/utils/Utilities.h>
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <future>

namespace
{
namespace fs = std::filesystem;
using Json = nlohmann::json;

/** Isolate source and installed roots without scanning the developer's documentation. */
class HelpFixture
{
public:
    fs::path root = fs::temp_directory_path() / ("mapget-help-" + drogon::utils::getUuid());
    mapget::detail::McpHelp help{root / "bundle", {root / "extra"}};

    /** Create only this test's sandbox; optional help roots can appear later. */
    HelpFixture() { fs::create_directories(root); }
    /** Remove all generated Markdown and registry files after concurrent readers have joined. */
    ~HelpFixture()
    {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    /** Replace one source file without touching CMake or restarting the index. */
    void write(std::string const& path, std::string const& text)
    {
        fs::create_directories((root / path).parent_path());
        std::ofstream(root / path, std::ios::binary) << text;
    }
    /** Query the default eight ranked documents and expose the complete response contract. */
    Json query(std::string text = {}) { return help.query(text, {}, 8); }
};
}  // namespace

TEST_CASE("MCP help extracts annotated sections, hints and fenced examples", "[mcp-help]")
{
    HelpFixture f;
    f.write("extra/guide.md", R"md(# Language
Unindexed introduction.
<!-- mcp:
keywords: [cardinality, counting]
hint: Use cardinality for array size.
-->
## Arrays
Use `#items`.
Inline `<!-- mcp: -->` and `` `<!-- example -->` `` are literal.
<!-- hidden ordinary comment -->
### Examples
```markdown
## This is not a section
<!-- mcp: -->
```
--8<-- "missing-include.md"
## Not indexed
This must not leak into the previous section.
)md");
    auto response = f.query("cardinality");
    REQUIRE(response["complete"] == true);
    REQUIRE(response["items"].size() == 1);
    auto const& item = response["items"][0];
    CHECK(item["title"] == "extra-1/guide.md / Language / Arrays");
    auto content = item["content"].get<std::string>();
    CHECK(content.find("### Examples") != std::string::npos);
    CHECK(content.find("## This is not a section\n<!-- mcp: -->") != std::string::npos);
    CHECK(content.find("MCP guidance:\nUse cardinality") != std::string::npos);
    CHECK(content.find("ordinary comment") == std::string::npos);
    CHECK(content.find("Inline `<!-- mcp: -->`") != std::string::npos);
    CHECK(content.find("missing-include") == std::string::npos);
    CHECK(content.find("must not leak") == std::string::npos);
    CHECK(item["source"] == "extra-1/guide.md");
    auto guessed = f.help.query({}, "Arrays", 8);
    CHECK(guessed["items"].empty());
    REQUIRE(guessed["issues"].size() == 1);
    CHECK(guessed["issues"][0]["message"].get<std::string>().find("query") != std::string::npos);
    auto exact = f.help.query({}, item["title"], 8);
    CHECK(exact == response);
}

TEST_CASE(
    "MCP help returns three full hits then five titles, ranked by title and keywords",
    "[mcp-help]")
{
    HelpFixture f;
    f.write("extra/00.md", "<!-- mcp: -->\n# Cardinality\nUseful reference.\n");
    f.write(
        "extra/01.md",
        "<!-- mcp:\nkeywords: [cardinality]\n-->\n# Counting\nUseful reference.\n");
    for (size_t i = 2; i < 12; ++i)
        f.write(
            "extra/" + std::to_string(i) + ".md",
            "<!-- mcp: -->\n# Topic\nUseful cardinality reference.\n");
    auto response = f.query("How do I use cardinality?");
    REQUIRE(response["items"].size() == 8);
    CHECK(response["complete"] == false);
    CHECK(response["nextOffset"] == 8);
    auto next = f.help.query("How do I use cardinality?", {}, 8, {}, "extra-1", 8);
    CHECK(next["complete"] == true);
    CHECK(next["items"].size() == 4);
    CHECK(f.help.query("cardinality", {}, 8, {}, "missing-component")["items"].empty());
    CHECK(response["items"][0]["title"] == "extra-1/00.md / Cardinality");
    CHECK(response["items"][1]["title"] == "extra-1/01.md / Counting");
    for (size_t i = 0; i < 8; ++i) {
        CHECK(response["items"][i].contains("content") == (i < 3));
        CHECK(response["items"][i].contains("source") == (i < 3));
    }
    CHECK(f.help.query({}, response["items"][7]["title"], 8)["items"][0].contains("content"));
    CHECK(f.query("\" OR * (NEAR:) cardinality")["items"].size() == 8);
    CHECK(f.query("\" * ^ :")["items"].empty());
    CHECK(f.help.query({}, "does not exist", 8)["items"].empty());
    CHECK_THROWS_AS(f.help.query("query", "title", 8), std::invalid_argument);
}

TEST_CASE("MCP help component filters separate similar domain terms", "[mcp-help]")
{
    HelpFixture f;
    f.write(
        "bundle/classicsource/guide.md",
        "<!-- mcp: -->\n# Lane validity\nClassic range masks.\n");
    f.write("bundle/livesource/guide.md", "<!-- mcp: -->\n# Lane validity\nLive lane ranges.\n");
    auto classic = f.help.query("lane validity", {}, 8, {}, "classicsource");
    REQUIRE(classic["complete"] == true);
    REQUIRE(classic["items"].size() == 1);
    CHECK(classic["items"][0]["source"] == "classicsource/guide.md");
    auto title = classic["items"][0]["title"].get<std::string>();
    CHECK(f.help.query({}, title, 8, {}, "classicsource")["items"].size() == 1);
    CHECK(f.help.query({}, title, 8, {}, "livesource")["items"].empty());
    CHECK(f.help.query({}, {}, 8, {}, "livesource")["items"][0]["source"] == "livesource/guide.md");
}

TEST_CASE(
    "MCP help ignores conversational filler without discarding all-filler queries",
    "[mcp-help]")
{
    HelpFixture f;
    f.write("extra/labels.md", "<!-- mcp: -->\n# Labels\nFeature labels.\n");
    f.write("extra/filler.md", "<!-- mcp: -->\n# Put on\nPut this on that.\n");
    auto result = f.query("can you please put labels on these features for me");
    REQUIRE(result["items"].size() == 1);
    CHECK(result["items"][0]["source"] == "extra-1/labels.md");
    CHECK_FALSE(f.query("put on")["items"].empty());
}

TEST_CASE(
    "MCP help sees edits, new nested files, renames and removals without reconfiguration",
    "[mcp-help]")
{
    HelpFixture f;
    auto empty = f.query();
    f.write("extra/guide.md", "<!-- mcp: -->\n# First\nAlpha.\n");
    auto first = f.query();
    CHECK(first["revision"] != empty["revision"]);
    auto timestamp = fs::last_write_time(f.root / "extra/guide.md");
    f.write("extra/guide.md", "<!-- mcp: -->\n# First\nBravo.\n");
    fs::last_write_time(f.root / "extra/guide.md", timestamp);
    auto edited = f.query();
    CHECK(edited["revision"] != first["revision"]);
    CHECK(edited["items"][0]["content"].get<std::string>().find("Bravo") != std::string::npos);
    f.write("extra/nested/new.md", "<!-- mcp: -->\n# Second\nAnother example.\n");
    CHECK(f.query()["items"].size() == 2);
    fs::rename(f.root / "extra/nested/new.md", f.root / "extra/nested/renamed.md");
    CHECK(f.query("Second")["items"][0]["source"] == "extra-1/nested/renamed.md");
    fs::remove(f.root / "extra/guide.md");
    CHECK(f.query()["items"].size() == 1);
    fs::remove_all(f.root / "extra");
    CHECK(f.query()["items"].empty());
}

TEST_CASE("MCP source roots replace bundles instead of resurrecting deleted snippets", "[mcp-help]")
{
    HelpFixture f;
    f.write("bundle/simfil/guide.md", "<!-- mcp: -->\n# Packaged\nOld content.\n");
    f.write("checkout/guide.md", "<!-- mcp: -->\n# Checkout\nNew content.\n");
    f.write(".mcp-help-sources.json", Json{{"simfil", (f.root / "checkout").string()}}.dump());
    auto response = f.query();
    REQUIRE(response["items"].size() == 1);
    CHECK(response["items"][0]["title"] == "simfil/guide.md / Checkout");
    fs::remove(f.root / "checkout/guide.md");
    CHECK(f.query()["items"].empty());
    fs::remove(f.root / ".mcp-help-sources.json");
    CHECK(f.query()["items"][0]["title"] == "simfil/guide.md / Packaged");
    // Explicit overlapping roots must not index the same physical file twice.
    mapget::detail::McpHelp
        overlap(f.root / "bundle", {f.root / "bundle/simfil", f.root / "bundle/simfil/guide.md"});
    CHECK(overlap.query({}, {}, 8)["items"].size() == 1);
}

TEST_CASE("MCP help rolls back malformed annotations and duplicate titles", "[mcp-help]")
{
    HelpFixture f;
    f.write("extra/guide.md", "<!-- mcp: -->\n# Stable\nValid content.\n");
    auto good = f.query();
    for (auto const& invalid :
         {"<!-- mcp:\nkeywords: scalar\n-->\n# Broken",
          "<!-- mcp: -->\nno heading",
          "<!-- mcp: [unterminated",
          "<!-- mcp:\ntypo: value\n-->\n# Broken",
          "<!-- mcp:\ntitle: ''\n-->\n# Broken",
          "<!-- mcp:\ntitle: Repeated\n-->\n# One\n<!-- mcp:\ntitle: Repeated\n-->\n# Two"})
    {
        f.write("extra/guide.md", invalid);
        auto failed = f.query();
        CHECK(failed["revision"] == good["revision"]);
        CHECK(failed["items"] == good["items"]);
        CHECK(failed["complete"] == false);
        CHECK(failed["reason"] == "docs_reload_failed");
        CHECK_FALSE(failed["issues"].empty());
    }
    f.write("extra/guide.md", "<!-- mcp: -->\n# Repaired\nValid again.\n");
    auto repaired = f.query();
    CHECK(repaired["complete"] == true);
    CHECK(repaired["revision"] != good["revision"]);
    CHECK(repaired["issues"].empty());
}

TEST_CASE("MCP help handles nested sections, CRLF, Unicode and tilde fences", "[mcp-help]")
{
    HelpFixture f;
    f.write(
        "extra/guide.md",
        "<!-- mcp: -->\r\n# Parent\r\n<!-- mcp:\ntitle: Child overview\n-->\n## "
        "Child\nM\xc3\xbcnchen.\n~~~md\n# Example\n<!-- mcp: -->\n~~~\n# End\n");
    auto response = f.query("M\xc3\xbcnchen");
    REQUIRE(response["items"].size() == 2);
    for (auto const& item : response["items"]) {
        CHECK(item["content"].get<std::string>().find("# Example") != std::string::npos);
        CHECK(item["content"].get<std::string>().find("# End") == std::string::npos);
    }
    CHECK(f.help.query({}, "extra-1/guide.md / Child overview", 8)["items"].size() == 1);
}

TEST_CASE(
    "MCP help cancellation does not poison its index and readers serialize safely",
    "[mcp-help]")
{
    HelpFixture f;
    f.write("extra/guide.md", "<!-- mcp: -->\n# Reference\nRead me.\n");
    auto good = f.query();
    f.write("extra/guide.md", "<!-- mcp: -->\n# Reference\nUpdated.\n");
    size_t steps = 0;
    CHECK_THROWS_AS(f.help.query({}, {}, 8, [&] { return ++steps < 6; }), std::length_error);
    std::vector<std::future<Json>> readers;
    for (size_t i = 0; i < 8; ++i)
        readers.push_back(std::async(
            std::launch::async,
            [&]
            {
                f.help.refresh();
                return f.query();
            }));
    auto result = f.query();
    CHECK(result["complete"] == true);
    CHECK(result["revision"] != good["revision"]);
    for (auto& reader : readers)
        CHECK(reader.get() == result);
}

TEST_CASE("MCP help bounds both input bytes and tiny-section floods", "[mcp-help]")
{
    HelpFixture f;
    f.write("extra/good.md", "<!-- mcp: -->\n# Stable\nKeep me.\n");
    auto good = f.query();
    f.write("extra/large.md", std::string(1024 * 1024 + 1, 'x'));
    CHECK(f.query()["items"] == good["items"]);
    CHECK(f.query()["reason"] == "docs_reload_failed");
    fs::remove(f.root / "extra/large.md");
    std::string flood;
    for (size_t i = 0; i < 4097; ++i)
        flood += "<!-- mcp: -->\n# Section " + std::to_string(i) + "\n";
    f.write("extra/flood.md", flood);
    auto failed = f.query();
    CHECK(failed["items"] == good["items"]);
    CHECK(failed["revision"] == good["revision"]);
    CHECK(failed["reason"] == "docs_reload_failed");
}

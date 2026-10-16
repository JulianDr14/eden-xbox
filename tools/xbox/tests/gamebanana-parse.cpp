// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Build with src/jadyx/mods/gamebanana.cpp, src/jadyx/mods/cnx_index.cpp and nlohmann_json's
// include folder. The JSON below is real API answers, cut down to a few records.
#include <cstdio>
#include <cstdlib>
#include <string>
#include "jadyx/mods/cnx_index.h"
#include "jadyx/mods/gamebanana.h"

namespace {
using namespace Jadyx::Mods;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        std::fflush(stdout);
        std::abort();
    }
}

constexpr std::string_view Games = R"({
    "_aMetadata": {"_nRecordCount": 232, "_nPerpage": 15, "_bIsComplete": false},
    "_aRecords": [
        {"_idRow": 6376, "_sModelName": "Game", "_sName": "Platform Quest Odyssey", "_sInitialVisibility": "show"},
        {"_idRow": 18381, "_sModelName": "Game", "_sName": "Platform Quest Land"},
        {"_idRow": 99, "_sModelName": "Mod", "_sName": "Not a game"}
    ]})";

constexpr std::string_view List = R"({
    "_aMetadata": {"_nRecordCount": 634, "_bIsComplete": false, "_nPerpage": 16},
    "_aRecords": [
        {"_idRow": 49049, "_sModelName": "Mod", "_sName": "100% Save File",
         "_tsDateAdded": 1599231491, "_tsDateModified": 1599231491,
         "_aPreviewMedia": {"_aImages": [{"_sType": "screenshot",
             "_sBaseUrl": "https:\/\/images.gamebanana.com\/img\/ss\/mods",
             "_sFile": "5f5255bf19193.jpg", "_sFile220": "220-90_5f5255bf19193.jpg"}]},
         "_aSubmitter": {"_idRow": 1746251, "_sName": "Modder64"},
         "_aRootCategory": {"_sName": "Game files"},
         "_sVersion": "", "_sInitialVisibility": "show", "_bHasContentRatings": false,
         "_nLikeCount": 28, "_nViewCount": "131824"},
        {"_idRow": 453408, "_sName": "Rated", "_sInitialVisibility": "hide", "_bHasContentRatings": true},
        {"_idRow": 455219, "_sName": "Warned", "_sInitialVisibility": "warn"},
        {"_idRow": 450017, "_sName": "Mild", "_sInitialVisibility": "show", "_bHasContentRatings": true},
        {"_idRow": 0, "_sName": "No id"}
    ]})";

constexpr std::string_view Detail = R"({
    "_idRow": 384214, "_sName": "Online Mod", "_sVersion": "1.0",
    "_tsDateAdded": 1655528545, "_tsDateModified": 1655529920,
    "_nDownloadCount": 27754, "_nLikeCount": 192, "_nViewCount": 307290,
    "_sInitialVisibility": "show",
    "_sDescription": "Online multiplayer",
    "_sText": "<br><br>Welcome to the &quot;official&quot; page!<h1>Features</h1><hr><br><ul><li>Explore together</li><li>Play &amp; race</li></ul><script>alert(1)</script><p>Bye&#33;&nbsp;&#x263A;</p>",
    "_aCategory": {"_idRow": 4132, "_sName": "Game files"},
    "_aSubmitter": {"_sName": "Crafty"},
    "_aPreviewMedia": {"_aImages": [
        {"_sBaseUrl": "https://images.gamebanana.com/img/ss/mods", "_sFile": "a.jpg", "_sFile530": "530-90_a.jpg", "_sFile220": "220-90_a.jpg"},
        {"_sBaseUrl": "https://images.gamebanana.com/img/ss/mods", "_sFile": "b.jpg", "_sFile100": "100-90_b.jpg"}]},
    "_aFiles": [
        {"_idRow": 815939, "_sFile": "releasebuild1.zip", "_nFilesize": 872913, "_tsDateAdded": 1655528236,
         "_nDownloadCount": 27885, "_sDownloadUrl": "https://gamebanana.com/dl/815939",
         "_sMd5Checksum": "119778AF93424ebd6023ecbbec821fbf", "_sAnalysisState": "done",
         "_sAnalysisResult": "ok", "_sAvState": "done", "_sAvResult": "clean", "_bIsArchived": false},
        {"_idRow": 815990, "_sFile": "newer.7z", "_nFilesize": 1000, "_tsDateAdded": 1655600000,
         "_sDownloadUrl": "https://gamebanana.com/dl/815990", "_sMd5Checksum": "bad",
         "_sAnalysisResult": "ok", "_sAvResult": "pending"}
    ]})";

// Mod/Categories: an array (trimmed); empty categories are left out.
constexpr std::string_view Categories = R"([
    {"_idRow": 10527, "_sName": "Skins", "_nItemCount": 2970, "_nCategoryCount": 5, "_bIsObsolete": false},
    {"_idRow": 26513, "_sName": "Code Patches", "_nItemCount": "105", "_nCategoryCount": 0},
    {"_idRow": 36000, "_sName": "Empty", "_nItemCount": 0, "_nCategoryCount": 0},
    {"_sName": "No id", "_nItemCount": 3}
])";

constexpr std::string_view CnxIndex = R"({
    "translations": {
        "One Two": {"enabled": true, "link": "https://github.com/x/releases/download/a/0100000000ABC000.zip",
                    "size": "53.7 MB", "installed": {"hash_file": "0100000000ABC000/romfs/a.bin"}},
        "Other": {"enabled": true, "link": "https://github.com/x/releases/download/b/0100000000DEF000.zip",
                  "size": "622 KB"},
        "Off": {"enabled": false, "link": "https://github.com/x/releases/download/c/0100000000ABC000.zip"}
    },
    "modifications": {
        "Patch": {"enabled": true, "link": "https://github.com/x/releases/download/d/patch.zip",
                  "size": "1.08 GB", "installed": {"hash_file": "0100000000abc000/exefs/main"}},
        "Plain": {"enabled": true, "link": "http://example.com/0100000000ABC000.zip"}
    },
    "GMPACK-MOTD": {"enabled": true}
})";

} // namespace

int main() {
    namespace GB = GameBanana;
    // URLs.
    Check(GB::PercentEncode("a b&ñ") == "a%20b%26%C3%B1", "percent encoding");
    Check(GB::ModListUrl(6376, GB::Sort::Liked, 2, "60 fps") ==
              "https://gamebanana.com/apiv11/Mod/Index?_nPage=2&_nPerpage=16&_aFilters%5BGeneric_Game%5D=6376"
              "&_sSort=Generic_MostLiked&_aFilters%5BGeneric_Name%5D=contains,60%20fps",
          "list url");
    Check(GB::ModListUrl(6376, GB::Sort::Popular, 1, "", 10527) ==
              "https://gamebanana.com/apiv11/Mod/Index?_nPage=1&_nPerpage=16&_aFilters%5BGeneric_Game%5D=6376"
              "&_sSort=Generic_MostDownloaded&_aFilters%5BGeneric_Category%5D=10527",
          "category url");
    Check(GB::ModListUrl(6376, GB::Sort::Newest, 1, "", 0, "st").ends_with(
              "&_sSort=Generic_Newest&_aFilters%5BGeneric_ContentRatings%5D=st"),
          "rating url");
    Check(GB::CategoriesUrl(6376, 0) ==
              "https://gamebanana.com/apiv11/Mod/Categories?_sSort=a_to_z&_bShowEmpty=false&_idGameRow=6376",
          "top categories url");
    Check(GB::CategoriesUrl(6376, 10527).ends_with("&_idCategoryRow=10527"), "subcategories url");
    Check(GB::ModDetailUrl(5) == "https://gamebanana.com/apiv11/Mod/5/ProfilePage", "detail url");

    // Categories.
    const auto categories = GB::ParseCategories(Categories);
    Check(categories && categories->size() == 2, "categories parsed, empty and nameless skipped");
    Check((*categories)[0].name == "Skins" && (*categories)[0].nested && (*categories)[0].mods == 2970,
          "nested category");
    Check((*categories)[1].mods == 105 && !(*categories)[1].nested, "count given as text");
    Check(!GB::ParseCategories(R"({"_sErrorCode":"INPUT_ERRORS"})"), "categories error refused");

    // Games and picking one.
    const auto games = GB::ParseGames(Games);
    Check(games && games->size() == 2 && (*games)[0].id == 6376, "games parsed, mods skipped");
    Check(GB::BestGame(*games, "Platform Quest\xE2\x84\xA2 Odyssey") == 0u, "trademark sign ignored");
    Check(GB::BestGame(*games, "platform quest: land") == 1u, "punctuation ignored");
    Check(!GB::BestGame(*games, "Platform Quest"), "two equal prefixes: ask the player");
    Check(!GB::BestGame(*games, "Something Else"), "no match");
    Check(!GB::ParseGames(R"({"_sErrorCode":"INPUT_ERRORS"})"), "API error refused");
    Check(!GB::ParseGames("<html>"), "not JSON refused");

    // A page of mods.
    const auto list = GB::ParseModList(List);
    Check(list && list->mods.size() == 4, "list parsed, record without id skipped");
    const auto& first = list->mods[0];
    Check(first.name == "100% Save File" && first.author == "Modder64" && first.category == "Game files",
          "summary fields");
    Check(first.thumbnail == "https://images.gamebanana.com/img/ss/mods/220-90_5f5255bf19193.jpg",
          "thumbnail at 220 px");
    Check(first.likes == 28 && first.views == 131824, "counts, one given as text");
    Check(!first.sensitive && list->mods[1].sensitive && list->mods[2].sensitive && !list->mods[3].sensitive,
          "hidden and warned mods are sensitive; mild ratings are not");
    Check(list->total == 634 && list->complete, "a short page is the last");

    // Lists merged: each mod once, in the order asked for.
    GB::ModList a, b;
    a.mods = {{.id = 1, .likes = 5, .views = 10, .added = 300}, {.id = 2, .likes = 9, .views = 50, .added = 100}};
    b.mods = {{.id = 2, .likes = 9, .views = 50, .added = 100}, {.id = 3, .likes = 1, .views = 30, .added = 200}};
    const auto ids = [](const std::vector<GB::ModSummary>& mods) {
        std::vector<std::int64_t> out;
        for (const auto& mod : mods) out.push_back(mod.id);
        return out;
    };
    Check(ids(GB::MergeLists({a, b}, GB::Sort::Popular)) == std::vector<std::int64_t>{2, 3, 1}, "merged by views");
    Check(ids(GB::MergeLists({a, b}, GB::Sort::Newest)) == std::vector<std::int64_t>{1, 3, 2}, "merged by date");

    // One mod.
    const auto detail = GB::ParseModDetail(Detail);
    Check(detail.has_value(), "detail parsed");
    Check(detail->summary.name == "Online Mod" && detail->summary.category == "Game files" &&
              detail->downloads == 27754,
          "detail fields");
    Check(detail->images.size() == 2 &&
              detail->images[0] == "https://images.gamebanana.com/img/ss/mods/530-90_a.jpg" &&
              detail->images[1] == "https://images.gamebanana.com/img/ss/mods/b.jpg",
          "pictures at 530 px, else the full one");
    Check(detail->files.size() == 2 && detail->files[0].name == "newer.7z", "newest file first");
    Check(!detail->files[0].safe && detail->files[0].md5.empty(), "pending antivirus is not safe; bad md5 dropped");
    Check(detail->files[1].safe && detail->files[1].md5 == "119778af93424ebd6023ecbbec821fbf" &&
              detail->files[1].size == 872913,
          "clean file with its md5 in lower case");
    Check(GB::NewestFile(detail->files) == 1655600000, "newest file date");
    const std::string expected = "Welcome to the \"official\" page!\n\nFeatures\n\n"
                                 "\xE2\x80\xA2 Explore together\n\xE2\x80\xA2 Play & race\n\n"
                                 "Bye! \xE2\x98\xBA";
    if (detail->text != expected) std::printf("text: [%s]\n", detail->text.c_str());
    Check(detail->text == expected, "description as text");
    Check(GB::HtmlToText("ab\xC3\xB1" "cd", 3) == "ab\xE2\x80\xA6", "cut on a character boundary");
    const auto files = GB::ParseModDetail(R"({"_idRow": 7, "_tsDateModified": 1, "_aFiles": []})");
    Check(files && files->files.empty(), "files-only answer parsed");

    // The CNX index.
    const auto entries = Cnx::ParseIndex(CnxIndex, "0100000000abc000");
    Check(entries.size() == 2, "entries for the title: enabled, https only");
    Check(entries[0].name == "One Two" && entries[0].kind == Cnx::Kind::Translation && entries[0].size == "53.7 MB",
          "translation entry");
    Check(entries[1].name == "Patch" && entries[1].kind == Cnx::Kind::Modification, "title from the installed file");
    Check(Cnx::ParseSize("53.7 MB") == static_cast<std::uint64_t>(53.7 * 1048576), "size in MB");
    Check(Cnx::ParseSize("622 KB") == 622 * 1024 && Cnx::ParseSize("1.08 GB") > 1000000000, "sizes in KB and GB");
    Check(Cnx::ParseIndex("[]", "0100000000ABC000").empty(), "garbage index");

    std::printf("gamebanana-parse: PASS\n");
    return 0;
}

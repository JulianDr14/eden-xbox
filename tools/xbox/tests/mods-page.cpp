// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstdlib>
#include "eden_uwp/mods/mods_page.h"

namespace {
using namespace EdenXbox::Mods;
using Kind = ModsCommand::Kind;
using In = ModsInput;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        std::fflush(stdout);
        std::abort();
    }
}

bool Is(const ModsCommand& command, Kind kind, std::size_t index = 0, std::size_t to = 0) {
    return command.kind == kind && command.index == index && command.to == to;
}
} // namespace

int main() {
    ModsCounts counts{.installed = 8, .files = 3};
    {
        // Installed: the cursor wraps, the window follows it, A toggles the row.
        ModsPage page;
        Check(Is(HandleModsInput(page, counts, In::Up), Kind::Moved) && page.selected == 7, "wrap up");
        Check(page.first == 2, "window follows");
        Check(Is(HandleModsInput(page, counts, In::Confirm), Kind::Toggle, 7), "toggle");
        Check(Is(HandleModsInput(page, counts, In::Back), Kind::Close), "close");
    }
    {
        // Moving: X picks the mod up, Up/Down carry it (not past the ends), A puts it down.
        ModsPage page;
        page.selected = 1;
        HandleModsInput(page, counts, In::Alternate);
        Check(page.moving, "pick up");
        Check(Is(HandleModsInput(page, counts, In::Up), Kind::Move, 1, 0) && page.selected == 0, "move up");
        Check(Is(HandleModsInput(page, counts, In::Up), Kind::None), "top stays");
        Check(Is(HandleModsInput(page, counts, In::Down), Kind::Move, 0, 1), "move down");
        Check(Is(HandleModsInput(page, counts, In::NextTab), Kind::None) && page.tab == ModsTab::Installed,
              "no tab switch while moving");
        HandleModsInput(page, counts, In::Confirm);
        Check(!page.moving, "put down");
        Check(Is(HandleModsInput(page, ModsCounts{.installed = 1}, In::Alternate), Kind::None),
              "one mod cannot move");
    }
    {
        // Actions: the files first; Move enters moving; Remove asks first, and B there keeps the mod.
        ModsPage page;
        page.selected = 3;
        HandleModsInput(page, counts, In::Menu);
        Check(page.screen == ModsScreen::Actions, "actions");
        HandleModsInput(page, counts, In::Confirm);
        Check(page.screen == ModsScreen::Contents && page.line == 0, "contents");
        // The files scroll a row at a time, no further than the last screenful.
        ModsCounts many = counts;
        many.contents = ModsLayout::SheetRows + 2;
        Check(Is(HandleModsInput(page, many, In::Up), Kind::None), "contents top");
        HandleModsInput(page, many, In::Down);
        HandleModsInput(page, many, In::Down);
        Check(Is(HandleModsInput(page, many, In::Down), Kind::None) && page.line == 2, "contents bottom");
        HandleModsInput(page, many, In::Back);
        Check(page.screen == ModsScreen::List, "contents closed");
        HandleModsInput(page, counts, In::Menu);
        HandleModsInput(page, counts, In::Down);
        HandleModsInput(page, counts, In::Confirm);
        Check(page.screen == ModsScreen::List && page.moving, "action move");
        page.moving = false;
        HandleModsInput(page, counts, In::Menu);
        HandleModsInput(page, counts, In::Down);
        HandleModsInput(page, counts, In::Down);
        HandleModsInput(page, counts, In::Confirm);
        Check(page.screen == ModsScreen::Remove, "confirm remove");
        HandleModsInput(page, counts, In::Back);
        Check(page.screen == ModsScreen::List, "keep");
        HandleModsInput(page, counts, In::Menu);
        HandleModsInput(page, counts, In::Up);
        HandleModsInput(page, counts, In::Confirm);
        Check(Is(HandleModsInput(page, counts, In::Confirm), Kind::Remove, 3), "remove");
    }
    {
        // Files: tabs switch, A opens, B goes up a folder or closes at the sources.
        ModsPage page;
        Check(Is(HandleModsInput(page, counts, In::NextTab), Kind::Store) && page.tab == ModsTab::Explore,
              "explore opens the store");
        HandleModsInput(page, counts, In::NextTab);
        Check(page.tab == ModsTab::Files && page.selected == 0, "files tab");
        HandleModsInput(page, counts, In::Down);
        Check(Is(HandleModsInput(page, counts, In::Confirm), Kind::Open, 1), "open");
        Check(Is(HandleModsInput(page, counts, In::Alternate), Kind::None), "no move in files");
        ModsCounts inside = counts;
        inside.files_root = false;
        Check(Is(HandleModsInput(page, inside, In::Back), Kind::Up), "up");
        Check(Is(HandleModsInput(page, counts, In::Back), Kind::Close), "close at root");
        Check(Is(HandleModsInput(page, ModsCounts{}, In::Confirm), Kind::None), "empty list");
    }
    {
        // Variants: one candidate installs with A; several are ticked with A and installed with X.
        ModsPage page;
        ShowVariants(page, 1);
        Check(Is(HandleModsInput(page, counts, In::Confirm), Kind::Install), "single");
        ShowVariants(page, 3);
        HandleModsInput(page, counts, In::Confirm);
        Check(!page.chosen[0] && page.chosen[1], "untick");
        page.chosen.assign(3, false);
        Check(Is(HandleModsInput(page, counts, In::Alternate), Kind::None), "nothing ticked");
        HandleModsInput(page, counts, In::Down);
        HandleModsInput(page, counts, In::Confirm);
        Check(Is(HandleModsInput(page, counts, In::Alternate), Kind::Install) && page.chosen[1], "install ticked");
        Check(Is(HandleModsInput(page, counts, In::Back), Kind::Dismiss), "dismiss variants");
        Check(page.screen == ModsScreen::List && page.chosen.empty(), "cancel variants");
    }
    {
        // Actions with an update: Update comes after the files and opens the update's page.
        ModsPage page;
        page.selected = 2;
        ModsCounts update = counts;
        update.update = true;
        HandleModsInput(page, update, In::Menu);
        HandleModsInput(page, update, In::Down);
        Check(Is(HandleModsInput(page, update, In::Confirm), Kind::Update, 2) && page.screen == ModsScreen::Detail,
              "update");
        Check(Is(HandleModsInput(page, update, In::Back), Kind::CloseDetail) && page.tab == ModsTab::Installed,
              "back from the update");
    }
    {
        // Explore: a grid of four columns; the cursor stays inside it and reports where it is.
        ModsPage page;
        page.tab = ModsTab::Explore;
        ModsCounts store = counts;
        store.cards = 10;
        Check(Is(HandleModsInput(page, store, In::Left), Kind::None), "left edge");
        Check(Is(HandleModsInput(page, store, In::Right), Kind::Want, 1), "right");
        Check(Is(HandleModsInput(page, store, In::Down), Kind::Want, 5), "down a row");
        Check(Is(HandleModsInput(page, store, In::Down), Kind::Want, 9) && page.card_first == 4,
              "down to the short last row; the grid scrolls");
        Check(Is(HandleModsInput(page, store, In::Down), Kind::None), "bottom edge");
        Check(Is(HandleModsInput(page, store, In::Up), Kind::Want, 5), "up");
        Check(Is(HandleModsInput(page, store, In::Confirm), Kind::Detail, 5) && page.screen == ModsScreen::Detail,
              "open a mod");
        // Its page: files with Up/Down, pictures with the bumpers, A downloads.
        store.detail_files = 2;
        store.detail_images = 3;
        HandleModsInput(page, store, In::Down);
        HandleModsInput(page, store, In::PreviousTab);
        Check(page.file == 1 && page.image == 2, "file and picture cursors wrap");
        Check(Is(HandleModsInput(page, store, In::Confirm), Kind::Download, 1), "download");
        Check(Is(HandleModsInput(page, store, In::Back), Kind::CloseDetail) && page.screen == ModsScreen::List,
              "close the page");
        Check(Is(HandleModsInput(page, store, In::Back), Kind::Close), "B leaves");
    }
    {
        // The store's options: the rows follow the source; Sort stays open to press again.
        ModsPage page;
        page.tab = ModsTab::Explore;
        ModsCounts store = counts;
        store.store = {true, true, true};
        store.search = L"fps";
        Check(StoreActions(store.store).size() == 7, "all options");
        Check(StoreActions({false, false, false}).size() == 2, "CNX: search and source");
        HandleModsInput(page, store, In::Menu);
        Check(Is(HandleModsInput(page, store, In::Confirm), Kind::Categories) && page.screen == ModsScreen::Categories,
              "category opens the sheet");
        HandleModsInput(page, store, In::Back);
        HandleModsInput(page, store, In::Menu);
        HandleModsInput(page, store, In::Down);
        Check(Is(HandleModsInput(page, store, In::Confirm), Kind::Sort) && page.screen == ModsScreen::StoreMenu,
              "sort in place");
        HandleModsInput(page, store, In::Down);
        HandleModsInput(page, store, In::Confirm);
        Check(page.screen == ModsScreen::Keyboard && page.keyboard && page.keyboard->Value() == L"fps",
              "search opens the keyboard with the search");
        HandleModsInput(page, store, In::Alternate); // Delete
        const auto done = HandleModsInput(page, store, In::Start);
        Check(done.kind == Kind::Search && done.text == L"fp" && page.screen == ModsScreen::List && !page.keyboard,
              "Menu finishes the search");
        HandleModsInput(page, store, In::Menu);
        page.menu = 6;
        Check(Is(HandleModsInput(page, store, In::Confirm), Kind::CancelDownloads), "cancel downloads");
    }
    {
        // Categories: the Menu button opens them; A goes down into one with others under it, X lists
        // it as it is, B goes up a level and closes at the top; A on the rest lists it.
        ModsPage page;
        page.tab = ModsTab::Explore;
        ModsCounts store = counts;
        store.cards = 4;
        Check(Is(HandleModsInput(page, store, In::Start), Kind::Categories) && page.screen == ModsScreen::Categories,
              "menu opens categories");
        store.category_nested = {false, false, true, false}; // All, 18+, a nested one, a leaf.
        HandleModsInput(page, store, In::Down);
        HandleModsInput(page, store, In::Down);
        Check(Is(HandleModsInput(page, store, In::Confirm), Kind::EnterCategory, 2) && page.category == 0 &&
                  page.screen == ModsScreen::Categories,
              "down a level");
        store.category_top = false;
        store.category_nested = {false, true, false};
        HandleModsInput(page, store, In::Down);
        Check(Is(HandleModsInput(page, store, In::Alternate), Kind::PickCategory, 1) && page.screen == ModsScreen::List,
              "X lists a nested one");
        HandleModsInput(page, store, In::Start);
        Check(Is(HandleModsInput(page, store, In::Back), Kind::LeaveCategory), "up a level");
        store.category_top = true;
        store.category_nested = {false, false, true, false};
        HandleModsInput(page, store, In::Up);
        Check(Is(HandleModsInput(page, store, In::Confirm), Kind::PickCategory, 3) && page.screen == ModsScreen::List &&
                  page.card == 0,
              "a leaf is listed");
        HandleModsInput(page, store, In::Start);
        Check(Is(HandleModsInput(page, store, In::Back), Kind::Moved) && page.screen == ModsScreen::List,
              "B closes at the top");
        ModsCounts cnx = store;
        cnx.store.gamebanana = false;
        Check(Is(HandleModsInput(page, cnx, In::Start), Kind::None), "CNX has no categories");
    }
    {
        // A game the store cannot match: A types its name, the results are picked from.
        ModsPage page;
        page.tab = ModsTab::Explore;
        ModsCounts store = counts;
        store.no_game = true;
        store.game_name = L"Juego";
        HandleModsInput(page, store, In::Confirm);
        Check(page.screen == ModsScreen::Keyboard && page.keyboard_for == KeyboardFor::Game &&
                  page.keyboard->Value() == L"Juego",
              "find the game");
        HandleModsInput(page, store, In::Back);
        Check(page.screen == ModsScreen::List && !page.keyboard, "keyboard cancelled");
        HandleModsInput(page, store, In::Confirm);
        const auto find = HandleModsInput(page, store, In::NextTab);
        Check(find.kind == Kind::FindGame && find.text == L"Juego" && page.screen == ModsScreen::Games, "search games");
        store.games = 3;
        HandleModsInput(page, store, In::Up);
        Check(Is(HandleModsInput(page, store, In::Confirm), Kind::PickGame, 2) && page.screen == ModsScreen::List,
              "pick a game");
        ModsCounts offline = counts;
        offline.offline = true;
        Check(Is(HandleModsInput(page, offline, In::Confirm), Kind::Retry), "retry when offline");
    }
    {
        // A running job takes only B, which cancels it.
        ModsPage page;
        ModsCounts busy = counts;
        busy.busy = true;
        Check(Is(HandleModsInput(page, busy, In::Confirm), Kind::None), "busy ignores");
        Check(Is(HandleModsInput(page, busy, In::Back), Kind::Cancel), "busy cancels");
    }
    std::puts("mods-page PASS");
}

// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The mods page of a game, opened from its options in the library. Three tabs:
//  - Installed: the game's mods in priority order. A turns one on or off; X picks it up to move it
//    with Up and Down (the higher one wins the files both change); Y opens what else can be done
//    with it (see its files, update, move, uninstall).
//  - Explore: GameBanana's mods for the game (or the CNX index's) in a grid of pictures. A opens a
//    mod's page: its pictures, description and files, where A downloads the file under the cursor.
//    X searches; the Menu button picks a category; Y changes the order, the category, the source,
//    or the game it is matched to.
//  - Files: the archives (.zip, .7z, .rar) in the folders the player gave the app, internal storage
//    first. A opens a folder or an archive; when an archive holds several mods (variants), the
//    player ticks which to install.
// This is the page's state and what each press does; ModSession carries the commands out.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eden_uwp/list_navigation.h"
#include "eden_uwp/text_entry.h"

namespace EdenXbox::Mods {

/// Geometry of the page's panel, from its top-left corner (layout units of the 1280x720 screen).
struct ModsLayout {
    static constexpr float Width = 1060, Height = 650, Margin = 40;
    static constexpr float TabsTop = 112, TabHeight = 38;
    static constexpr float RowTop = 166, RowStride = 62, RowHeight = 56;
    static constexpr std::size_t VisibleRows = 6;
    static constexpr float Detail = 546, Footer = 606;
    // Explore's grid: four cards a row, two rows on screen.
    static constexpr std::size_t Columns = 4, GridRows = 2;
    static constexpr float GridTop = 164, CardWidth = 233, CardHeight = 182, CardGapX = 16, CardGapY = 14;
    static constexpr float PictureHeight = 131;
    /// The text entry panel, drawn over the page.
    static constexpr std::size_t SearchLength = 40;
    /// Rows the contents and categories sheets show at once.
    static constexpr std::size_t SheetRows = 7;
};

enum class ModsTab : std::uint8_t { Installed, Explore, Files };
inline constexpr std::size_t ModsTabCount = 3;

enum class ModsScreen : std::uint8_t {
    List,      ///< The tab's list.
    Actions,   ///< What can be done with the selected mod.
    Remove,    ///< Asking before an uninstall.
    Variants,  ///< Which mods of an archive to install.
    Detail,    ///< A store mod's page.
    StoreMenu, ///< The store's options.
    Keyboard,  ///< Typing a search, or a game's name.
    Games,     ///< Picking the game the store lists mods for.
    Contents,  ///< The files of an installed mod.
    Categories, ///< Picking the category the store lists.
};

/// Rows of the Actions sheet; Update only when the mod has one.
enum class ModAction : std::uint8_t { Contents, Update, Move, Remove };

inline std::vector<ModAction> InstalledActions(bool update) {
    std::vector<ModAction> actions;
    actions.push_back(ModAction::Contents);
    if (update) actions.push_back(ModAction::Update);
    actions.push_back(ModAction::Move);
    actions.push_back(ModAction::Remove);
    return actions;
}

/// What the store's options depend on.
struct StoreFlags {
    bool gamebanana{true}; ///< The source is GameBanana (else the CNX index).
    bool search{};         ///< A search filters the list.
    bool downloads{};      ///< Something downloads.
};

/// Rows of the store's options.
enum class StoreAction : std::uint8_t { Category, Sort, Search, ClearSearch, Source, ChangeGame, CancelDownloads };

inline std::vector<StoreAction> StoreActions(const StoreFlags& flags) {
    std::vector<StoreAction> actions;
    if (flags.gamebanana) actions.push_back(StoreAction::Category);
    if (flags.gamebanana) actions.push_back(StoreAction::Sort);
    actions.push_back(StoreAction::Search);
    if (flags.search) actions.push_back(StoreAction::ClearSearch);
    actions.push_back(StoreAction::Source);
    if (flags.gamebanana) actions.push_back(StoreAction::ChangeGame);
    if (flags.downloads) actions.push_back(StoreAction::CancelDownloads);
    return actions;
}

enum class KeyboardFor : std::uint8_t { Search, Game };

struct ModsPage {
    ModsTab tab{ModsTab::Installed};
    ModsScreen screen{ModsScreen::List};
    std::size_t selected{}; ///< Row in the Installed or Files list.
    std::size_t first{};    ///< First row shown.
    bool moving{};          ///< Installed: Up and Down move the selected mod.
    std::size_t action{};   ///< Row in the Actions sheet.
    std::vector<bool> chosen; ///< Variants: which candidates install.
    std::size_t variant{};  ///< Variants: the cursor.
    std::size_t card{};     ///< Explore: the card under the cursor.
    std::size_t card_first{}; ///< Explore: the first card shown (a row's first).
    std::size_t file{};     ///< Detail: the file under the cursor.
    std::size_t image{};    ///< Detail: the picture shown.
    std::size_t menu{};     ///< StoreMenu: the cursor.
    std::size_t game{};     ///< Games: the cursor.
    std::size_t line{};     ///< Contents: the first file shown.
    std::size_t category{}; ///< Categories: the cursor.
    std::optional<TextEntry> keyboard;
    KeyboardFor keyboard_for{KeyboardFor::Search};
    ModsScreen keyboard_return{ModsScreen::List}; ///< Where Back leaves the keyboard to.
};

enum class ModsInput : std::uint8_t {
    Up, Down, Left, Right, PreviousTab, NextTab,
    Confirm,   ///< A
    Alternate, ///< X
    Menu,      ///< Y
    Start,     ///< The Menu button
    Back,      ///< B
};

/// What a press asks ModSession to do.
struct ModsCommand {
    enum class Kind : std::uint8_t {
        None,
        Moved,       ///< Only the page changed.
        Toggle,      ///< Turn mod `index` on or off.
        Move,        ///< Move mod `index` to `to`.
        Remove,      ///< Uninstall mod `index`.
        Open,        ///< Open row `index` of the Files list (a folder, an archive, or "add a folder").
        Up,          ///< Files: back to the parent folder.
        Install,     ///< Install the ticked candidates (ModsPage::chosen).
        Dismiss,     ///< Leave an archive's variants without installing.
        Cancel,      ///< Stop the running job.
        Close,       ///< Leave the page.
        Store,       ///< Explore is shown: the store opens (once).
        Want,        ///< Explore's cursor is on card `index`.
        Detail,      ///< Open card `index`'s page.
        Update,      ///< Open the page of installed mod `index`'s update.
        CloseDetail,
        Download,    ///< Download file `index` of the open page.
        Sort,        ///< The next order.
        Search,      ///< Filter by `text` (empty clears it).
        Source,      ///< Switch between GameBanana and the CNX index.
        FindGame,    ///< Look GameBanana's games up by `text`.
        PickGame,    ///< The store lists game `index`'s mods.
        CancelDownloads,
        Retry,       ///< Ask the store again after it could not be reached.
        Categories,  ///< Open the categories sheet.
        EnterCategory, ///< Go down to the categories under row `index`.
        LeaveCategory, ///< Go up a level of categories.
        PickCategory,  ///< The store lists row `index`'s mods.
    };
    Kind kind{Kind::None};
    std::size_t index{};
    std::size_t to{};
    std::wstring text;
};

/// What the page needs to know of the session.
struct ModsCounts {
    std::size_t installed{};
    std::size_t files{};      ///< Rows of the Files list.
    bool files_root{true};    ///< The Files list shows the sources, not a folder.
    bool busy{};              ///< A job runs: only Back is taken, and it cancels the job.
    bool update{};            ///< The selected installed mod has an update.
    std::size_t cards{};      ///< Explore's cards.
    bool no_game{};           ///< The store could not match the game: A picks it.
    bool offline{};           ///< The store could not be reached: A asks again.
    std::size_t detail_files{};
    std::size_t detail_images{};
    std::size_t games{};      ///< A game search's results.
    std::size_t contents{};   ///< Files of the selected installed mod.
    /// The categories sheet's rows: whether each has categories under it.
    std::vector<bool> category_nested;
    bool category_top{true};  ///< The sheet shows the top level.
    StoreFlags store{};
    std::wstring search;    ///< The current search, to edit.
    std::wstring game_name; ///< The game's name, to look it up by.
};

/// Keeps the cursor inside a list of `count` rows and the window around it.
inline void ClampModsList(ModsPage& page, std::size_t count) {
    ClampListWindow(page.selected, page.first, count, ModsLayout::VisibleRows);
}

/// Keeps Explore's cursor inside `count` cards, with its row on screen.
inline void ClampModsGrid(ModsPage& page, std::size_t count) {
    using L = ModsLayout;
    page.card = count ? std::min(page.card, count - 1) : 0;
    const std::size_t row = page.card / L::Columns;
    std::size_t first_row = page.card_first / L::Columns;
    if (row < first_row) first_row = row;
    if (row >= first_row + L::GridRows) first_row = row + 1 - L::GridRows;
    page.card_first = first_row * L::Columns;
}

/// Opens the Variants screen for an archive's `count` candidates, all ticked.
inline void ShowVariants(ModsPage& page, std::size_t count) {
    page.screen = ModsScreen::Variants;
    page.chosen.assign(count, true);
    page.variant = 0;
}

/// After an install: back to the Installed list, on its top row (the new mods go first).
inline void ShowInstalled(ModsPage& page) {
    page = ModsPage{};
}

namespace ModsPageDetail {

inline void Step(std::size_t& cursor, std::size_t size, bool up) {
    if (size) cursor = (cursor + (up ? size - 1 : 1)) % size;
}

inline ModsCommand OpenCategories(ModsPage& page) {
    page.screen = ModsScreen::Categories;
    page.category = 0;
    return {ModsCommand::Kind::Categories};
}

inline ModsCommand HandleCategories(ModsPage& page, const ModsCounts& counts, ModsInput input) {
    using Kind = ModsCommand::Kind;
    const std::size_t rows = counts.category_nested.size();
    page.category = rows ? std::min(page.category, rows - 1) : 0;
    switch (input) {
    case ModsInput::Up: case ModsInput::Down:
        Step(page.category, rows, input == ModsInput::Up);
        return {Kind::Moved};
    case ModsInput::Confirm: {
        if (page.category >= rows) return {};
        const std::size_t row = page.category;
        if (counts.category_nested[row]) {
            page.category = 0;
            return {Kind::EnterCategory, row};
        }
        page.screen = ModsScreen::List;
        page.card = page.card_first = 0;
        return {Kind::PickCategory, row};
    }
    case ModsInput::Alternate:
        // X lists a category that has others under it, without going down.
        if (page.category >= rows) return {};
        page.screen = ModsScreen::List;
        page.card = page.card_first = 0;
        return {Kind::PickCategory, page.category};
    case ModsInput::Back:
        if (!counts.category_top) {
            page.category = 0;
            return {Kind::LeaveCategory};
        }
        page.screen = ModsScreen::List;
        return {Kind::Moved};
    case ModsInput::Start: case ModsInput::Menu:
        page.screen = ModsScreen::List;
        return {Kind::Moved};
    default:
        return {};
    }
}

inline void OpenKeyboard(ModsPage& page, KeyboardFor purpose, std::wstring_view text) {
    page.keyboard.emplace(std::wstring{text}, ModsLayout::SearchLength);
    page.keyboard_for = purpose;
    page.keyboard_return = page.screen;
    page.screen = ModsScreen::Keyboard;
}

inline ModsCommand HandleKeyboard(ModsPage& page, ModsInput input) {
    using Kind = ModsCommand::Kind;
    using Command = TextEntry::Command;
    Command command{};
    switch (input) {
    case ModsInput::Up: command = Command::Up; break;
    case ModsInput::Down: command = Command::Down; break;
    case ModsInput::Left: command = Command::Left; break;
    case ModsInput::Right: command = Command::Right; break;
    case ModsInput::Confirm: command = Command::Press; break;
    case ModsInput::Alternate: command = Command::Delete; break;
    case ModsInput::Menu: command = Command::Space; break;
    case ModsInput::PreviousTab: command = Command::Shift; break;
    case ModsInput::NextTab: case ModsInput::Start: command = Command::Done; break;
    case ModsInput::Back: command = Command::Cancel; break;
    }
    if (!page.keyboard) {
        page.screen = ModsScreen::List;
        return {Kind::Moved};
    }
    const auto result = page.keyboard->Apply(command);
    if (result == TextEntry::Result::None) return {Kind::Moved};
    std::wstring text = page.keyboard->Value();
    page.keyboard.reset();
    if (result == TextEntry::Result::Cancel) {
        page.screen = page.keyboard_return;
        return {Kind::Moved};
    }
    if (page.keyboard_for == KeyboardFor::Search) {
        page.screen = ModsScreen::List;
        page.card = page.card_first = 0;
        return {Kind::Search, 0, 0, std::move(text)};
    }
    if (text.find_first_not_of(L' ') == std::wstring::npos) {
        page.screen = page.keyboard_return;
        return {Kind::Moved};
    }
    page.screen = ModsScreen::Games;
    page.game = 0;
    return {Kind::FindGame, 0, 0, std::move(text)};
}

inline ModsCommand HandleStoreMenu(ModsPage& page, const ModsCounts& counts, ModsInput input) {
    using Kind = ModsCommand::Kind;
    const auto actions = StoreActions(counts.store);
    page.menu = std::min(page.menu, actions.size() - 1);
    switch (input) {
    case ModsInput::Up: case ModsInput::Down:
        Step(page.menu, actions.size(), input == ModsInput::Up);
        return {Kind::Moved};
    case ModsInput::Back: case ModsInput::Menu:
        page.screen = ModsScreen::List;
        return {Kind::Moved};
    case ModsInput::Confirm:
        break;
    default:
        return {};
    }
    switch (actions[page.menu]) {
    case StoreAction::Category:
        return OpenCategories(page);
    case StoreAction::Sort:
        // The order changes in place, so pressing again goes through them all.
        page.card = page.card_first = 0;
        return {Kind::Sort};
    case StoreAction::Search:
        OpenKeyboard(page, KeyboardFor::Search, counts.search);
        return {Kind::Moved};
    case StoreAction::ClearSearch:
        page.screen = ModsScreen::List;
        page.card = page.card_first = 0;
        return {Kind::Search};
    case StoreAction::Source:
        page.screen = ModsScreen::List;
        page.card = page.card_first = 0;
        return {Kind::Source};
    case StoreAction::ChangeGame:
        OpenKeyboard(page, KeyboardFor::Game, counts.game_name);
        return {Kind::Moved};
    case StoreAction::CancelDownloads:
        page.screen = ModsScreen::List;
        return {Kind::CancelDownloads};
    }
    return {};
}

inline ModsCommand HandleExplore(ModsPage& page, const ModsCounts& counts, ModsInput input) {
    using Kind = ModsCommand::Kind;
    using L = ModsLayout;
    const std::size_t count = counts.cards;
    const auto moved = [&] {
        ClampModsGrid(page, count);
        return ModsCommand{Kind::Want, page.card};
    };
    switch (input) {
    case ModsInput::Left:
        if (page.card == 0) return {};
        --page.card;
        return moved();
    case ModsInput::Right:
        if (page.card + 1 >= count) return {};
        ++page.card;
        return moved();
    case ModsInput::Up:
        if (page.card < L::Columns) return {};
        page.card -= L::Columns;
        return moved();
    case ModsInput::Down:
        // Down from a row above a shorter last row lands on that row's last card.
        if (count == 0 || page.card / L::Columns == (count - 1) / L::Columns) return {};
        page.card = std::min(page.card + L::Columns, count - 1);
        return moved();
    case ModsInput::Confirm:
        if (count > 0) {
            page.screen = ModsScreen::Detail;
            page.file = page.image = 0;
            return {Kind::Detail, page.card};
        }
        if (counts.no_game) {
            OpenKeyboard(page, KeyboardFor::Game, counts.game_name);
            return {Kind::Moved};
        }
        return counts.offline ? ModsCommand{Kind::Retry} : ModsCommand{};
    case ModsInput::Alternate:
        OpenKeyboard(page, KeyboardFor::Search, counts.search);
        return {Kind::Moved};
    case ModsInput::Menu:
        page.screen = ModsScreen::StoreMenu;
        page.menu = 0;
        return {Kind::Moved};
    case ModsInput::Start:
        if (!counts.store.gamebanana) return {};
        return OpenCategories(page);
    default:
        return {};
    }
}

} // namespace ModsPageDetail

inline ModsCommand HandleModsInput(ModsPage& page, const ModsCounts& counts, ModsInput input) {
    using Kind = ModsCommand::Kind;
    using namespace ModsPageDetail;
    if (counts.busy) return {input == ModsInput::Back ? Kind::Cancel : Kind::None};
    const std::size_t count = page.tab == ModsTab::Installed ? counts.installed : counts.files;

    switch (page.screen) {
    case ModsScreen::Actions: {
        const auto actions = InstalledActions(counts.update);
        page.action = std::min(page.action, actions.size() - 1);
        switch (input) {
        case ModsInput::Up: case ModsInput::Down:
            Step(page.action, actions.size(), input == ModsInput::Up);
            return {Kind::Moved};
        case ModsInput::Confirm:
            switch (actions[page.action]) {
            case ModAction::Contents:
                page.screen = ModsScreen::Contents;
                page.line = 0;
                return {Kind::Moved};
            case ModAction::Update:
                page.screen = ModsScreen::Detail;
                page.file = page.image = 0;
                return {Kind::Update, page.selected};
            case ModAction::Move:
                page.screen = ModsScreen::List;
                page.moving = true;
                return {Kind::Moved};
            case ModAction::Remove:
                page.screen = ModsScreen::Remove;
                return {Kind::Moved};
            }
            return {};
        case ModsInput::Back: case ModsInput::Menu:
            page.screen = ModsScreen::List;
            return {Kind::Moved};
        default:
            return {};
        }
    }
    case ModsScreen::Remove:
        if (input == ModsInput::Confirm) {
            page.screen = ModsScreen::List;
            return {Kind::Remove, page.selected};
        }
        if (input == ModsInput::Back) page.screen = ModsScreen::List;
        return {Kind::Moved};
    case ModsScreen::Variants: {
        const std::size_t variants = page.chosen.size();
        const bool any = std::find(page.chosen.begin(), page.chosen.end(), true) != page.chosen.end();
        switch (input) {
        case ModsInput::Up: case ModsInput::Down:
            Step(page.variant, variants, input == ModsInput::Up);
            return {Kind::Moved};
        case ModsInput::Confirm:
            // One candidate: A installs it. Several: A ticks, X installs the ticked ones.
            if (variants == 1) return {Kind::Install};
            if (page.variant < variants) page.chosen[page.variant] = !page.chosen[page.variant];
            return {Kind::Moved};
        case ModsInput::Alternate:
            return any ? ModsCommand{Kind::Install} : ModsCommand{};
        case ModsInput::Back:
            page.screen = ModsScreen::List;
            page.chosen.clear();
            return {Kind::Dismiss};
        default:
            return {};
        }
    }
    case ModsScreen::Detail:
        switch (input) {
        case ModsInput::Up: case ModsInput::Down:
            Step(page.file, counts.detail_files, input == ModsInput::Up);
            return {Kind::Moved};
        case ModsInput::PreviousTab: case ModsInput::NextTab: case ModsInput::Left: case ModsInput::Right: {
            const bool back = input == ModsInput::PreviousTab || input == ModsInput::Left;
            Step(page.image, counts.detail_images, back);
            return {Kind::Moved};
        }
        case ModsInput::Confirm:
            if (page.file >= counts.detail_files) return {};
            return {Kind::Download, page.file};
        case ModsInput::Back:
            page.screen = ModsScreen::List;
            return {Kind::CloseDetail};
        default:
            return {};
        }
    case ModsScreen::Contents: {
        // The list scrolls a row at a time; it has no cursor.
        const std::size_t last = counts.contents > ModsLayout::SheetRows ? counts.contents - ModsLayout::SheetRows : 0;
        switch (input) {
        case ModsInput::Up:
            if (page.line == 0) return {};
            --page.line;
            return {Kind::Moved};
        case ModsInput::Down:
            if (page.line >= last) return {};
            ++page.line;
            return {Kind::Moved};
        case ModsInput::Back: case ModsInput::Confirm:
            page.screen = ModsScreen::List;
            return {Kind::Moved};
        default:
            return {};
        }
    }
    case ModsScreen::Categories:
        return HandleCategories(page, counts, input);
    case ModsScreen::StoreMenu:
        return HandleStoreMenu(page, counts, input);
    case ModsScreen::Keyboard:
        return HandleKeyboard(page, input);
    case ModsScreen::Games:
        switch (input) {
        case ModsInput::Up: case ModsInput::Down:
            Step(page.game, counts.games, input == ModsInput::Up);
            return {Kind::Moved};
        case ModsInput::Confirm:
            if (page.game >= counts.games) return {};
            page.screen = ModsScreen::List;
            page.card = page.card_first = 0;
            return {Kind::PickGame, page.game};
        case ModsInput::Alternate:
            OpenKeyboard(page, KeyboardFor::Game, counts.game_name);
            return {Kind::Moved};
        case ModsInput::Back:
            page.screen = ModsScreen::List;
            return {Kind::Moved};
        default:
            return {};
        }
    case ModsScreen::List:
        break;
    }

    if (page.moving) {
        switch (input) {
        case ModsInput::Up: case ModsInput::Down: {
            const bool up = input == ModsInput::Up;
            if ((up && page.selected == 0) || (!up && page.selected + 1 >= count)) return {};
            const std::size_t from = page.selected;
            page.selected = up ? from - 1 : from + 1;
            ClampModsList(page, count);
            return {Kind::Move, from, page.selected};
        }
        case ModsInput::Confirm: case ModsInput::Alternate: case ModsInput::Back:
            page.moving = false;
            return {Kind::Moved};
        default:
            return {};
        }
    }

    if (input == ModsInput::PreviousTab || input == ModsInput::NextTab) {
        const auto tab = static_cast<std::size_t>(page.tab);
        page.tab = static_cast<ModsTab>((tab + (input == ModsInput::NextTab ? 1 : ModsTabCount - 1)) % ModsTabCount);
        page.selected = page.first = 0;
        return {page.tab == ModsTab::Explore ? Kind::Store : Kind::Moved};
    }
    if (page.tab == ModsTab::Explore) {
        if (input == ModsInput::Back) return {Kind::Close};
        return HandleExplore(page, counts, input);
    }

    switch (input) {
    case ModsInput::Up: case ModsInput::Down:
        Step(page.selected, count, input == ModsInput::Up);
        ClampModsList(page, count);
        return {Kind::Moved};
    case ModsInput::Confirm:
        if (count == 0) return {};
        return {page.tab == ModsTab::Installed ? Kind::Toggle : Kind::Open, page.selected};
    case ModsInput::Alternate:
        if (page.tab != ModsTab::Installed || count < 2) return {};
        page.moving = true;
        return {Kind::Moved};
    case ModsInput::Menu:
        if (page.tab != ModsTab::Installed || count == 0) return {};
        page.screen = ModsScreen::Actions;
        page.action = 0;
        return {Kind::Moved};
    case ModsInput::Back:
        if (page.tab == ModsTab::Files && !counts.files_root) {
            page.selected = page.first = 0;
            return {Kind::Up};
        }
        return {Kind::Close};
    default:
        return {};
    }
}

} // namespace EdenXbox::Mods

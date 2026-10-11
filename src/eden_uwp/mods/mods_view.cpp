// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/mods/mods_view.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>

#include "eden_uwp/text_entry_panel.h"
#include "eden_uwp/ui_painter.h"
#include "eden_uwp/utf8.h"

namespace EdenXbox::Mods {
namespace {
using I18n::Text;
using I18n::Tr;
using L = ModsLayout;

constexpr float RowWidth = L::Width - 2 * L::Margin;
const std::wstring Dot =L"  ·  ";

/// "512 KB", "37 MB", "1.4 GB".
std::wstring Size(std::uint64_t bytes) {
    wchar_t text[32];
    if (bytes >= (1ull << 30)) std::swprintf(text, 32, L"%.1f GB", static_cast<double>(bytes) / (1ull << 30));
    else if (bytes >= (1ull << 20)) std::swprintf(text, 32, L"%llu MB", static_cast<unsigned long long>(bytes >> 20));
    else std::swprintf(text, 32, L"%llu KB", static_cast<unsigned long long>((bytes + 1023) >> 10));
    return text;
}

/// "950", "12K", "1.3M": counts that fit a card.
std::wstring Count(std::int64_t value) {
    wchar_t text[32];
    if (value >= 1'000'000) std::swprintf(text, 32, L"%.1fM", static_cast<double>(value) / 1e6);
    else if (value >= 10'000) std::swprintf(text, 32, L"%lldK", static_cast<long long>(value / 1000));
    else std::swprintf(text, 32, L"%lld", static_cast<long long>(std::max<std::int64_t>(value, 0)));
    return text;
}

/// "2022-06-12": the same in every language.
std::wstring Date(std::int64_t unix_time) {
    if (unix_time <= 0) return {};
    const std::chrono::sys_days day{std::chrono::floor<std::chrono::days>(std::chrono::sys_seconds{std::chrono::seconds{unix_time}})};
    const std::chrono::year_month_day ymd{day};
    wchar_t text[16];
    std::swprintf(text, 16, L"%04d-%02u-%02u", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                  static_cast<unsigned>(ymd.day()));
    return text;
}

std::wstring Parts(std::uint8_t parts) {
    std::wstring text;
    const std::pair<ModPart, Text> names[] = {
        {RomFs, Text::ModsPartRomFs}, {RomFsExt, Text::ModsPartRomFsExt}, {ExeFs, Text::ModsPartExeFs},
        {Patch, Text::ModsPartPatch}, {Cheats, Text::ModsPartCheats}};
    for (const auto& [part, name] : names) {
        if (!(parts & part)) continue;
        if (!text.empty()) text += L" · ";
        text += Tr(name);
    }
    return text;
}

Text SortText(GameBanana::Sort sort) {
    switch (sort) {
    case GameBanana::Sort::Popular: return Text::ModsSortPopular;
    case GameBanana::Sort::Newest: return Text::ModsSortNewest;
    case GameBanana::Sort::Updated: return Text::ModsSortUpdated;
    case GameBanana::Sort::Liked: return Text::ModsSortLiked;
    }
    return Text::ModsSortPopular;
}

Text SourceText(StoreSource source) {
    return source == StoreSource::Cnx ? Text::ModsSourceCnx : Text::ModsSourceGameBanana;
}

/// The category the store lists, as a path ("Skins › Characters").
std::wstring CategoryLabel(const StoreState& store) {
    if (store.adult) return std::wstring{Tr(Text::ModsCategoryAdult)};
    if (store.category.empty()) return std::wstring{Tr(Text::ModsCategoryAll)};
    std::wstring label;
    for (const auto& category : store.category) {
        if (!label.empty()) label += L" › ";
        label += Utf8ToUtf16(category.name);
    }
    return label;
}

/// "Changes 14 game files: a.szs, b.bars, c.szs…", naming the largest; empty for a mod of patches
/// or cheats only.
std::wstring ChangesLine(const ModContents& contents) {
    const auto count = contents.files.size();
    if (count == 0) return {};
    constexpr std::size_t named = 3;
    std::wstring names;
    for (const auto& name : MainFiles(contents.listing, named)) {
        if (!names.empty()) names += L", ";
        names += Utf8ToUtf16(name);
    }
    if (count > named) names += L"…";
    return I18n::Format(count == 1 ? Text::ModsChangesOne : Text::ModsChangesOther, {std::to_wstring(count), names});
}

std::size_t ActiveCount(const std::vector<InstalledMod>& mods) {
    return static_cast<std::size_t>(
        std::count_if(mods.begin(), mods.end(), [](const InstalledMod& m) { return m.record.enabled; }));
}

/// Conflicts counted once per pair.
std::size_t ConflictCount(const std::vector<InstalledMod>& mods) {
    std::size_t count = 0;
    for (const auto& mod : mods)
        count += static_cast<std::size_t>(
            std::count_if(mod.conflicts.begin(), mod.conflicts.end(), [](const Conflict& c) { return c.wins; }));
    return count;
}

/// A small rounded label over a picture ("18+", "Installed").
void Chip(UiPainter& paint, std::wstring_view text, float x, float y, unsigned background, unsigned color) {
    const float width = paint.MeasureText(text, 13, true) + 16;
    paint.Round(x, y, width, 22, 11, background);
    paint.Text(text, x, y, 13, color, width, 22, true, false, true);
}

void Bar(UiPainter& paint, float x, float y, float width, std::uint64_t done, std::uint64_t total) {
    paint.Round(x, y, width, 6, 3, Palette::Track);
    if (total == 0) return;
    const float filled = width * static_cast<float>(std::min(done, total)) / static_cast<float>(total);
    if (filled > 0) paint.Round(x, y, std::max(filled, 6.0f), 6, 3, Palette::Accent);
}

/// The first download and how the queue goes, in one line; empty when nothing downloads.
std::wstring DownloadLine(const std::vector<DownloadStatus>& downloads) {
    if (downloads.empty()) return {};
    const auto& first = downloads.front();
    std::wstring line = I18n::Format(Text::ModsDownloading, {Utf8ToUtf16(first.title)});
    if (first.running && first.total > 0) {
        line += Dot;
        line += std::to_wstring(std::min<std::uint64_t>(first.done * 100 / first.total, 100)) + L"%";
        if (first.speed > 0) line += Dot + Size(first.speed) + L"/s";
    }
    if (downloads.size() > 1)
        line += Dot + I18n::Plural(Text::ModsQueuedOne, Text::ModsQueuedOther, downloads.size() - 1);
    return line;
}

void DrawHeader(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& snapshot = view.session.View();
    const auto& mods = snapshot.mods;
    paint.PageHeader(x + L::Margin, y + 26, UiIcon::Mods, Tr(Text::ModsTitle), view.game);
    const float right = x + L::Width - L::Margin;
    std::wstring summary;
    unsigned color = Palette::Accent;
    if (const auto active = ActiveCount(mods)) {
        summary = I18n::Plural(Text::ModsActiveOne, Text::ModsActiveOther, active);
        if (const auto conflicts = ConflictCount(mods)) {
            summary += Dot;
            summary += I18n::Plural(Text::ModsConflictsOne, Text::ModsConflictsOther, conflicts);
            color = Palette::Warning;
        }
    }
    if (const auto updates = view.session.Store().updates.size()) {
        if (!summary.empty()) summary += Dot;
        summary += I18n::Plural(Text::ModsUpdatesOne, Text::ModsUpdatesOther, updates);
    }
    if (!summary.empty())
        paint.Text(summary, right - 460, y + 32, 20, color, 460, 30, true, false, false, true);
    if (snapshot.loaded)
        paint.Text(I18n::Format(Text::ModsUsage, {Size(snapshot.mods_bytes), Size(snapshot.free_space)}),
                   right - 460, y + 64, 14, Palette::TextSecondary, 460, 22, false, false, false, true);
}

void DrawTabs(UiPainter& paint, const ModsView& view, float x, float y) {
    float left = x + L::Margin;
    paint.ButtonGlyph(L"LB", left, y + 3, ButtonColor::Neutral);
    left += 50;
    const std::wstring labels[ModsTabCount] = {
        std::wstring{Tr(Text::ModsTabInstalled)} + L"  " + std::to_wstring(view.session.View().mods.size()),
        std::wstring{Tr(Text::ModsTabExplore)}, std::wstring{Tr(Text::ModsTabFiles)}};
    for (std::size_t i = 0; i < ModsTabCount; ++i) {
        const bool active = static_cast<std::size_t>(view.page.tab) == i;
        const float width = paint.MeasureText(labels[i], 17, true) + 36;
        paint.Round(left, y, width, L::TabHeight, L::TabHeight / 2, active ? Palette::Accent : Palette::Row);
        paint.Text(labels[i], left, y, 17, active ? Palette::OnAccent : Palette::TextSecondary, width,
                   L::TabHeight, true, false, true);
        left += width + 10;
    }
    paint.ButtonGlyph(L"RB", left + 2, y + 3, ButtonColor::Neutral);
    // Downloads keep going on the other tabs: their progress stays in sight.
    const auto downloads = view.session.Downloads();
    if (!downloads.empty() && view.page.tab != ModsTab::Explore) {
        const auto& first = downloads.front();
        std::wstring text = I18n::Plural(Text::ModsDownloadsOne, Text::ModsDownloadsOther, downloads.size());
        if (first.running && first.total > 0)
            text += Dot + std::to_wstring(std::min<std::uint64_t>(first.done * 100 / first.total, 100)) + L"%";
        paint.Text(text, x + L::Width - L::Margin - 300, y + 8, 15, Palette::Accent, 300, 24, false, false, false, true);
    }
}

void DrawScrollMarks(UiPainter& paint, std::size_t first, std::size_t count, float x, float y) {
    paint.ScrollIndicator(x + L::Width - L::Margin + 14, y + L::RowTop,
                          L::VisibleRows * L::RowStride - 6, first, L::VisibleRows, count);
}

void DrawMessage(UiPainter& paint, UiIcon icon, std::wstring_view message, float x, float y) {
    const float top = y + L::RowTop + 90;
    paint.Icon(icon, x + L::Width / 2 - 16, top - 50, Palette::TextSecondary);
    paint.Text(message, x + L::Margin + 60, top, 20, Palette::TextHint, RowWidth - 120, 120, false, true, true);
}

std::wstring Origin(const ModSource& source) {
    switch (source.kind) {
    case SourceKind::Local: return std::wstring{Tr(Text::ModsSourceLocal)};
    case SourceKind::Archive: return I18n::Format(Text::ModsSourceArchive, {Utf8ToUtf16(source.archive)});
    case SourceKind::GameBanana: return I18n::Format(Text::ModsSourceGameBananaFile, {Utf8ToUtf16(source.archive)});
    case SourceKind::Cnx: return I18n::Format(Text::ModsSourceCnxFile, {Utf8ToUtf16(source.archive)});
    }
    return {};
}

void DrawInstalled(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& mods = view.session.View().mods;
    if (mods.empty()) {
        DrawMessage(paint, UiIcon::Mods, Tr(view.session.View().loaded ? Text::ModsEmpty : Text::ModsLoading), x, y);
        return;
    }
    const auto& page = view.page;
    const std::size_t end = std::min(mods.size(), page.first + L::VisibleRows);
    for (std::size_t i = page.first; i < end; ++i) {
        const auto& mod = mods[i];
        const float top = y + L::RowTop + static_cast<float>(i - page.first) * L::RowStride;
        const float left = x + L::Margin;
        const bool active = i == page.selected;
        const bool on = mod.record.enabled;
        // A mod being moved is lifted: an accent rim around its row.
        if (active && page.moving) paint.Round(left - 3, top - 3, RowWidth + 6, L::RowHeight + 6, 15, Palette::Accent);
        paint.Round(left, top, RowWidth, L::RowHeight, 12, active ? Palette::RowActive : Palette::Row);
        if (active && !page.moving) paint.Round(left, top + 12, 3, L::RowHeight - 24, 1.5f, Palette::Accent);
        // Its place in the order: 1 wins over 2.
        paint.Round(left + 16, top + 14, 28, 28, 14, on ? Palette::Track : Palette::Row);
        paint.Text(std::to_wstring(i + 1), left + 16, top + 14, 14, on ? Palette::Text : Palette::TextSecondary,
                   28, 28, true, false, true);
        paint.Text(Utf8ToUtf16(mod.record.name), left + 60, top + 5, 19,
                   on || active ? Palette::Text : Palette::TextHint, RowWidth - 170, 28, active);
        std::wstring detail = on ? Parts(mod.contents.parts) : std::wstring{Tr(Text::ModsOff)};
        const float detail_width = paint.MeasureText(detail, 14, false);
        paint.Text(detail, left + 60, top + 31, 14, Palette::TextSecondary, RowWidth - 170, 22);
        // What needs the player's eye, after the parts: a problem first, then an update.
        std::wstring flag;
        unsigned flag_color = Palette::Warning;
        if (on && mod.compat == Compat::OtherVersion) flag = Tr(Text::ModsOtherVersion);
        else if (on && !mod.conflicts.empty())
            flag = I18n::Plural(Text::ModsConflictsOne, Text::ModsConflictsOther, mod.conflicts.size());
        else if (view.session.HasUpdate(i)) {
            flag = Tr(Text::ModsUpdateAvailable);
            flag_color = Palette::Accent;
        }
        if (!flag.empty())
            paint.Text(L"·  " + flag, left + 68 + detail_width, top + 31, 14, flag_color,
                       RowWidth - 180 - detail_width, 22);
        paint.Switch(left + RowWidth - 76, top + 14, on, active);
    }
    DrawScrollMarks(paint, page.first, mods.size(), x, y);
}

/// Under the Installed list: the selected mod in plain words.
void DrawInstalledDetail(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& mods = view.session.View().mods;
    const float left = x + L::Margin;
    const float top = y + L::Detail;
    if (view.page.moving) {
        paint.Text(Tr(Text::ModsMovingHint), left, top, 16, Palette::Accent, RowWidth, 24);
        return;
    }
    if (view.page.selected >= mods.size()) return;
    const auto& mod = mods[view.page.selected];
    std::wstring line;
    unsigned color = Palette::TextHint;
    if (mod.record.enabled && mod.compat == Compat::OtherVersion) {
        line = Tr(Text::ModsOtherVersion);
        color = Palette::Warning;
    } else if (mod.record.enabled && !mod.conflicts.empty()) {
        // The heaviest conflict first; one line is room for two.
        auto conflicts = mod.conflicts;
        std::sort(conflicts.begin(), conflicts.end(), [](const Conflict& a, const Conflict& b) { return a.files > b.files; });
        for (std::size_t i = 0; i < std::min<std::size_t>(conflicts.size(), 2); ++i) {
            const auto& c = conflicts[i];
            if (!line.empty()) line += Dot;
            line += I18n::Format(c.wins ? Text::ModsWins : Text::ModsLoses,
                                 {Utf8ToUtf16(mods[c.other].record.name), std::to_wstring(c.files)});
            if (!c.wins) color = Palette::Warning;
        }
    } else if (mod.record.enabled) {
        line = Tr(mod.compat == Compat::Matches ? Text::ModsForThisVersion : Text::ModsNoConflicts);
        color = Palette::Accent;
    }
    // Then what it changes, which the conflicts already speak of when there are some.
    if (const auto changes = ChangesLine(mod.contents); !changes.empty() && (line.empty() || color == Palette::Accent)) {
        const float status = line.empty() ? 0 : paint.MeasureText(line + Dot, 16, false);
        if (!line.empty()) paint.Text(line + Dot, left, top, 16, color, RowWidth, 24);
        paint.Text(changes, left + status, top, 16, Palette::Text, RowWidth - status, 24);
    } else {
        paint.Text(line, left, top, 16, color, RowWidth, 24);
    }
    std::wstring origin = Origin(mod.record.source);
    if (!mod.record.source.version.empty() && mod.record.source.kind == SourceKind::GameBanana)
        origin += Dot + I18n::Format(Text::ModsVersion, {Utf8ToUtf16(mod.record.source.version)});
    origin += Dot + Size(mod.contents.bytes);
    const bool notice = !view.notice.empty();
    paint.Text(notice ? std::wstring{view.notice} : origin + std::wstring{Dot} + std::wstring{Tr(Text::ModsAppliesAtBoot)},
               left, top + 28, 14, notice ? Palette::Warning : Palette::TextSecondary, RowWidth, 22);
}

void DrawFiles(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& snapshot = view.session.View();
    const auto& rows = snapshot.files;
    if (rows.empty()) {
        DrawMessage(paint, UiIcon::Archive, Tr(Text::ModsFolderEmpty), x, y);
    } else {
        const auto& page = view.page;
        const std::size_t end = std::min(rows.size(), page.first + L::VisibleRows);
        for (std::size_t i = page.first; i < end; ++i) {
            const auto& row = rows[i];
            const float top = y + L::RowTop + static_cast<float>(i - page.first) * L::RowStride;
            const bool active = i == page.selected;
            const UiIcon icon = row.kind == FileRow::Kind::Archive ? UiIcon::Archive : UiIcon::Folder;
            paint.RowFrame(x + L::Margin, top, RowWidth, L::RowHeight, active, icon);
            const unsigned color = row.kind == FileRow::Kind::AddFolder ? Palette::Accent : Palette::Text;
            paint.Text(row.name, x + L::Margin + 68, top + 14, 19, color, RowWidth - 120, 28, active);
            if (row.kind != FileRow::Kind::Archive)
                paint.Chevron(x + L::Margin + RowWidth - 28, top + L::RowHeight / 2,
                              active ? Palette::Accent : Palette::TextSecondary);
        }
        DrawScrollMarks(paint, page.first, rows.size(), x, y);
    }
    const float top = y + L::Detail;
    const std::wstring where = snapshot.folder.empty() ? std::wstring{Tr(Text::ModsSourcesHint)} : snapshot.folder_name;
    paint.Text(where, x + L::Margin, top, 15, Palette::TextHint, RowWidth, 44, false, true);
    if (!view.notice.empty())
        paint.Text(view.notice, x + L::Margin, top + 30, 15, Palette::Warning, RowWidth, 22);
}

// Explore.

float CardX(std::size_t column, float x) {
    return x + L::Margin + static_cast<float>(column) * (L::CardWidth + L::CardGapX);
}

float CardY(std::size_t row, float y) {
    return y + L::GridTop + static_cast<float>(row) * (L::CardHeight + L::CardGapY);
}

/// The store's picture for `url` in the rectangle, or a tile with `icon` while it downloads.
void DrawPicture(UiPainter& paint, const ModSession& session, const std::string& url, UiIcon icon,
                 float x, float y, float w, float h) {
    const auto file = url.empty() ? std::filesystem::path{} : session.Pictures().Get(url);
    if (!file.empty() && paint.Picture(file, x, y, w, h)) return;
    paint.Round(x, y, w, h, 12, Palette::Track);
    paint.Icon(icon, x + w / 2 - 16, y + h / 2 - 16, Palette::TextSecondary);
}

void DrawCard(UiPainter& paint, const ModsView& view, const StoreCard& card, bool active, bool installed,
              float x, float y) {
    const float w = L::CardWidth;
    if (active) paint.Round(x - 4, y - 4, w + 8, L::CardHeight + 8, 16, Palette::Accent);
    paint.Round(x, y, w, L::CardHeight, 12, active ? Palette::RowActive : Palette::Row);
    const UiIcon icon = card.translation ? UiIcon::Language : UiIcon::Mods;
    DrawPicture(paint, view.session, card.thumbnail, icon, x, y, w, L::PictureHeight);
    if (card.sensitive) Chip(paint, Tr(Text::ModsSensitive), x + 8, y + 8, Palette::Warning, Palette::OnAccent);
    if (installed) {
        const auto label = Tr(Text::ModsInstalledBadge);
        Chip(paint, label, x + w - 24 - paint.MeasureText(label, 13, true), y + 8, Palette::Accent, Palette::OnAccent);
    }
    paint.Text(Utf8ToUtf16(card.name), x + 12, y + L::PictureHeight + 8, 16, Palette::Text, w - 24, 24, true);
    std::wstring meta;
    if (view.session.Store().source == StoreSource::Cnx) {
        meta = std::wstring{Tr(card.translation ? Text::ModsCnxTranslation : Text::ModsCnxModification)};
        if (!card.size.empty()) meta += Dot + Utf8ToUtf16(card.size);
    } else {
        meta = L"♥ " + Count(card.likes) + Dot + I18n::Format(Text::ModsViews, {Count(card.views)});
        if (!card.author.empty()) meta += Dot + Utf8ToUtf16(card.author);
    }
    paint.Text(meta, x + 12, y + L::PictureHeight + 32, 13, Palette::TextSecondary, w - 24, 20);
}

void DrawExplore(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& store = view.session.Store();
    const auto& page = view.page;
    if (store.cards.empty()) {
        switch (store.status) {
        case StoreStatus::Offline:
            DrawMessage(paint, UiIcon::Mods, I18n::Format(Text::ModsStoreOffline, {Tr(SourceText(store.source))}), x, y);
            return;
        case StoreStatus::Empty:
            DrawMessage(paint, UiIcon::Mods,
                        Tr(store.search.empty() && store.category.empty() && !store.adult ? Text::ModsStoreEmpty
                                                                                           : Text::ModsStoreNoResults),
                        x, y);
            return;
        case StoreStatus::NoGame:
            DrawMessage(paint, UiIcon::Game, I18n::Format(Text::ModsStoreNoGame, {view.game}), x, y);
            return;
        default:
            // Cards to come: their outlines, so the grid does not jump when they arrive.
            for (std::size_t i = 0; i < L::Columns * L::GridRows; ++i) {
                const float cx = CardX(i % L::Columns, x), cy = CardY(i / L::Columns, y);
                paint.Round(cx, cy, L::CardWidth, L::CardHeight, 12, Palette::Row);
                paint.Round(cx, cy, L::CardWidth, L::PictureHeight, 12, Palette::Track);
                paint.Round(cx + 12, cy + L::PictureHeight + 12, L::CardWidth * 0.6f, 12, 6, Palette::Track);
            }
            return;
        }
    }
    // Mods of this game already installed from the store wear a badge.
    std::set<std::int64_t> installed;
    for (const auto& mod : view.session.View().mods)
        if (mod.record.source.kind == SourceKind::GameBanana) installed.insert(mod.record.source.mod_id);
    const bool gamebanana = store.source == StoreSource::GameBanana;
    const std::size_t end = std::min(store.cards.size(), page.card_first + L::Columns * L::GridRows);
    for (std::size_t i = page.card_first; i < end; ++i) {
        const std::size_t slot = i - page.card_first;
        const auto& card = store.cards[i];
        DrawCard(paint, view, card, i == page.card, gamebanana && installed.contains(card.id),
                 CardX(slot % L::Columns, x), CardY(slot / L::Columns, y));
    }
    paint.ScrollIndicator(x + L::Width - L::Margin + 14, y + L::GridTop,
                          L::GridRows * L::CardHeight + (L::GridRows - 1) * L::CardGapY, page.card_first,
                          L::Columns * L::GridRows, store.cards.size(), store.loading_more);
}

/// Under the grid: where the mods come from and how they are filtered, then the downloads.
void DrawExploreFooter(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& store = view.session.Store();
    const float left = x + L::Margin;
    const float top = y + L::Detail;
    std::wstring line{Tr(SourceText(store.source))};
    if (store.source == StoreSource::GameBanana) {
        if (store.game) line += Dot + Utf8ToUtf16(store.game->name);
        if (store.adult || !store.category.empty()) line += Dot + CategoryLabel(store);
        line += Dot;
        line += Tr(SortText(store.sort));
    }
    if (!store.search.empty()) line += Dot + L"“" + Utf8ToUtf16(store.search) + L"”";
    if (store.hidden) line += Dot + I18n::Plural(Text::ModsHiddenOne, Text::ModsHiddenOther, store.hidden);
    if (store.loading_more) line += Dot + std::wstring{Tr(Text::ModsLoadingMore)};
    paint.Text(line, left, top, 15, Palette::TextHint, RowWidth, 24);
    const auto downloads = view.session.Downloads();
    if (!downloads.empty()) {
        paint.Text(DownloadLine(downloads), left, top + 24, 14, Palette::Accent, RowWidth, 22);
        const auto& first = downloads.front();
        Bar(paint, left, top + 48, RowWidth, first.done, first.total);
    } else if (!view.notice.empty()) {
        paint.Text(view.notice, left, top + 26, 15, Palette::Warning, RowWidth, 22);
    }
}

/// A card over the dimmed page.
void Sheet(UiPainter& paint, float x, float y, float width, float height) {
    paint.Round(x, y, L::Width, L::Height, 24, 0x0b111a);
    paint.Round(x + (L::Width - width) / 2, y + (L::Height - height) / 2, width, height, 20, Palette::Panel);
}

void DrawDetail(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& store = view.session.Store();
    paint.Round(x, y, L::Width, L::Height, 24, Palette::Panel);
    if (!store.detail) {
        const auto text = store.detail_failed ? I18n::Format(Text::ModsStoreOffline, {Tr(Text::ModsSourceGameBanana)})
                                              : std::wstring{Tr(Text::ModsDetailLoading)};
        paint.Icon(UiIcon::Mods, x + L::Width / 2 - 16, y + 240, Palette::TextSecondary);
        paint.Text(text, x + L::Margin, y + 290, 20, Palette::TextHint, RowWidth, 60, false, true, true);
        return;
    }
    const auto& detail = *store.detail;
    const auto& page = view.page;
    // Left: the pictures and the numbers.
    const float left = x + L::Margin;
    const float top = y + 36;
    constexpr float picture_w = 480, picture_h = 270;
    const auto& images = detail.images;
    const std::string url = images.empty() ? std::string{} : images[std::min(page.image, images.size() - 1)];
    DrawPicture(paint, view.session, url, detail.card.translation ? UiIcon::Language : UiIcon::Mods,
                left, top, picture_w, picture_h);
    if (images.size() > 1) {
        const auto count = std::to_wstring(std::min(page.image, images.size() - 1) + 1) + L" / " +
                           std::to_wstring(images.size());
        paint.ButtonGlyph(L"LB", left, top + picture_h + 10, ButtonColor::Neutral);
        paint.Text(count, left + 44, top + picture_h + 12, 16, Palette::TextSecondary, picture_w - 88, 28, false,
                   false, true);
        paint.ButtonGlyph(L"RB", left + picture_w - 38, top + picture_h + 10, ButtonColor::Neutral);
    }
    float line_y = top + picture_h + 52;
    const auto stat_line = [&](const std::wstring& text, unsigned color) {
        if (text.empty()) return;
        paint.Text(text, left, line_y, 15, color, picture_w, 24);
        line_y += 26;
    };
    if (detail.source == StoreSource::GameBanana) {
        stat_line(L"♥ " + Count(detail.card.likes) + Dot +
                      I18n::Format(Text::ModsDownloadsCount, {Count(detail.downloads)}) + Dot +
                      I18n::Format(Text::ModsViews, {Count(detail.card.views)}),
                  Palette::Text);
        std::wstring by;
        if (!detail.version.empty()) by = I18n::Format(Text::ModsVersion, {Utf8ToUtf16(detail.version)});
        if (!detail.card.author.empty()) {
            if (!by.empty()) by += Dot;
            by += I18n::Format(Text::ModsBy, {Utf8ToUtf16(detail.card.author)});
        }
        stat_line(by, Palette::TextSecondary);
        stat_line(Utf8ToUtf16(detail.card.category), Palette::TextSecondary);
    } else {
        stat_line(std::wstring{Tr(detail.card.translation ? Text::ModsCnxTranslation : Text::ModsCnxModification)} +
                      Dot + Utf8ToUtf16(detail.card.size),
                  Palette::Text);
    }
    // Right: the name, the description and the files.
    const float right = x + 560;
    const float right_w = L::Width - 560 - L::Margin;
    paint.Text(Utf8ToUtf16(detail.card.name), right, top - 4, 24, Palette::Text, right_w, 64, true, true);
    float text_top = top + 64;
    if (!store.replace.empty()) {
        paint.Text(Tr(Text::ModsUpdateBanner), right, text_top, 15, Palette::Accent, right_w, 44, false, true);
        text_top += 48;
    }
    const std::wstring text = detail.text.empty() ? std::wstring{Tr(Text::ModsCnxDescription)} : Utf8ToUtf16(detail.text);
    paint.Text(text, right, text_top, 15, Palette::TextHint, right_w, top + 286 - text_top, false, true);
    paint.Text(Tr(Text::ModsFiles), right, top + 296, 17, Palette::Text, right_w, 26, true);
    const auto downloads = view.session.Downloads();
    constexpr std::size_t visible = 4;
    const std::size_t first = page.file >= visible ? page.file + 1 - visible : 0;
    const std::size_t end = std::min(detail.files.size(), first + visible);
    for (std::size_t i = first; i < end; ++i) {
        const auto& file = detail.files[i];
        const float row = top + 330 + static_cast<float>(i - first) * 56;
        const bool active = i == page.file;
        paint.Round(right, row, right_w, 50, 10, active ? Palette::RowActive : Palette::Row);
        if (active) paint.Round(right, row + 12, 3, 26, 1.5f, Palette::Accent);
        paint.Text(Utf8ToUtf16(file.name), right + 14, row + 4, 16, Palette::Text, right_w - 28, 24, active);
        std::wstring meta;
        unsigned color = Palette::TextSecondary;
        const auto title = detail.card.name + " \xC2\xB7 " + file.name;
        const auto running = std::find_if(downloads.begin(), downloads.end(),
                                          [&](const DownloadStatus& d) { return d.title == title; });
        if (running != downloads.end()) {
            meta = running->running && running->total
                       ? I18n::Format(Text::ModsDownloadingFile,
                                      {std::to_wstring(std::min<std::uint64_t>(running->done * 100 / running->total, 100)) + L"%"})
                       : std::wstring{Tr(Text::ModsQueuedFile)};
            color = Palette::Accent;
        } else if (!file.safe) {
            meta = Tr(Text::ModsFileUnsafe);
            color = Palette::Warning;
        } else {
            meta = file.size ? Size(file.size) : std::wstring{};
            if (const auto date = Date(file.added); !date.empty()) meta += (meta.empty() ? L"" : std::wstring{Dot}) + date;
            if (file.downloads > 0)
                meta += Dot + I18n::Format(Text::ModsDownloadsCount, {Count(file.downloads)});
            if (!file.description.empty()) meta += Dot + Utf8ToUtf16(file.description);
        }
        paint.Text(meta, right + 14, row + 27, 13, color, right_w - 28, 20);
    }
    paint.ScrollIndicator(right + right_w + 14, top + 330, visible * 56 - 6,
                          first, visible, detail.files.size());
    if (detail.files.empty())
        paint.Text(Tr(Text::ModsNoFiles), right, top + 336, 15, Palette::TextHint, right_w, 24);
}

void DrawStoreMenu(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& store = view.session.Store();
    const auto downloads = view.session.Downloads();
    const auto actions = StoreActions({store.source == StoreSource::GameBanana, !store.search.empty(), !downloads.empty()});
    constexpr float width = 700;
    const float height = 90 + static_cast<float>(actions.size()) * 62;
    Sheet(paint, x, y, width, height);
    const float left = x + (L::Width - width) / 2 + 30;
    const float top = y + (L::Height - height) / 2 + 24;
    const float inner = width - 60;
    paint.Text(Tr(Text::ModsStoreMenuTitle), left, top, 22, Palette::Text, inner, 32, true);
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const float row = top + 50 + static_cast<float>(i) * 62;
        const bool active = i == view.page.menu;
        paint.Round(left, row, inner, 54, 12, active ? Palette::RowActive : Palette::Row);
        if (active) paint.Round(left, row + 14, 3, 26, 1.5f, Palette::Accent);
        Text label{};
        std::wstring value;
        switch (actions[i]) {
        case StoreAction::Category:
            label = Text::ModsStoreCategory;
            value = CategoryLabel(store);
            break;
        case StoreAction::Sort:
            label = Text::ModsStoreSort;
            value = Tr(SortText(store.sort));
            break;
        case StoreAction::Search:
            label = Text::ModsStoreSearch;
            value = store.search.empty() ? L"—" : Utf8ToUtf16(store.search);
            break;
        case StoreAction::ClearSearch:
            label = Text::ModsStoreClearSearch;
            break;
        case StoreAction::Source:
            label = Text::ModsStoreSource;
            value = Tr(SourceText(store.source));
            break;
        case StoreAction::ChangeGame:
            label = Text::ModsStoreGame;
            value = store.game ? Utf8ToUtf16(store.game->name) : L"—";
            break;
        case StoreAction::CancelDownloads:
            label = Text::ModsStoreCancel;
            value = std::to_wstring(downloads.size());
            break;
        }
        paint.Text(Tr(label), left + 20, row + 13, 19, Palette::Text, inner / 2, 28, active);
        paint.Text(value, left + inner / 2, row + 15, 17, active ? Palette::Accent : Palette::TextSecondary,
                   inner / 2 - 20, 26, false, false, false, true);
    }
}

void DrawKeyboard(UiPainter& paint, const ModsView& view, float x, float y) {
    if (!view.page.keyboard) return;
    using K = TextEntryPanelLayout;
    paint.Round(x, y, L::Width, L::Height, 24, 0x0b111a);
    const float left = x + (L::Width - K::Width) / 2;
    paint.Round(left, y, K::Width, 600, 22, 0x111923);
    const bool search = view.page.keyboard_for == KeyboardFor::Search;
    DrawTextEntryPanel(paint, {*view.page.keyboard, Tr(search ? Text::ModsSearchTitle : Text::ModsGameTitle),
                               Tr(search ? Text::ModsSearchHint : Text::ModsGameHint), {}},
                       left, y);
}

void DrawGames(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& store = view.session.Store();
    constexpr float width = 760, height = 520;
    Sheet(paint, x, y, width, height);
    const float left = x + (L::Width - width) / 2 + 30;
    const float top = y + (L::Height - height) / 2 + 24;
    const float inner = width - 60;
    paint.Text(Tr(Text::ModsGamesTitle), left, top, 22, Palette::Text, inner, 32, true);
    if (store.searching_games || store.games.empty()) {
        paint.Text(Tr(store.searching_games ? Text::ModsGamesSearching : Text::ModsGamesNone), left, top + 180, 18,
                   Palette::TextHint, inner, 60, false, true, true);
        return;
    }
    constexpr std::size_t visible = 6;
    const std::size_t first = view.page.game >= visible ? view.page.game + 1 - visible : 0;
    const std::size_t end = std::min(store.games.size(), first + visible);
    for (std::size_t i = first; i < end; ++i) {
        const float row = top + 52 + static_cast<float>(i - first) * 68;
        paint.RowFrame(left, row, inner, 60, i == view.page.game, UiIcon::Game);
        paint.Text(Utf8ToUtf16(store.games[i].name), left + 68, row + 16, 19, Palette::Text, inner - 90, 28,
                   i == view.page.game);
    }
    paint.ScrollIndicator(left + inner + 14, top + 52, visible * 68 - 8,
                          first, visible, store.games.size());
}

void DrawActions(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& mods = view.session.View().mods;
    if (view.page.selected >= mods.size()) return;
    const auto actions = InstalledActions(view.session.HasUpdate(view.page.selected));
    constexpr float width = 560;
    const float height = 122 + static_cast<float>(actions.size()) * 64;
    Sheet(paint, x, y, width, height);
    const float left = x + (L::Width - width) / 2 + 30;
    const float top = y + (L::Height - height) / 2 + 26;
    paint.Text(Utf8ToUtf16(mods[view.page.selected].record.name), left, top, 22, Palette::Text, width - 60, 32, true);
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const float row = top + 56 + static_cast<float>(i) * 64;
        const bool active = i == view.page.action;
        Text label{};
        UiIcon icon{};
        switch (actions[i]) {
        case ModAction::Contents: label = Text::ModsActionContents; icon = UiIcon::Folder; break;
        case ModAction::Update: label = Text::ModsActionUpdate; icon = UiIcon::Archive; break;
        case ModAction::Move: label = Text::ModsActionMove; icon = UiIcon::Mods; break;
        case ModAction::Remove: label = Text::ModsActionRemove; icon = UiIcon::Remove; break;
        }
        paint.RowFrame(left, row, width - 60, 54, active, icon);
        paint.Text(Tr(label), left + 68, row + 13, 19, Palette::Text, width - 140, 28, active);
    }
}

/// Every file of the selected mod, with its size; the largest stand out.
void DrawContents(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& mods = view.session.View().mods;
    if (view.page.selected >= mods.size()) return;
    const auto& mod = mods[view.page.selected];
    const auto& listing = mod.contents.listing;
    constexpr float width = 900, height = 600;
    Sheet(paint, x, y, width, height);
    const float left = x + (L::Width - width) / 2 + 30;
    const float top = y + (L::Height - height) / 2 + 24;
    const float inner = width - 60;
    paint.Text(Utf8ToUtf16(mod.record.name), left, top, 22, Palette::Text, inner, 32, true);
    paint.Text(I18n::Plural(Text::ModsFilesOne, Text::ModsFilesOther, listing.size()) + Dot + Size(mod.contents.bytes),
               left, top + 36, 15, Palette::TextSecondary, inner, 22);
    paint.Text(Tr(Text::ModsContentsHint), left, top + 60, 14, Palette::TextHint, inner, 40, false, true);
    const std::size_t first = std::min(view.page.line, listing.size());
    const std::size_t end = std::min(listing.size(), first + L::SheetRows);
    for (std::size_t i = first; i < end; ++i) {
        const auto& file = listing[i];
        const float row = top + 108 + static_cast<float>(i - first) * 50;
        paint.Round(left, row, inner, 44, 10, Palette::Row);
        const auto slash = file.path.rfind('/');
        const std::wstring folder = slash == std::string::npos ? L"" : Utf8ToUtf16(file.path.substr(0, slash + 1));
        const float folder_w = folder.empty() ? 0 : paint.MeasureText(folder, 16, false);
        if (!folder.empty()) paint.Text(folder, left + 16, row + 10, 16, Palette::TextSecondary, inner - 150, 24);
        paint.Text(Utf8ToUtf16(file.path.substr(slash == std::string::npos ? 0 : slash + 1)), left + 16 + folder_w,
                   row + 10, 16, Palette::Text, std::max(inner - 150 - folder_w, 60.0f), 24, true);
        paint.Text(Size(file.bytes), left + inner - 130, row + 11, 15, Palette::TextSecondary, 114, 22, false, false,
                   false, true);
    }
    paint.ScrollIndicator(left + inner + 14, top + 108, L::SheetRows * 50 - 6,
                          first, L::SheetRows, listing.size());
}

/// The store's categories at the level browsed; the one listed is marked.
void DrawCategories(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& store = view.session.Store();
    const auto rows = view.session.CategoryRows();
    constexpr float width = 760, height = 600;
    Sheet(paint, x, y, width, height);
    const float left = x + (L::Width - width) / 2 + 30;
    const float top = y + (L::Height - height) / 2 + 24;
    const float inner = width - 60;
    std::wstring title{Tr(Text::ModsCategoriesTitle)};
    for (const auto& category : store.browse) title += L" › " + Utf8ToUtf16(category.name);
    paint.Text(title, left, top, 22, Palette::Text, inner, 32, true);
    if (rows.empty()) {
        const auto text = store.categories_failed
                              ? I18n::Format(Text::ModsStoreOffline, {Tr(Text::ModsSourceGameBanana)})
                              : std::wstring{Tr(Text::ModsCategoriesLoading)};
        paint.Text(text, left, top + 220, 18, Palette::TextHint, inner, 60, false, true, true);
        return;
    }
    const auto& page = view.page;
    const std::size_t cursor = std::min(page.category, rows.size() - 1);
    const std::size_t first = cursor >= L::SheetRows ? cursor + 1 - L::SheetRows : 0;
    const std::size_t end = std::min(rows.size(), first + L::SheetRows);
    for (std::size_t i = first; i < end; ++i) {
        const auto& row = rows[i];
        const float row_y = top + 52 + static_cast<float>(i - first) * 70;
        const bool active = i == cursor;
        paint.Round(left, row_y, inner, 62, 12, active ? Palette::RowActive : Palette::Row);
        if (active) paint.Round(left, row_y + 16, 3, 30, 1.5f, Palette::Accent);
        std::wstring name, meta;
        bool nested = false, shown = false;
        switch (row.kind) {
        case CategoryRow::Kind::All:
            name = store.browse.empty() ? std::wstring{Tr(Text::ModsCategoryAll)}
                                        : I18n::Format(Text::ModsCategoryAllOf, {Utf8ToUtf16(store.browse.back().name)});
            shown = !store.adult && store.category.size() == store.browse.size() &&
                    std::equal(store.browse.begin(), store.browse.end(), store.category.begin(),
                               [](const auto& a, const auto& b) { return a.id == b.id; });
            break;
        case CategoryRow::Kind::Adult:
            name = Tr(Text::ModsCategoryAdult);
            meta = Tr(Text::ModsCategoryAdultHint);
            shown = store.adult;
            break;
        case CategoryRow::Kind::Category: {
            const auto& category = store.categories[row.index];
            name = Utf8ToUtf16(category.name);
            meta = I18n::Plural(Text::ModsModsOne, Text::ModsModsOther, static_cast<std::uint64_t>(category.mods));
            nested = category.nested;
            shown = !store.adult && !store.category.empty() && store.category.back().id == category.id;
            break;
        }
        }
        float text_left = left + 20;
        if (row.kind == CategoryRow::Kind::Adult) {
            Chip(paint, Tr(Text::ModsSensitive), left + 18, row_y + 20, Palette::Warning, Palette::OnAccent);
            text_left += paint.MeasureText(Tr(Text::ModsSensitive), 13, true) + 28;
        }
        paint.Text(name, text_left, row_y + (meta.empty() ? 17 : 6), 19, shown ? Palette::Accent : Palette::Text,
                   inner - (text_left - left) - 60, 28, active || shown);
        if (!meta.empty())
            paint.Text(meta, text_left, row_y + 34, 14, Palette::TextSecondary, inner - (text_left - left) - 60, 22);
        if (shown) paint.Text(L"✓", left + inner - 48, row_y + 17, 20, Palette::Accent, 28, 28, true, false, true);
        else if (nested) paint.Chevron(left + inner - 30, row_y + 31, active ? Palette::Accent : Palette::TextSecondary);
    }
    paint.ScrollIndicator(left + inner + 14, top + 52, L::SheetRows * 70 - 8,
                          first, L::SheetRows, rows.size());
}

void DrawRemove(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& mods = view.session.View().mods;
    if (view.page.selected >= mods.size()) return;
    constexpr float width = 640, height = 180;
    Sheet(paint, x, y, width, height);
    const float left = x + (L::Width - width) / 2 + 30;
    const float top = y + (L::Height - height) / 2 + 30;
    paint.Icon(UiIcon::Remove, left, top + 6, Palette::Warning);
    paint.Text(I18n::Format(Text::ModsRemoveConfirm, {Utf8ToUtf16(mods[view.page.selected].record.name)}),
               left + 48, top, 20, Palette::Text, width - 110, 120, false, true);
}

void DrawVariants(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& snapshot = view.session.View();
    const auto& candidates = snapshot.candidates;
    const bool several = candidates.size() > 1;
    constexpr float width = 860, height = 540;
    Sheet(paint, x, y, width, height);
    const float left = x + (L::Width - width) / 2 + 30;
    const float top = y + (L::Height - height) / 2 + 24;
    const float inner = width - 60;
    paint.Icon(UiIcon::Archive, left, top + 2, Palette::Accent);
    paint.Text(snapshot.archive, left + 46, top, 22, Palette::Text, inner - 46, 34, true);
    paint.Text(Tr(several ? Text::ModsVariantsHint : Text::ModsInstallOneHint), left, top + 42, 16,
               Palette::TextHint, inner, 26);
    constexpr std::size_t visible = 5;
    const std::size_t first = view.page.variant >= visible ? view.page.variant + 1 - visible : 0;
    const std::size_t end = std::min(candidates.size(), first + visible);
    for (std::size_t i = first; i < end; ++i) {
        const auto& candidate = candidates[i];
        const float row = top + 84 + static_cast<float>(i - first) * 76;
        const bool active = several && i == view.page.variant;
        const bool ticked = i < view.page.chosen.size() && view.page.chosen[i];
        paint.Round(left, row, inner, 68, 12, active ? Palette::RowActive : Palette::Row);
        if (active) paint.Round(left, row + 14, 3, 40, 1.5f, Palette::Accent);
        float text_left = left + 20;
        if (several) {
            // A tick box: filled when the variant installs.
            paint.Round(left + 18, row + 20, 28, 28, 7, ticked ? Palette::Accent : Palette::Track);
            if (ticked) paint.Text(L"✓", left + 18, row + 20, 18, Palette::OnAccent, 28, 28, true, false, true);
            text_left = left + 62;
        }
        paint.Text(Utf8ToUtf16(candidate.name), text_left, row + 8, 19, Palette::Text, inner - 90, 28, active);
        std::wstring detail = Parts(candidate.parts) + std::wstring{Dot} + Size(candidate.bytes);
        paint.Text(detail, text_left, row + 38, 14, Palette::TextSecondary, inner - 90, 22);
        if (candidate.other_title)
            paint.Text(Tr(Text::ModsOtherTitle), text_left + paint.MeasureText(detail, 14, false) + 12, row + 38, 14,
                       Palette::Warning, inner - 100 - paint.MeasureText(detail, 14, false), 22);
    }
    paint.ScrollIndicator(left + inner + 14, top + 84, visible * 76 - 8,
                          first, visible, candidates.size());
}

/// The running job, over the list: what it does and, while installing, how far it is.
void DrawBusy(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto& session = view.session;
    const float left = x + L::Margin;
    const float top = y + L::RowTop;
    paint.Round(left, top, RowWidth, L::RowStride * L::VisibleRows - 6, 16, Palette::Panel);
    std::wstring text;
    switch (session.Running()) {
    case ModJob::Installing:
        text = I18n::Format(Text::ModsInstalling, {Size(session.Done()), Size(session.Total())});
        break;
    case ModJob::Reading: text = Tr(Text::ModsReading); break;
    case ModJob::Browsing: text = Tr(Text::ModsBrowsing); break;
    case ModJob::Removing: text = Tr(Text::ModsRemoving); break;
    default: text = Tr(Text::ModsLoading); break;
    }
    const float middle = top + 120;
    paint.Icon(session.Running() == ModJob::Installing ? UiIcon::Archive : UiIcon::Mods, x + L::Width / 2 - 16,
               middle - 56, Palette::Accent);
    paint.Text(text, left, middle, 21, Palette::Text, RowWidth, 32, true, false, true);
    if (session.Running() == ModJob::Installing && !session.View().archive.empty())
        paint.Text(session.View().archive, left, middle + 34, 16, Palette::TextSecondary, RowWidth, 24, false, false, true);
    if (session.Running() == ModJob::Installing && session.Total() > 0) {
        constexpr float bar = 520;
        Bar(paint, x + (L::Width - bar) / 2, middle + 70, bar, session.Done(), session.Total());
    }
}

} // namespace

I18n::Text ErrorText(ModError error) {
    switch (error) {
    case ModError::NotAMod: return Text::ModsErrorNotAMod;
    case ModError::Encrypted: return Text::ModsErrorEncrypted;
    case ModError::Unreadable: return Text::ModsErrorUnreadable;
    case ModError::TooLarge: return Text::ModsErrorTooLarge;
    case ModError::Space: return Text::ModsErrorSpace;
    case ModError::Write: return Text::ModsErrorWrite;
    case ModError::Cancelled: return Text::ModsErrorCancelled;
    case ModError::Unavailable: return Text::ModsErrorUnavailable;
    case ModError::Network: return Text::ModsErrorNetwork;
    case ModError::Checksum: return Text::ModsErrorChecksum;
    case ModError::None: break;
    }
    return Text::ModsErrorWrite;
}

void DrawModsPanel(UiPainter& paint, const ModsView& view, float x, float y) {
    const auto screen = view.page.screen;
    // Full-panel screens draw over everything.
    if (!view.session.Busy() && screen == ModsScreen::Detail) {
        DrawDetail(paint, view, x, y);
        return;
    }
    paint.Round(x, y, L::Width, L::Height, 24, Palette::Panel);
    DrawHeader(paint, view, x, y);
    DrawTabs(paint, view, x, y + L::TabsTop);
    if (view.session.Busy()) {
        DrawBusy(paint, view, x, y);
        return;
    }
    switch (view.page.tab) {
    case ModsTab::Installed:
        DrawInstalled(paint, view, x, y);
        DrawInstalledDetail(paint, view, x, y);
        break;
    case ModsTab::Explore:
        DrawExplore(paint, view, x, y);
        DrawExploreFooter(paint, view, x, y);
        break;
    case ModsTab::Files:
        DrawFiles(paint, view, x, y);
        break;
    }
    switch (screen) {
    case ModsScreen::Actions: DrawActions(paint, view, x, y); break;
    case ModsScreen::Remove: DrawRemove(paint, view, x, y); break;
    case ModsScreen::Variants: DrawVariants(paint, view, x, y); break;
    case ModsScreen::StoreMenu: DrawStoreMenu(paint, view, x, y); break;
    case ModsScreen::Keyboard: DrawKeyboard(paint, view, x, y); break;
    case ModsScreen::Games: DrawGames(paint, view, x, y); break;
    case ModsScreen::Contents: DrawContents(paint, view, x, y); break;
    case ModsScreen::Categories: DrawCategories(paint, view, x, y); break;
    case ModsScreen::Detail: case ModsScreen::List: break;
    }
}

std::vector<ModsPrompt> ModsPrompts(const ModSession& session, const ModsPage& page) {
    using B = ModsPrompt::Button;
    if (session.Busy()) return {{B::B, Text::ModsPromptCancel}};
    const auto& store = session.Store();
    switch (page.screen) {
    case ModsScreen::Actions: return {{B::A, Text::Select}, {B::B, Text::Back}};
    case ModsScreen::Remove: return {{B::A, Text::ModsPromptUninstall}, {B::B, Text::ModsPromptCancel}};
    case ModsScreen::Variants:
        if (page.chosen.size() > 1)
            return {{B::A, Text::ModsPromptTick}, {B::X, Text::ModsPromptInstall}, {B::B, Text::ModsPromptCancel}};
        return {{B::A, Text::ModsPromptInstall}, {B::B, Text::ModsPromptCancel}};
    case ModsScreen::Detail: {
        std::vector<ModsPrompt> prompts;
        if (store.detail && !store.detail->files.empty()) prompts.push_back({B::A, Text::ModsPromptDownload});
        if (store.detail && store.detail->images.size() > 1) prompts.push_back({B::Bumpers, Text::ModsPromptPictures});
        prompts.push_back({B::B, Text::Back});
        return prompts;
    }
    case ModsScreen::StoreMenu: return {{B::A, Text::Select}, {B::B, Text::Back}};
    case ModsScreen::Keyboard:
        return {{B::A, Text::ModsPromptType}, {B::X, Text::ModsPromptDelete}, {B::Y, Text::ModsPromptSpace},
                {B::Menu, Text::ModsPromptDone}, {B::B, Text::ModsPromptCancel}};
    case ModsScreen::Games:
        if (store.games.empty()) return {{B::X, Text::ModsPromptSearch}, {B::B, Text::Back}};
        return {{B::A, Text::Select}, {B::X, Text::ModsPromptSearch}, {B::B, Text::Back}};
    case ModsScreen::Contents: return {{B::B, Text::Back}};
    case ModsScreen::Categories: {
        const auto rows = session.CategoryRows();
        if (rows.empty()) return {{B::B, Text::Back}};
        const auto& row = rows[std::min(page.category, rows.size() - 1)];
        const bool nested = row.kind == CategoryRow::Kind::Category && store.categories[row.index].nested;
        if (nested) return {{B::A, Text::ModsPromptOpen}, {B::X, Text::ModsPromptListAll}, {B::B, Text::Back}};
        return {{B::A, Text::Select}, {B::B, Text::Back}};
    }
    case ModsScreen::List: break;
    }
    if (page.moving) return {{B::A, Text::ModsPromptDrop}};
    switch (page.tab) {
    case ModsTab::Files:
        return {{B::A, Text::ModsPromptOpen}, {B::Bumpers, Text::ModsPromptTab}, {B::B, Text::Back}};
    case ModsTab::Explore: {
        std::vector<ModsPrompt> prompts;
        if (!store.cards.empty()) prompts.push_back({B::A, Text::ModsPromptOpen});
        else if (store.status == StoreStatus::NoGame) prompts.push_back({B::A, Text::ModsPromptFindGame});
        else if (store.status == StoreStatus::Offline) prompts.push_back({B::A, Text::ModsPromptRetry});
        prompts.push_back({B::X, Text::ModsPromptSearch});
        if (store.source == StoreSource::GameBanana) prompts.push_back({B::Menu, Text::ModsPromptCategories});
        prompts.push_back({B::Y, Text::ModsPromptOptions});
        prompts.push_back({B::Bumpers, Text::ModsPromptTab});
        prompts.push_back({B::B, Text::Back});
        return prompts;
    }
    case ModsTab::Installed:
        break;
    }
    std::vector<ModsPrompt> prompts;
    const auto count = session.View().mods.size();
    if (count) prompts.push_back({B::A, Text::ModsPromptToggle});
    if (count > 1) prompts.push_back({B::X, Text::ModsPromptMove});
    if (count) prompts.push_back({B::Y, Text::ModsPromptOptions});
    prompts.push_back({B::Bumpers, Text::ModsPromptTab});
    prompts.push_back({B::B, Text::Back});
    return prompts;
}

ModsCounts CountsFor(const ModSession& session, const ModsPage& page, std::wstring_view game_name) {
    const auto& state = session.View();
    const auto& store = session.Store();
    ModsCounts counts;
    counts.installed = state.mods.size();
    counts.files = state.files.size();
    counts.files_root = state.folder.empty();
    counts.busy = session.Busy();
    counts.update = session.HasUpdate(page.selected);
    counts.cards = store.cards.size();
    counts.no_game = store.status == StoreStatus::NoGame;
    counts.offline = store.status == StoreStatus::Offline;
    if (store.detail) {
        counts.detail_files = store.detail->files.size();
        counts.detail_images = store.detail->images.size();
    }
    counts.games = store.searching_games ? 0 : store.games.size();
    if (page.selected < state.mods.size()) counts.contents = state.mods[page.selected].contents.listing.size();
    for (const auto& row : session.CategoryRows())
        counts.category_nested.push_back(row.kind == CategoryRow::Kind::Category && store.categories[row.index].nested);
    counts.category_top = store.browse.empty();
    counts.store = {store.source == StoreSource::GameBanana, !store.search.empty(), !session.Downloads().empty()};
    counts.search = Utf8ToUtf16(store.search);
    counts.game_name = std::wstring{game_name};
    return counts;
}

} // namespace EdenXbox::Mods

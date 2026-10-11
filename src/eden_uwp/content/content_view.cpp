// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/content/content_view.h"
#include <iomanip>
#include <sstream>
#include "eden_uwp/ui_painter.h"
#include "eden_uwp/utf8.h"

namespace EdenXbox::Content {
using T = I18n::Text;
using I18n::Tr;
I18n::Text ErrorText(Error error) {
    switch (error) {
    case Error::None: return T::ContentReady;
    case Error::Cancelled: return T::ContentCancelled;
    case Error::Unavailable: return T::ContentUnavailable;
    case Error::Unsupported: return T::ContentUnsupported;
    case Error::Invalid: return T::ContentInvalid;
    case Error::MissingKeys: return T::ContentMissingKeys;
    case Error::Incomplete: return T::ContentIncomplete;
    case Error::HashMismatch: return T::ContentHashMismatch;
    case Error::WrongGame: return T::ContentWrongGame;
    case Error::Conflict: return T::ContentConflict;
    case Error::Space: return T::ContentSpace;
    case Error::Registry: return T::ContentRegistryError;
    case Error::Requirements: return T::ContentRequirements;
    default: return T::ContentIo;
    }
}
namespace {
std::wstring Label(const Revision& r) {
    std::wostringstream title;
    if (!r.name.empty()) title << Utf8ToUtf16(r.name);
    else title << Tr(r.kind == Kind::Update ? T::ContentUpdateItem : T::ContentDlcItem);
    title << L"  ·  ";
    if (!r.display_version.empty()) title << Utf8ToUtf16(r.display_version);
    else title << L"v" << r.version;
    if (r.name.empty()) title << L"  ·  " << std::uppercase << std::hex << std::setw(16) << std::setfill(L'0') << r.title;
    return title.str();
}
std::wstring Detail(const Revision& r, Error error) {
    std::wostringstream text;
    text << (error != Error::None ? Tr(ErrorText(error)) :
        Tr(r.enabled ? T::ContentActive : T::ContentInactive));
    text << L"  ·  " << Tr(r.internal ? T::ContentInternal : T::ContentUsb);
    text << L"  ·  " << r.bytes / (1024 * 1024) << L" MiB";
    return text.str();
}
} // namespace
void DrawMissingContent(UiPainter& paint) {
    paint.Round(120, 210, 1040, 320, 24, Palette::Panel);
    paint.Text(Tr(T::ContentMissingLaunch), 160, 245, 25, Palette::Warning, 960, 130, true, true);
    paint.Prompt(L"A", ButtonColor::A, Tr(T::ContentReconnect), 165, 435);
    paint.Prompt(L"X", ButtonColor::X, Tr(T::ContentLaunchWithout), 430, 435);
    paint.Prompt(L"B", ButtonColor::B, Tr(T::Cancel), 980, 435);
}
void DrawContentPanel(UiPainter& paint, const ContentSession& session, std::wstring_view game) {
    const auto& view = session.View();
    paint.Round(0, 0, 1280, 720, 0, Palette::Backdrop);
    paint.Round(100, 30, 1080, 660, 24, Palette::Panel);
    paint.PageHeader(140, 58, UiIcon::Folder,
        Tr(session.Category() == Kind::Update ? T::ContentUpdates : T::ContentDlc), game);
    const auto storage = Tr(session.CopyInternal() ? T::ContentInternal : T::ContentUsb);
    std::wstring header;
    switch (view.screen) {
    case Screen::Manager: header = Tr(T::ContentManagerHint); break;
    case Screen::Storage: header = Tr(T::ContentChooseStorage); break;
    case Screen::Sources: header = Tr(T::ContentSourcesStep); break;
    case Screen::Browser: header = std::wstring{Tr(T::ContentChooseFiles)} + L"  ·  " + std::wstring{storage}; break;
    case Screen::Preview: case Screen::ConfirmImport:
        header = std::wstring{Tr(T::ContentReview)} + L"  ·  " + std::wstring{storage}; break;
    case Screen::Result:
        header = I18n::Format(T::ContentResult, {std::to_wstring(view.added), std::to_wstring(view.already_present)}); break;
    default: {
        const auto item = std::find_if(view.rows.begin(), view.rows.end(), [&](const auto& r) { return r.id == view.action_id; });
        if (item != view.rows.end()) header = Label(*item) + L"  ·  " + Detail(*item, Error::None);
        break;
    }
    }
    paint.Text(header, 145, 130, 17, Palette::TextHint, 980, 43, false, true);
    if (session.Busy()) {
        paint.Text(Tr(view.screen == Screen::ConfirmImport ? (session.CopyInternal() ? T::ContentAddingInternal : T::ContentAddingUsb) : T::ContentWorking), 145, 210, 23, Palette::Text, 980, 55, true);
        paint.Round(145, 290, 980, 12, 6, Palette::Track);
        if (session.Total()) {
            const auto fraction = std::min(1.0, static_cast<double>(session.Done()) / session.Total());
            paint.Round(145, 290, static_cast<float>(980 * fraction), 12, 6, Palette::Accent);
            paint.Text(std::to_wstring(session.Done() / (1024 * 1024)) + L" / " +
                std::to_wstring(session.Total() / (1024 * 1024)) + L" MiB", 145, 315, 19, Palette::TextHint, 980);
        }
        paint.Prompt(L"B", ButtonColor::B, Tr(T::Cancel), 145, 635); return;
    }
    if (view.screen == Screen::ConfirmImport || view.screen == Screen::ConfirmRemove) {
        paint.Text(Tr(view.screen == Screen::ConfirmImport ? T::ContentConfirmImport : T::ContentConfirmRemove),
                   145, 225, 23, Palette::Warning, 980, 170, true, true);
        if (view.screen == Screen::ConfirmImport) {
            paint.Text(storage, 145, 185, 22, Palette::Accent, 980, 36, true);
            paint.Text(Tr(session.CopyInternal() ? T::ContentInternalHint : T::ContentUsbHint),
                       145, 410, 19, Palette::Text, 980, 75, false, true);
            if (session.CopyInternal()) {
                std::uint64_t required{};
                for (size_t i = 0; i < view.preview.size(); ++i)
                    if (view.checked[i] && PlanImport(view.catalog.revisions, view.preview[i].revision) == ImportAction::Add)
                        required += view.preview[i].revision.bytes;
                paint.Text(std::to_wstring(required / (1024*1024)) + L" MiB  ·  " +
                    I18n::Format(T::ContentFreeSpace, {std::to_wstring(view.available / (1024*1024))}),
                    145, 515, 18, Palette::TextHint, 980, 35);
            }
        }
        paint.Prompt(L"A", ButtonColor::A, Tr(view.screen == Screen::ConfirmImport ? T::ContentImportSelected : T::Yes), 145, 635);
        paint.Prompt(L"B", ButtonColor::B, Tr(T::Cancel), 900, 635); return;
    }
    struct Row { std::wstring name, detail; bool checked{}; };
    std::vector<Row> rows;
    switch (view.screen) {
    case Screen::Manager:
        rows.push_back({std::wstring{Tr(T::ContentInstall)}, std::wstring{Tr(T::ContentInstallHint)}});
        for (size_t i = 0; i < view.rows.size(); ++i)
            rows.push_back({Label(view.rows[i]), Detail(view.rows[i], view.row_errors[i]), view.rows[i].enabled});
        break;
    case Screen::Storage:
        rows.push_back({std::wstring{Tr(T::ContentUsb)}, std::wstring{Tr(T::ContentUsbHint)}});
        rows.push_back({std::wstring{Tr(T::ContentInternal)}, std::wstring{Tr(T::ContentInternalHint)}});
        break;
    case Screen::Result:
        for (const auto& item : view.completed) rows.push_back({Label(item), Detail(item, Error::None), item.enabled});
        rows.push_back({std::wstring{Tr(T::ContentReturnManager)}, std::wstring{Tr(view.error == Error::None ? T::ContentFinished : T::ContentResultFailed)}});
        break;
    case Screen::Sources:
        rows.push_back({std::wstring{Tr(T::ContentAuthorize)}, std::wstring{Tr(T::ContentAuthorizeHint)}});
        for (const auto& item : view.browser) rows.push_back({item.name, std::wstring{Tr(T::ContentFolder)}});
        break;
    case Screen::Browser:
        for (const auto& item : view.browser) rows.push_back({item.name,
            std::wstring{Tr(item.folder ? T::ContentFolder : T::ContentPackage)},
            std::find(view.selected_files.begin(), view.selected_files.end(), item.path) != view.selected_files.end()});
        rows.push_back({I18n::Format(T::ContentContinueFiles, {std::to_wstring(view.selected_files.size())}), std::wstring{Tr(T::ContentSelectionHint)}});
        break;
    case Screen::Preview:
        for (size_t i = 0; i < view.preview.size(); ++i) {
            const auto& item = view.preview[i];
            const auto plan = PlanImport(view.catalog.revisions, item.revision);
            const auto status = item.error != Error::None ? ErrorText(item.error) :
                plan == ImportAction::Duplicate ? T::ContentDuplicate : T::ContentVerifyBeforeActivate;
            auto detail = std::wstring{Tr(status)};
            if (plan == ImportAction::Duplicate) {
                const auto saved = std::find_if(view.catalog.revisions.begin(), view.catalog.revisions.end(),
                    [&](const auto& r) { return r.id == item.revision.id; });
                if (saved != view.catalog.revisions.end()) detail += L"  ·  " + std::wstring{Tr(saved->internal ? T::ContentInternal : T::ContentUsb)};
            } else detail += L"  ·  " + std::to_wstring(item.revision.bytes / (1024*1024)) + L" MiB";
            rows.push_back({Label(item.revision), std::move(detail), view.checked[i]});
        }
        rows.push_back({I18n::Format(T::ContentContinueItems, {std::to_wstring(std::count(view.checked.begin(), view.checked.end(), true))}), std::wstring{storage}});
        break;
    case Screen::Actions: {
        const auto item = std::find_if(view.rows.begin(), view.rows.end(), [&](const auto& r) { return r.id == view.action_id; });
        const auto toggle = item != view.rows.end() && item->enabled ? T::ContentDisable : T::ContentEnable;
        for (auto text : {toggle, T::ContentVerify, T::ContentRelocate, T::ContentDelete})
            rows.push_back({std::wstring{Tr(text)}, {}});
        break;
    }
    default: break;
    }
    const size_t first = view.selected / 6 * 6;
    for (size_t i = first; i < std::min(rows.size(), first + 6); ++i) {
        const float y = 183 + static_cast<float>(i - first) * 64;
        paint.RowFrame(140, y, 1000, 58, i == view.selected, rows[i].checked ? UiIcon::Game : UiIcon::Folder);
        paint.Text((rows[i].checked ? L"[X] " : L"") + rows[i].name, 210, y + 4, 20, Palette::Text, 890, 29, true);
        paint.Text(rows[i].detail, 210, y + 31, 14, Palette::TextHint, 890, 24);
    }
    paint.ScrollIndicator(1154, 183, 5 * 64 + 58, first,
                          6, rows.size());
    if (rows.empty()) paint.Text(Tr(T::ContentEmpty), 145, 215, 22, Palette::TextHint, 980);
    if (view.error != Error::None) paint.Text(Tr(ErrorText(view.error)), 145, 580, 17, Palette::Warning, 980, 45, false, true);
    paint.Prompt(L"A", ButtonColor::A, Tr(view.screen == Screen::Result ? T::ContentReturnManager : T::Select), 145, 635);
    if (view.screen == Screen::Browser || view.screen == Screen::Preview)
        paint.Prompt(L"X", ButtonColor::X, Tr(T::ContentImportSelected), 450, 635);
    if (view.screen == Screen::Browser) paint.Prompt(L"Y", ButtonColor::Y, Tr(T::ContentMarkFile), 680, 635);
    if (view.screen == Screen::Browser)
        paint.Text(I18n::Format(T::ContentContinueFiles, {std::to_wstring(view.selected_files.size())}), 145, 553, 17, Palette::Accent, 980, 26);
    if (view.screen == Screen::Result && view.error == Error::None)
        paint.Text(Tr(T::ContentFinished), 145, 553, 18, Palette::Accent, 980, 26, true);
    paint.Prompt(L"B", ButtonColor::B, Tr(T::Back), 1000, 635);
}
} // namespace EdenXbox::Content

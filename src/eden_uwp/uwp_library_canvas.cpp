// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/uwp_library_canvas.h"
#include "eden_uwp/pro_controller_geometry.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include "eden_uwp/game_library.h"
#include "eden_uwp/uwp_input.h"

namespace EdenXbox {
using Microsoft::WRL::ComPtr;
// The launcher owns a small D3D11/Direct2D surface only while selecting a game.
// Release every reference (including the swapchain) before D3D12 starts.
struct LibraryCanvas::Impl {
public:
    Impl(void* window, unsigned width, unsigned height) {
        ComPtr<ID3D11Device> device;
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device, nullptr, &context));
        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(device.As(&dxgi));
        ComPtr<IDXGIAdapter> adapter;
        winrt::check_hresult(dxgi->GetAdapter(&adapter));
        ComPtr<IDXGIFactory2> factory;
        winrt::check_hresult(adapter->GetParent(IID_PPV_ARGS(&factory)));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = std::max(1U, width);
        desc.Height = std::max(1U, height);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        winrt::check_hresult(factory->CreateSwapChainForCoreWindow(device.Get(),
            static_cast<IUnknown*>(window), &desc, nullptr, &swapchain));
        ComPtr<ID2D1Factory1> d2d;
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                              IID_PPV_ARGS(&d2d)));
        ComPtr<ID2D1Device> d2d_device;
        winrt::check_hresult(d2d->CreateDevice(dxgi.Get(), &d2d_device));
        winrt::check_hresult(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                           &target));
        ComPtr<IDXGISurface> backbuffer;
        winrt::check_hresult(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)));
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(desc.Format, D2D1_ALPHA_MODE_IGNORE));
        ComPtr<ID2D1Bitmap1> bitmap;
        winrt::check_hresult(target->CreateBitmapFromDxgiSurface(backbuffer.Get(),
                                                                &properties, &bitmap));
        target->SetTarget(bitmap.Get());
        winrt::check_hresult(target->CreateSolidColorBrush(D2D1::ColorF(0xf0f4fa), &brush));
        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write.GetAddressOf())));
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
        try {
            asset_root = std::filesystem::path{
                winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str()} / L"Assets";
            logo = LoadAssetBitmap(L"EdenLogo.png");
        } catch (const winrt::hresult_error&) { /* Wordmark remains available. */ }
        const float scale = std::min(width / 1280.0f, height / 720.0f);
        target->SetTransform(D2D1::Matrix3x2F::Scale(std::max(scale, 0.01f), std::max(scale, 0.01f)) *
            D2D1::Matrix3x2F::Translation((width - 1280 * scale) / 2, (height - 720 * scale) / 2));
    }
    ~Impl() {
        target->SetTarget(nullptr);
        target.Reset();
        swapchain.Reset();
        context->ClearState();
        context->Flush();
    }
    void Draw(const LibraryScan& scan, size_t selected, bool loading, bool settings,
              unsigned setting_row, const ControllerOptions& options, const std::wstring& notice, const ControllerPanel& panel) {
        prompt_family = PromptFamily::Keyboard;
        if (panel.xbox) prompt_family = PromptFamily::Xbox;
        else if (const auto index = SelectController(panel.devices, options.controller_id))
            prompt_family = panel.devices[*index].pro ? PromptFamily::Nintendo : PromptFamily::Xbox;
        target->BeginDraw();
        target->Clear(D2D1::ColorF(0x0b1018));
        if (logo) target->DrawBitmap(logo.Get(), D2D1::RectF(50, 38, 104, 92), 1,
                                     D2D1_INTERPOLATION_MODE_LINEAR);
        Text(L"eden", 120, 40, 32, 0xf4f7fa, 140, 48, true);
        Text(L"BIBLIOTECA", 266, 54, 16, 0x9ba9ba, 200);
        Round(670, 43, 225, 44, 14, 0x202b39);
        ControlPrompt(Navigation::AddFolder, L"Agregar carpeta", 680, 49, 174);
        Text(std::to_wstring(scan.entries.size()) + L" juegos", 900, 52, 18, 0x9ba9ba, 170);
        Round(1080, 43, 146, 44, 14, 0x202b39);
        ControlPrompt(Navigation::Settings, L"Mando", 1090, 49, 100);
        Round(50, 112, 1176, 1, 0, 0x233142);
        if (loading) {
            Text(L"Preparando tu biblioteca", 64, 192, 40, 0xf4f7fa, 1000, 64, true);
            Text(L"Buscando juegos en tu carpeta...", 64, 266, 24, 0x9ba9ba, 1000);
        } else if (scan.entries.empty()) {
            Text(L"Tu proxima aventura empieza aqui", 64, 192, 40, 0xf4f7fa, 1100, 70, true);
            Text(L"Conecta tu USB y usa Agregar carpeta para buscar tus juegos.", 64, 278, 24, 0x9ba9ba, 1120);
        } else {
            const auto& game = scan.entries[selected];
            Cover(game, 64, 148, 220);
            Text(L"LISTO PARA JUGAR", 320, 149, 16, 0x77e3bd, 700);
            Text(game.name, 320, 184, 42, 0xf4f7fa, 866, 110, true, true);
            Text((game.developer.empty() ? L"Tu biblioteca personal" : game.developer) +
                 (game.source_name.empty() ? L"" : L" · " + game.source_name),
                 320, 292, 23, 0x9ba9ba, 850);
            Round(320, 334, 184, 46, 14, 0x77e3bd);
            Round(331, 341, 34, 32, 8, 0x152b26);
            ControlPrompt(Navigation::Play, L"Jugar", 332, 340, 100, 0x0b1018);
            Text(game.relative_path.extension().wstring().substr(1), 532, 344, 18, 0x9ba9ba, 600);
            Text(L"Tus juegos", 64, 402, 24, 0xe5edf5, 950, 36, true);
            Text(std::to_wstring(selected + 1) + L" / " + std::to_wstring(scan.entries.size()),
                 1100, 405, 19, 0x9ba9ba, 120);
            const size_t first = selected / 5 * 5;
            for (size_t i = first; i < std::min(first + 5, scan.entries.size()); ++i) {
                const float x = 64 + static_cast<float>(i - first) * 232;
                Round(x - 6, 444, 220, 191, 16, i == selected ? 0x77e3bd : 0x202b39);
                Round(x - 3, 447, 214, 185, 13, i == selected ? 0x203b3a : 0x151e2b);
                Cover(scan.entries[i], x + 39, 454, 124);
                Text(scan.entries[i].name, x + 3, 586, 19, 0xf4f7fa, 197, 40, true);
            }
        }
        ControlPrompt(Navigation::Play, L"Jugar", 64, 654, 150);
        ControlPrompt(Navigation::Explore, L"Explorar", 294, 654, 150);
        ControlPrompt(Navigation::Settings, L"Mando", 524, 654, 150);
        ControlPrompt(Navigation::Refresh, L"Actualizar", 754, 654, 150);
        ControlPrompt(Navigation::Back, L"Salir", 1000, 654, 150);
        if (!notice.empty()) Text(notice, 320, 316, 15, 0xe4bb83, 860, 22);
        if (settings) {
            brush->SetColor(D2D1::ColorF(0, 0, 0, 0.72f));
            target->FillRectangle(D2D1::RectF(0, 0, 1280, 720), brush.Get());
            Round(620, 133, 586, 487, 24, 0x202b39);
            Text(L"Tu mando", 658, 168, 34, 0xf4f7fa, 490, 55, true);
            Text(L"Jugador 1 · Ajustes para todos tus juegos", 658, 227, 21, 0x9ba9ba, 490);
            const unsigned offset = panel.xbox ? 0 : 1;
            for (unsigned row = 0; row < 3 + offset; ++row) {
                const float y = ControllerRowTop + row * ControllerRowHeight;
                Round(648, y, 530, 56, 14, row == setting_row ? 0x304c4b : 0x151e2b);
                const bool device = offset && row == 0;
                const bool type = row == offset;
                const bool face = row == offset + 1;
                Text(device ? L"Dispositivo                         v" :
                     type ? L"La consola lo ve como          < >" :
                     face ? L"Botones A / B / X / Y" : L"Zona muerta de sticks",
                     669, y + 2, 19, 0xf4f7fa, 460, 28, true);
                Text(device ? (options.controller_id.empty() ? L"Automatico · " : L"") +
                         ControllerLabel(panel.devices, options.controller_id) :
                     type ? std::wstring{ConsoleControllerStyleLabel(options.style)} :
                     face ? (options.swap_face_buttons ? L"Por posicion" : L"Por letra") :
                         std::to_wstring(static_cast<int>(options.deadzone * 100)) + L"%",
                     669, y + 28, 18, 0x77e3bd, 460, 26);
            }
            if (!panel.xbox) {
                Round(648, 510, 530, 34, 10, setting_row == 4 ? 0x304c4b : 0x151e2b);
                Text(L"Configurar teclado   >", 669, 512, 19, 0x77e3bd, 460, 30, true);
            }
            ControlPrompt(Navigation::Explore, L"Elegir", 658, 551, 90);
            ControlPrompt(Navigation::Play, L"Cambiar", 838, 551, 100);
            ControlPrompt(Navigation::Back, L"Volver", 1018, 551, 100);
            if (!panel.notice.empty()) Text(panel.notice, 658, 594, 16, 0x77e3bd, 515, 25);
        }
        if (settings && panel.expanded) {
            const size_t first = ControllerChoiceFirst(panel);
            const size_t count = panel.devices.size() + 2;
            const size_t end = std::min(count, first + ControllerVisibleChoices);
            Round(648, ControllerChoiceTop - 4, 530,
                  (end - first) * ControllerChoiceHeight + 8, 12, 0x354556);
            for (size_t i = first; i < end; ++i) {
                const float y = ControllerChoiceTop + (i - first) * ControllerChoiceHeight;
                Round(652, y, 522, 40, 8, i == panel.choice ? 0x304c4b : 0x151e2b);
                const bool session = i >= 2 && panel.devices[i - 2].id.starts_with(L"session:");
                const bool supported = i < 2 || (bool(panel.devices[i - 2]) && !session);
                const std::wstring label = i == 0 ? L"Automatico (primer mando disponible)" :
                    i == 1 ? L"Teclado" : panel.devices[i - 2].name +
                    (supported ? (panel.devices[i - 2].wireless ? L" · Inalambrico" : L" · USB") :
                                 session ? L" · Solo modo automatico" : L" · Sin mapeo compatible");
                Text(label, 665, y + 6, 18, supported ? 0xf4f7fa : 0x9ba9ba, 495, 30);
            }
        }
        if (panel.keyboard.open) DrawKeyboard(panel.keyboard);
        winrt::check_hresult(target->EndDraw());
        winrt::check_hresult(swapchain->Present(1, 0));
    }
    void InvalidateCovers() { covers.clear(); }
private:
    void DrawKeyboard(const KeyboardEditor& editor) {
        brush->SetColor(D2D1::ColorF(0, 0, 0, 0.86f));
        target->FillRectangle(D2D1::RectF(0, 0, 1280, 720), brush.Get());
        Round(80, 88, 1120, 584, 22, 0x111923);
        Text(L"Configuracion de teclado", 120, 118, 28, 0xf4f7fa, 900, 44, true);
        Text(L"Jugador 1  /  Mando Pro", 120, 166, 17, 0x91a3b8, 650, 30);
        Round(1120, 112, 48, 40, 10, 0x202c3c);
        Text(L"×", 1120, 112, 24, 0xc5cfdb, 48, 40, false, false, true);
        Round(120, 210, 480, 342, 16, 0x182331);
        Text(L"VISTA DEL MANDO", 143, 225, 13, 0x91a3b8, 220, 24, true);
        // Reuse Eden Qt's original Pro Controller contours and button coordinates.
        // Create paths once; fills/strokes/highlights share cached GPU resources.
        if (!controller_paths[0]) {
            using namespace ProControllerGeometry;
            ComPtr<ID2D1Factory> factory; target->GetFactory(&factory);
            auto polygon = [&](size_t index, const auto& points, bool mirror, bool symmetric) {
                winrt::check_hresult(factory->CreatePathGeometry(&controller_paths[index]));
                ComPtr<ID2D1GeometrySink> sink;
                winrt::check_hresult(controller_paths[index]->Open(&sink));
                auto point = [&](size_t i, bool reflected) {
                    const float x = points[i * 2] * (reflected ? -1.0f : 1.0f);
                    return D2D1::Point2F(350 + x, 390 + points[i * 2 + 1]);
                };
                sink->BeginFigure(point(0, mirror), D2D1_FIGURE_BEGIN_FILLED);
                for (size_t i = 1; i < points.size() / 2; ++i) sink->AddLine(point(i, mirror));
                if (symmetric)
                    for (size_t i = points.size() / 2; i-- > 0;) sink->AddLine(point(i, !mirror));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                winrt::check_hresult(sink->Close());
            };
            polygon(0, pro_body, false, true);
            polygon(1, pro_left_handle, false, false); polygon(2, pro_left_handle, true, false);
            polygon(3, pro_left_trigger, false, false); polygon(4, pro_left_trigger, true, false);
        }
        auto path = [&](size_t i, unsigned color) {
            brush->SetColor(D2D1::ColorF(color)); target->FillGeometry(controller_paths[i].Get(), brush.Get());
            brush->SetColor(D2D1::ColorF(0x526478)); target->DrawGeometry(controller_paths[i].Get(), brush.Get(), 1.2f);
        };
        const size_t action = editor.selected;
        path(1, 0x263445); path(2, 0x263445); path(0, 0x202c3b);
        path(3, action == 6 ? 0x77e3bd : 0x35465b); path(4, action == 7 ? 0x77e3bd : 0x35465b);
        auto circle = [&](float x, float y, float radius, const wchar_t* label, bool active, float font = 16) {
            const auto shape = D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius);
            brush->SetColor(D2D1::ColorF(active ? 0x77e3bdU : 0x121c29U)); target->FillEllipse(shape, brush.Get());
            brush->SetColor(D2D1::ColorF(active ? 0xa7f1d7U : 0x607287U)); target->DrawEllipse(shape, brush.Get(), 1.4f);
            Text(label, x - radius, y - radius, font, active ? 0x102921 : 0xe3eaf2,
                 radius * 2, radius * 2, true, false, true);
        };
        auto stick = [&](float x, float y, const wchar_t* label, bool active) {
            circle(x, y, 32, L"", active);
            circle(x, y, 22, label, false, 12);
        };
        stick(239, 335, L"L3", action == 4 || (action >= 20 && action < 24));
        stick(401, 390, L"R3", action == 5 || action >= 24);
        circle(486, 334, 15, L"A", action == 0); circle(455, 365, 15, L"B", action == 1);
        circle(455, 303, 15, L"X", action == 2); circle(424, 334, 15, L"Y", action == 3);
        Round(278, 359, 22, 62, 4, 0x111b28); Round(258, 379, 62, 22, 4, 0x111b28);
        for (size_t i = 12; i <= 15; ++i) {
            const auto tile = KeyboardDiagramTile(i);
            Round(tile.x, tile.y, tile.w, tile.h, 3, action == i ? 0x77e3bd : 0x344459);
        }
        circle(300, 304, 10, L"-", action == 11, 14); circle(400, 304, 10, L"+", action == 10, 14);
        circle(379, 334, 10, L"", action == 18);
        brush->SetColor(D2D1::ColorF(action == 18 ? 0x102921U : 0xc5cfdbU));
        target->DrawLine(D2D1::Point2F(374, 334), D2D1::Point2F(379, 330), brush.Get(), 1.4f);
        target->DrawLine(D2D1::Point2F(379, 330), D2D1::Point2F(384, 334), brush.Get(), 1.4f);
        target->DrawRectangle(D2D1::RectF(376, 334, 382, 339), brush.Get(), 1.2f);
        Round(314, 327, 14, 14, 3, action == 19 ? 0x77e3bd : 0x111b28);
        brush->SetColor(D2D1::ColorF(0x91a3b8));
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(321, 334), 4, 4), brush.Get(), 1.2f);
        for (const auto i : {size_t{8}, size_t{9}}) {
            const auto tile = KeyboardDiagramTile(i);
            Round(tile.x, tile.y, tile.w, tile.h, 8, action == i ? 0x77e3bd : 0x263445);
            Text(i == 8 ? L"ZL" : L"ZR", tile.x, tile.y, 14, action == i ? 0x102921 : 0xc5cfdb,
                 tile.w, tile.h, true, false, true);
        }
        Text(L"L", 220, 252, 14, action == 6 ? 0x102921 : 0xc5cfdb, 60, 22, true, false, true);
        Text(L"R", 450, 252, 14, action == 7 ? 0x102921 : 0xc5cfdb, 40, 22, true, false, true);
        Round(120, 564, 480, 67, 12, 0x20342f);
        Text(L"ASIGNACION SELECCIONADA", 140, 574, 11, 0x91b7a7, 300, 19, true);
        Text(KeyboardActionNames[action], 140, 594, 20, 0xf0f7f4, 285, 30, true);
        Round(436, 579, 142, 38, 8, 0x111f1c);
        Text(editor.capturing ? L"Pulsa una tecla" : KeyboardKeyName(editor.bindings[action]),
             436, 579, 16, 0x77e3bd, 142, 38, true, false, true);
        constexpr std::array<const wchar_t*, 3> pages{L"Botones", L"Cruceta / sistema", L"Sticks"};
        for (unsigned page = 0; page < pages.size(); ++page) {
            const float x = 635 + page * 174.0f;
            Round(x, 203, 166, 36, 9, page == editor.page ? 0x304c46 : 0x1b2736);
            Text(pages[page], x, 203, 15, page == editor.page ? 0x95ebca : 0xabb9c9,
                 166, 36, true, false, true);
        }
        for (const size_t i : KeyboardPageActions(editor.page)) {
            const auto tile = KeyboardTileFor(i);
            Round(tile.x, tile.y, tile.w, tile.h, 9, i == action ? 0x233e36 : 0x192535);
            Text(KeyboardActionNames[i], tile.x + 14, tile.y, 15, 0xc5cfdb, 136, tile.h, false, false, true);
            Round(tile.x + 153, tile.y + 9, 86, 27, 6, 0x111b28);
            Text(i == action && editor.capturing ? L"..." : KeyboardKeyName(editor.bindings[i]),
                 tile.x + 153, tile.y + 9, 14, i == action ? 0x77e3bd : 0xabb9c9, 86, 27, true, false, true);
        }
        Round(635, 603, 155, 36, 8, 0x1b2736); Text(L"Borrar asignacion", 635, 603, 15, 0xc5cfdb, 155, 36, false, false, true);
        Round(806, 603, 170, 36, 8, 0x1b2736); Text(L"Restaurar", 806, 603, 15, 0xc5cfdb, 170, 36, false, false, true);
        if (!editor.notice.empty()) Text(editor.notice, 990, 609, 13, 0x91b7a7, 172, 30);
        Text(editor.capturing ? L"Esperando tecla...  Esc cancela  /  4 segundos" :
             L"Click o Enter: asignar    Tab: cambiar grupo    Supr: borrar    Esc: volver",
             120, 644, 13, 0x7f91a6, prompt_family == PromptFamily::Keyboard ? 1030.0f : 860.0f, 22);
        if (prompt_family != PromptFamily::Keyboard)
            ControlPrompt(Navigation::Back, L"Volver", 1010, 637, 100);
    }
    ComPtr<ID2D1Bitmap1> LoadAssetBitmap(const std::filesystem::path& relative) {
        ComPtr<ID2D1Bitmap1> bitmap;
        if (asset_root.empty()) return bitmap;
        try {
            ComPtr<IWICBitmapDecoder> decoder;
            const auto path = asset_root / relative;
            winrt::check_hresult(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &decoder));
            ComPtr<IWICBitmapFrameDecode> frame;
            winrt::check_hresult(decoder->GetFrame(0, &frame));
            ComPtr<IWICFormatConverter> converter;
            winrt::check_hresult(wic->CreateFormatConverter(&converter));
            winrt::check_hresult(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
            winrt::check_hresult(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap));
        } catch (const winrt::hresult_error&) {}
        return bitmap;
    }
    enum class PromptFamily : unsigned { Keyboard, Xbox, Nintendo };
    enum class Navigation : unsigned { Play, Explore, Settings, Refresh, Back, AddFolder };
    void ControlPrompt(Navigation action, const wchar_t* label, float x, float y,
                       float width, unsigned color = 0x9ba9ba) {
        struct Glyph { const wchar_t* asset; const wchar_t* fallback; };
        static constexpr std::array<Glyph, 6> keyboard{{
            {L"keyboard_enter", L"Enter"}, {L"keyboard_arrows", L"Flechas"},
            {L"keyboard_f1", L"F1"}, {L"keyboard_r", L"R"}, {L"keyboard_escape", L"Esc"}, {L"keyboard_o", L"O"}}};
        static constexpr std::array<Glyph, 6> xbox{{
            {L"xbox_a", L"A"}, {L"xbox_dpad", L"Cruceta"}, {L"xbox_view", L"View"},
            {L"xbox_menu", L"Menu"}, {L"xbox_b", L"B"}, {L"xbox_x", L"X"}}};
        const auto index = static_cast<unsigned>(action);
        if (prompt_family != PromptFamily::Keyboard && action == Navigation::Explore) {
            // Neutral monochrome d-pad for both controller families. The old
            // bitmap was Kenney's red color variant, not an application state.
            Round(x + 11, y + 3, 10, 26, 2, 0xf4f7fa);
            Round(x + 3, y + 11, 26, 10, 2, 0xf4f7fa);
            brush->SetColor(D2D1::ColorF(0xf4f7fa));
            target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + 56, y + 16), 13, 13), brush.Get(), 2);
            Text(L"L", x + 40, y, 16, 0xf4f7fa, 32, 32, true, false, true);
            Text(label, x + 80, y + 3, 19, color, width);
            return;
        }
        if (prompt_family == PromptFamily::Nintendo) {
            // Native D2D glyphs match Nintendo's monochrome A/B and minus/plus.
            // They reuse cached text formats and allocate no bitmap or path.
            brush->SetColor(D2D1::ColorF(0xf4f7fa));
            const auto center = D2D1::Point2F(x + 16, y + 16);
            if (action == Navigation::Play || action == Navigation::Back || action == Navigation::AddFolder) {
                target->DrawEllipse(D2D1::Ellipse(center, 13, 13), brush.Get(), 2);
                Text(action == Navigation::Play ? L"A" : action == Navigation::AddFolder ? L"X" : L"B",
                     x, y, 18, 0xf4f7fa, 32, 32, true, false, true);
            } else {
                target->DrawLine(D2D1::Point2F(x + 7, y + 16), D2D1::Point2F(x + 25, y + 16), brush.Get(), 3);
                if (action == Navigation::Refresh)
                    target->DrawLine(D2D1::Point2F(x + 16, y + 7), D2D1::Point2F(x + 16, y + 25), brush.Get(), 3);
            }
            Text(label, x + 40, y + 3, 19, color, width);
            return;
        }
        const auto& glyph = prompt_family == PromptFamily::Keyboard ? keyboard[index] : xbox[index];
        Prompt(glyph.asset, label, glyph.fallback, x, y, width, color);
    }
    void Prompt(const wchar_t* asset, const wchar_t* label, const wchar_t* fallback,
                float x, float y, float width, unsigned color = 0x9ba9ba) {
        auto found = prompts.find(asset);
        if (found == prompts.end()) {
            auto bitmap = LoadAssetBitmap(std::filesystem::path{L"InputPrompts"} /
                                          (std::wstring{asset} + L".png"));
            found = prompts.emplace(asset, std::move(bitmap)).first;
        }
        if (found->second) {
            target->DrawBitmap(found->second.Get(), D2D1::RectF(x, y, x + 32, y + 32), 1,
                               D2D1_INTERPOLATION_MODE_LINEAR);
            Text(label, x + 40, y + 3, 19, color, width);
        } else {
            Text(std::wstring{fallback} + L"  " + label, x, y + 3, 19, color, width + 40);
        }
    }
    void Round(float x, float y, float w, float h, float radius, unsigned color) {
        brush->SetColor(D2D1::ColorF(color));
        target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius), brush.Get());
    }
    void Text(const std::wstring& text, float x, float y, float size, unsigned color,
              float width, float height = 40, bool bold = false, bool wrap = false, bool center = false) {
        const unsigned key = static_cast<unsigned>(size) * 8 + (bold ? 1 : 0) + (wrap ? 2 : 0) + (center ? 4 : 0);
        auto& format = formats[key];
        if (!format) {
            winrt::check_hresult(write->CreateTextFormat(L"Segoe UI", nullptr,
                bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"es-ES", &format));
            if (center) {
                format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis;
            winrt::check_hresult(write->CreateEllipsisTrimmingSign(format.Get(), &ellipsis));
            format->SetTrimming(&trim, ellipsis.Get());
        }
        brush->SetColor(D2D1::ColorF(color));
        target->DrawText(text.data(), static_cast<UINT32>(text.size()), format.Get(),
            D2D1::RectF(x, y, x + width, y + height), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void Cover(const LibraryEntry& entry, float x, float y, float size) {
        Round(x, y, size, size, 12, 0x283648);
        const auto key = entry.relative_path.wstring();
        auto found = covers.find(key);
        if (found == covers.end() && !entry.icon.empty()) {
            ComPtr<ID2D1Bitmap1> bitmap;
            try {
                ComPtr<IWICStream> stream;
                winrt::check_hresult(wic->CreateStream(&stream));
                winrt::check_hresult(stream->InitializeFromMemory(const_cast<BYTE*>(entry.icon.data()),
                                                                 static_cast<DWORD>(entry.icon.size())));
                ComPtr<IWICBitmapDecoder> decoder;
                winrt::check_hresult(wic->CreateDecoderFromStream(stream.Get(), nullptr,
                    WICDecodeMetadataCacheOnDemand, &decoder));
                ComPtr<IWICBitmapFrameDecode> frame;
                winrt::check_hresult(decoder->GetFrame(0, &frame));
                UINT w, h;
                winrt::check_hresult(frame->GetSize(&w, &h));
                if (w == 0 || h == 0 || w > 4096 || h > 4096)
                    winrt::throw_hresult(E_INVALIDARG);
                ComPtr<IWICBitmapScaler> scaler;
                winrt::check_hresult(wic->CreateBitmapScaler(&scaler));
                winrt::check_hresult(scaler->Initialize(frame.Get(), 256, 256, WICBitmapInterpolationModeFant));
                ComPtr<IWICFormatConverter> converter;
                winrt::check_hresult(wic->CreateFormatConverter(&converter));
                winrt::check_hresult(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
                winrt::check_hresult(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap));
            } catch (const winrt::hresult_error&) {}
            if (covers.size() >= 12) covers.clear();
            found = covers.emplace(key, std::move(bitmap)).first;
        }
        if (found != covers.end() && found->second) {
            // Geometries are cached by size; transform places the rounded mask
            // without reallocating it on each selection or focus redraw.
            auto& mask = masks[static_cast<unsigned>(size)];
            if (!mask) {
                ComPtr<ID2D1Factory> factory;
                target->GetFactory(&factory);
                winrt::check_hresult(factory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(D2D1::RectF(0, 0, size, size), 12, 12), &mask));
            }
            D2D1_LAYER_PARAMETERS1 layer{};
            layer.contentBounds = D2D1::RectF(x, y, x + size, y + size);
            layer.geometricMask = mask.Get();
            layer.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
            layer.maskTransform = D2D1::Matrix3x2F::Translation(x, y);
            layer.opacity = 1;
            target->PushLayer(&layer, nullptr);
            target->DrawBitmap(found->second.Get(), D2D1::RectF(x, y, x + size, y + size), 1,
                               D2D1_INTERPOLATION_MODE_LINEAR);
            target->PopLayer();
        } else {
            Text(L"EDEN", x + size * 0.12f, y + size * 0.32f, size > 150 ? 34.0f : 20.0f,
                 0x77e3bd, size * 0.85f, 54, true);
            Text(entry.relative_path.extension().wstring().substr(1), x + size * 0.12f,
                 y + size * 0.58f, 16, 0x9ba9ba, size * 0.85f);
        }
    }
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID2D1DeviceContext> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteFactory> write;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1Bitmap1> logo;
    std::array<ComPtr<ID2D1PathGeometry>, 5> controller_paths;
    PromptFamily prompt_family{PromptFamily::Keyboard};
    std::filesystem::path asset_root;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> prompts;
    std::unordered_map<unsigned, ComPtr<ID2D1RoundedRectangleGeometry>> masks;
    std::unordered_map<unsigned, ComPtr<IDWriteTextFormat>> formats;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> covers;
};
LibraryCanvas::LibraryCanvas(void* window, unsigned width, unsigned height)
    : impl{std::make_unique<Impl>(window, width, height)} {}
LibraryCanvas::~LibraryCanvas() = default;
void LibraryCanvas::InvalidateCovers() { impl->InvalidateCovers(); }
void LibraryCanvas::Draw(const LibraryScan& scan, size_t selected, bool loading, bool settings,
                         unsigned row, const ControllerOptions& options, const std::wstring& notice,
                         const ControllerPanel& panel) {
    impl->Draw(scan, selected, loading, settings, row, options, notice, panel);
}
} // namespace EdenXbox

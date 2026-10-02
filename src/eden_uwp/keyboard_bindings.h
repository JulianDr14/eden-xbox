// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <string>
#include <cstddef>
#include <span>

namespace EdenXbox {
// First 20 actions deliberately match VirtualGamepad::VirtualButton ordering.
inline constexpr size_t KeyboardButtonCount = 20;
inline constexpr size_t KeyboardActionCount = 28;
using KeyboardBindings = std::array<unsigned, KeyboardActionCount>;
inline constexpr KeyboardBindings DefaultKeyboardBindings{
    'B', 'N', 'X', 'Y', 'F', 'H', 'C', 'V', 'Z', 'E', 'M', 'K',
    37, 38, 39, 40, 0, 0, 36, 35, 'W', 'S', 'A', 'D', 'I', 'L', 'J', 'O'};
inline constexpr std::array<const wchar_t*, KeyboardActionCount> KeyboardActionNames{
    L"A", L"B", L"X", L"Y", L"Clic stick L", L"Clic stick R", L"L", L"R",
    L"ZL", L"ZR", L"+", L"-", L"Cruceta izquierda", L"Cruceta arriba",
    L"Cruceta derecha", L"Cruceta abajo", L"SL", L"SR", L"HOME", L"Captura",
    L"L arriba", L"L abajo", L"L izquierda", L"L derecha",
    L"R arriba", L"R abajo", L"R izquierda", L"R derecha"};
// One key per action. Move duplicate bindings instead of leaving hidden conflicts.
inline bool AssignKeyboardKey(KeyboardBindings& bindings, size_t action, unsigned key) {
    if (action >= bindings.size() || key == 27 || key > 254 || (key >= 195 && key <= 218)) return false;
    if (key) for (auto& existing : bindings) if (existing == key) existing = 0;
    bindings[action] = key;
    return true;
}
KeyboardBindings LoadKeyboardBindings();
bool SaveKeyboardBindings(const KeyboardBindings& bindings);
std::wstring KeyboardKeyName(unsigned key);
void SetKeyboardKey(unsigned key, bool pressed);
void ResetKeyboardKeys();
void InitializeKeyboardBindings();
bool RunKeyboardInputSelfTest(); // Explicit boot.cfg gate; isolated engine, no user events.

struct KeyboardEditor {
    std::wstring notice;
    bool open{}, capturing{};
    size_t selected{};
    unsigned page{};
    KeyboardBindings bindings{DefaultKeyboardBindings};
};
struct KeyboardTile { float x, y, w, h; };
inline std::span<const size_t> KeyboardPageActions(unsigned page) {
    static constexpr std::array<size_t, 12> buttons{0,1,2,3,4,5,6,7,8,9,10,11};
    static constexpr std::array<size_t, 6> system{12,13,14,15,18,19};
    static constexpr std::array<size_t, 8> sticks{20,21,22,23,24,25,26,27};
    if (page == 0) return buttons;
    if (page == 1) return system;
    return sticks;
}
inline unsigned KeyboardPageFor(size_t action) { return action < 12 ? 0U : action < 20 ? 1U : 2U; }
inline KeyboardTile KeyboardTileFor(size_t action) {
    const auto actions = KeyboardPageActions(KeyboardPageFor(action));
    size_t index = 0;
    while (index < actions.size() && actions[index] != action) ++index;
    if (index == actions.size()) return {};
    return {635 + (index % 2) * 263.0f, 253 + (index / 2) * 53.0f, 251, 45};
}
inline bool Contains(KeyboardTile tile, float x, float y) {
    return x >= tile.x && x < tile.x + tile.w && y >= tile.y && y < tile.y + tile.h;
}
inline KeyboardTile KeyboardDiagramTile(size_t action) {
    switch (action) {
    case 0: return {471, 319, 30, 30}; case 1: return {440, 350, 30, 30};
    case 2: return {440, 288, 30, 30}; case 3: return {409, 319, 30, 30};
    case 4: return {207, 303, 64, 64}; case 5: return {369, 358, 64, 64};
    case 6: return {195, 249, 119, 29}; case 7: return {386, 249, 119, 29};
    case 8: return {137, 252, 44, 28}; case 9: return {519, 252, 44, 28};
    case 10: return {390, 294, 20, 20}; case 11: return {290, 294, 20, 20};
    case 12: return {258, 379, 20, 22}; case 13: return {278, 359, 22, 20};
    case 14: return {300, 379, 20, 22}; case 15: return {278, 401, 22, 20};
    case 18: return {369, 324, 20, 20}; case 19: return {314, 327, 14, 14};
    default: return {};
    }
}
}

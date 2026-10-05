// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>
#include <winrt/Windows.Storage.h>

namespace EdenXbox {
struct GameFolderSource { std::string token; std::wstring name; };
struct FileSetupStatus {
    bool keys_ready{};
    unsigned firmware_files{};
    std::vector<GameFolderSource> sources;
};
struct ConfigurationPanel {
    bool open{};
    bool files{};
    bool busy{};
    size_t selected{};
    FileSetupStatus status;
    std::wstring notice;
    unsigned completed{}, total{};
};
inline size_t ConfigurationRowCount(const ConfigurationPanel& panel) {
    return panel.files ? 3 + panel.status.sources.size() : 2;
}
inline size_t ConfigurationFirstRow(const ConfigurationPanel& panel) {
    return (panel.selected / 5) * 5;
}
// Share geometry between painting and pointer hit testing.
struct ConfigurationLayout {
    float x, width, top, stride, row_height, footer;
};
inline ConfigurationLayout GetConfigurationLayout(const ConfigurationPanel& panel) {
    return panel.files ? ConfigurationLayout{126, 1028, 246, 62, 54, 622}
                       : ConfigurationLayout{180, 920, 246, 100, 86, 540};
}
enum class FileImport { Keys, Firmware };
struct FileImportProgress {
    std::atomic<unsigned> completed{};
    std::atomic<unsigned> total{};
};
// These operations run on an MTA worker while the guest is stopped.
FileSetupStatus ReadFileSetupStatus();
void ForgetGameFolder(std::string_view token);
std::string ImportSystemFiles(const winrt::Windows::Storage::StorageFolder& source,
                             FileImport kind, FileImportProgress& progress, std::stop_token stop);
bool HasUsableHeaderKey();
bool RunFileManagerGate(const std::function<void(std::string)>& diagnostic);
} // namespace EdenXbox

// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace {
class UncommittedFile : public FileSys::ArrayVfsFile<1> {
public:
    UncommittedFile() : ArrayVfsFile(std::array<u8, 1>{}) {}
    bool IsWritable() const override { return true; }
};
}

int main() {
    const auto path = std::filesystem::temp_directory_path() /
        ("pulse-fs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        auto unsupported = std::make_shared<UncommittedFile>();
        FileSys::OffsetVfsFile wrapped(unsupported, 1);
        if (unsupported->CommitForCheckpoint() || wrapped.CommitForCheckpoint()) {
            throw std::runtime_error("unsupported writable backend accepted");
        }
        const FileSys::ArrayVfsFile<1> readonly(std::array<u8, 1>{});
        if (!readonly.CommitForCheckpoint()) throw std::runtime_error("read-only commit failed");
        for (unsigned cycle = 0; cycle < 100; ++cycle) {
            std::array<u8, 1024> expected{};
            for (size_t i = 0; i < expected.size(); ++i) expected[i] = static_cast<u8>(i + cycle);
            {
                FileSys::RealVfsFilesystem fs;
                auto file = fs.CreateFile(path.string());
                if (!file || !file->Resize(2048)) throw std::runtime_error("create failed");
                FileSys::OffsetVfsFile view(file, 1024, 512);
                if (view.Write(expected.data(), expected.size(), 0) != expected.size() ||
                    !view.CommitForCheckpoint()) throw std::runtime_error("durable commit failed");
            }
            std::ifstream reader(path, std::ios::binary);
            reader.seekg(512);
            std::array<u8, 1024> actual{};
            reader.read(reinterpret_cast<char*>(actual.data()), actual.size());
            if (!reader || actual != expected) throw std::runtime_error("committed bytes differ");
        }
        std::filesystem::remove(path);
        std::cout << "Pulse FS: 100 real file/offset commit cycles; unsupported writers rejected\n";
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}

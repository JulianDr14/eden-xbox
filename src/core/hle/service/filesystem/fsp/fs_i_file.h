// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/file_sys/fsa/fs_i_file.h"
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/service.h"

namespace Service::FileSystem {

class IFile final : public ServiceFramework<IFile> {
public:
    explicit IFile(Core::System& system_, FileSys::VirtualFile file_);

    // Called only after the service barrier; a successful ordinary Flush() is not evidence that
    // writes have reached durable storage (that SDK entry point is currently a no-op).
    bool CommitForCheckpoint() const {
        return backend && backend->CommitForCheckpoint();
    }

private:
    std::unique_ptr<FileSys::Fsa::IFile> backend;

    Result Read(FileSys::ReadOption option, Out<s64> out_size, s64 offset,
                const OutBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure>
                    out_buffer,
                s64 size);
    Result Write(
        const InBuffer<BufferAttr_HipcMapAlias | BufferAttr_HipcMapTransferAllowsNonSecure> buffer,
        FileSys::WriteOption option, s64 offset, s64 size);
    Result Flush();
    Result SetSize(s64 size);
    Result GetSize(Out<s64> out_size);
};

} // namespace Service::FileSystem

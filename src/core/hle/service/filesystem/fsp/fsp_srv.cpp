// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cinttypes>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/fs_directory.h"
#include "core/file_sys/fs_filesystem.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/system_archive/system_archive.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/result.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/fsp/fs_i_file.h"
#include "core/hle/service/filesystem/fsp/fs_i_filesystem.h"
#include "core/hle/service/filesystem/fsp/fs_i_multi_commit_manager.h"
#include "core/hle/service/filesystem/fsp/fs_i_save_data_info_reader.h"
#include "core/hle/service/filesystem/fsp/fs_i_storage.h"
#include "core/hle/service/filesystem/fsp/fsp_srv.h"
#include "core/hle/service/filesystem/fsp/save_data_transfer_prohibiter.h"
#include "core/hle/service/filesystem/romfs_controller.h"
#include "core/hle/service/filesystem/save_data_controller.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/loader/loader.h"
#include "core/reporter.h"

namespace Service::FileSystem {

FSP_SRV::FSP_SRV(Core::System& system_)
    : ServiceFramework{system_, "fsp-srv"}, fsc{system.GetFileSystemController()},
      content_provider{system.GetContentProvider()}, reporter{system.GetReporter()} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "OpenFileSystem"},
        {1, D<&FSP_SRV::SetCurrentProcess>, "SetCurrentProcess"},
        {2, D<&FSP_SRV::OpenDataFileSystemByCurrentProcess>, "OpenDataFileSystemByCurrentProcess"},
        {7, D<&FSP_SRV::OpenFileSystemWithPatch>, "OpenFileSystemWithPatch"},
        {8, nullptr, "OpenFileSystemWithId"},
        {9, nullptr, "OpenDataFileSystemByApplicationId"},
        {11, nullptr, "OpenBisFileSystem"},
        {12, nullptr, "OpenBisStorage"},
        {13, nullptr, "InvalidateBisCache"},
        {17, nullptr, "OpenHostFileSystem"},
        {18, D<&FSP_SRV::OpenSdCardFileSystem>, "OpenSdCardFileSystem"},
        {19, nullptr, "FormatSdCardFileSystem"},
        {21, nullptr, "DeleteSaveDataFileSystem"},
        {22, D<&FSP_SRV::CreateSaveDataFileSystem>, "CreateSaveDataFileSystem"},
        {23, D<&FSP_SRV::CreateSaveDataFileSystemBySystemSaveDataId>, "CreateSaveDataFileSystemBySystemSaveDataId"},
        {24, nullptr, "RegisterSaveDataFileSystemAtomicDeletion"},
        {25, nullptr, "DeleteSaveDataFileSystemBySaveDataSpaceId"},
        {26, nullptr, "FormatSdCardDryRun"},
        {27, D<&FSP_SRV::IsExFatSupported>, "IsExFatSupported"},
        {28, nullptr, "DeleteSaveDataFileSystemBySaveDataAttribute"},
        {30, nullptr, "OpenGameCardStorage"},
        {31, nullptr, "OpenGameCardFileSystem"},
        {32, D<&FSP_SRV::ExtendSaveDataFileSystem>, "ExtendSaveDataFileSystem"},
        {33, nullptr, "DeleteCacheStorage"},
        {34, D<&FSP_SRV::GetCacheStorageSize>, "GetCacheStorageSize"},
        {35, nullptr, "CreateSaveDataFileSystemByHashSalt"},
        {36, nullptr, "OpenHostFileSystemWithOption"},
        {37, D<&FSP_SRV::CreateSaveDataFileSystemWithCreationInfo2>, "CreateSaveDataFileSystemWithCreationInfo2"},
        {51, D<&FSP_SRV::OpenSaveDataFileSystem>, "OpenSaveDataFileSystem"},
        {52, D<&FSP_SRV::OpenSaveDataFileSystemBySystemSaveDataId>, "OpenSaveDataFileSystemBySystemSaveDataId"},
        {53, D<&FSP_SRV::OpenReadOnlySaveDataFileSystem>, "OpenReadOnlySaveDataFileSystem"},
        {57, D<&FSP_SRV::ReadSaveDataFileSystemExtraDataBySaveDataSpaceId>, "ReadSaveDataFileSystemExtraDataBySaveDataSpaceId"},
        {58, D<&FSP_SRV::ReadSaveDataFileSystemExtraData>, "ReadSaveDataFileSystemExtraData"},
        {59, D<&FSP_SRV::WriteSaveDataFileSystemExtraData>, "WriteSaveDataFileSystemExtraData"},
        {60, nullptr, "OpenSaveDataInfoReader"},
        {61, D<&FSP_SRV::OpenSaveDataInfoReaderBySaveDataSpaceId>, "OpenSaveDataInfoReaderBySaveDataSpaceId"},
        {62, D<&FSP_SRV::OpenSaveDataInfoReaderOnlyCacheStorage>, "OpenSaveDataInfoReaderOnlyCacheStorage"},
        {64, nullptr, "OpenSaveDataInternalStorageFileSystem"},
        {65, nullptr, "UpdateSaveDataMacForDebug"},
        {66, nullptr, "WriteSaveDataFileSystemExtraData2"},
        {67, D<&FSP_SRV::FindSaveDataWithFilter>, "FindSaveDataWithFilter"},
        {68, nullptr, "OpenSaveDataInfoReaderBySaveDataFilter"},
        {69, D<&FSP_SRV::ReadSaveDataFileSystemExtraDataBySaveDataAttribute>, "ReadSaveDataFileSystemExtraDataBySaveDataAttribute"},
        {70, D<&FSP_SRV::WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute>, "WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute"},
        {71, D<&FSP_SRV::ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute>, "ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute"},
        {80, nullptr, "OpenSaveDataMetaFile"},
        {81, nullptr, "OpenSaveDataTransferManager"},
        {82, nullptr, "OpenSaveDataTransferManagerVersion2"},
        {83, D<&FSP_SRV::OpenSaveDataTransferProhibiter>, "OpenSaveDataTransferProhibiter"},
        {84, nullptr, "ListApplicationAccessibleSaveDataOwnerId"},
        {85, nullptr, "OpenSaveDataTransferManagerForSaveDataRepair"},
        {86, nullptr, "OpenSaveDataMover"},
        {87, nullptr, "OpenSaveDataTransferManagerForRepair"},
        {100, nullptr, "OpenImageDirectoryFileSystem"},
        {101, nullptr, "OpenBaseFileSystem"},
        {102, nullptr, "FormatBaseFileSystem"},
        {110, nullptr, "OpenContentStorageFileSystem"},
        {120, nullptr, "OpenCloudBackupWorkStorageFileSystem"},
        {130, nullptr, "OpenCustomStorageFileSystem"},
        {200, D<&FSP_SRV::OpenDataStorageByCurrentProcess>, "OpenDataStorageByCurrentProcess"},
        {201, nullptr, "OpenDataStorageByProgramId"},
        {202, D<&FSP_SRV::OpenDataStorageByDataId>, "OpenDataStorageByDataId"},
        {203, D<&FSP_SRV::OpenPatchDataStorageByCurrentProcess>, "OpenPatchDataStorageByCurrentProcess"},
        {204, nullptr, "OpenDataFileSystemByProgramIndex"},
        {205, D<&FSP_SRV::OpenDataStorageWithProgramIndex>, "OpenDataStorageWithProgramIndex"},
        {206, nullptr, "OpenDataStorageByPath"},
        {210, D<&FSP_SRV::SetCurrentProcess>, "SetCurrentProcess"},
        {400, nullptr, "OpenDeviceOperator"},
        {500, nullptr, "OpenSdCardDetectionEventNotifier"},
        {501, nullptr, "OpenGameCardDetectionEventNotifier"},
        {510, nullptr, "OpenSystemDataUpdateEventNotifier"},
        {511, nullptr, "NotifySystemDataUpdateEvent"},
        {520, nullptr, "SimulateGameCardDetectionEvent"},
        {600, nullptr, "SetCurrentPosixTime"},
        {601, nullptr, "QuerySaveDataTotalSize"},
        {602, nullptr, "VerifySaveDataFileSystem"},
        {603, nullptr, "CorruptSaveDataFileSystem"},
        {604, nullptr, "CreatePaddingFile"},
        {605, nullptr, "DeleteAllPaddingFiles"},
        {606, nullptr, "GetRightsId"},
        {607, nullptr, "RegisterExternalKey"},
        {608, nullptr, "UnregisterAllExternalKey"},
        {609, nullptr, "GetRightsIdByPath"},
        {610, nullptr, "GetRightsIdAndKeyGenerationByPath"},
        {611, nullptr, "SetCurrentPosixTimeWithTimeDifference"},
        {612, nullptr, "GetFreeSpaceSizeForSaveData"},
        {613, nullptr, "VerifySaveDataFileSystemBySaveDataSpaceId"},
        {614, nullptr, "CorruptSaveDataFileSystemBySaveDataSpaceId"},
        {615, nullptr, "QuerySaveDataInternalStorageTotalSize"},
        {616, nullptr, "GetSaveDataCommitId"},
        {617, nullptr, "UnregisterExternalKey"},
        {620, nullptr, "SetSdCardEncryptionSeed"},
        {630, nullptr, "SetSdCardAccessibility"},
        {631, D<&FSP_SRV::IsSdCardAccessible>, "IsSdCardAccessible"},
        {640, nullptr, "IsSignedSystemPartitionOnSdCardValid"},
        {700, nullptr, "OpenAccessFailureResolver"},
        {701, nullptr, "GetAccessFailureDetectionEvent"},
        {702, nullptr, "IsAccessFailureDetected"},
        {710, nullptr, "ResolveAccessFailure"},
        {720, nullptr, "AbandonAccessFailure"},
        {800, nullptr, "GetAndClearFileSystemProxyErrorInfo"},
        {810, nullptr, "RegisterProgramIndexMapInfo"},
        {820, nullptr, "GetContentStorageInfoIndex"},
        {830, nullptr, "EncryptStreamPlaySaveData"},
        {831, nullptr, "DecryptStreamPlaySaveData"},
        {1000, nullptr, "SetBisRootForHost"},
        {1001, nullptr, "SetSaveDataSize"},
        {1002, nullptr, "SetSaveDataRootPath"},
        {1003, D<&FSP_SRV::DisableAutoSaveDataCreation>, "DisableAutoSaveDataCreation"},
        {1004, D<&FSP_SRV::SetGlobalAccessLogMode>, "SetGlobalAccessLogMode"},
        {1005, D<&FSP_SRV::GetGlobalAccessLogMode>, "GetGlobalAccessLogMode"},
        {1006, D<&FSP_SRV::OutputAccessLogToSdCard>, "OutputAccessLogToSdCard"},
        {1007, nullptr, "RegisterUpdatePartition"},
        {1008, nullptr, "OpenRegisteredUpdatePartition"},
        {1009, nullptr, "GetAndClearMemoryReportInfo"},
        {1010, nullptr, "SetDataStorageRedirectTarget"},
        {1011, D<&FSP_SRV::GetProgramIndexForAccessLog>, "GetProgramIndexForAccessLog"},
        {1012, nullptr, "GetFsStackUsage"},
        {1013, nullptr, "UnsetSaveDataRootPath"},
        {1014, nullptr, "OutputMultiProgramTagAccessLog"},
        {1016, D<&FSP_SRV::FlushAccessLogOnSdCard>, "FlushAccessLogOnSdCard"},
        {1017, nullptr, "OutputApplicationInfoAccessLog"},
        {1018, nullptr, "SetDebugOption"},
        {1019, nullptr, "UnsetDebugOption"},
        {1100, nullptr, "OverrideSaveDataTransferTokenSignVerificationKey"},
        {1110, nullptr, "CorruptSaveDataFileSystemBySaveDataSpaceId2"},
        {1200, D<&FSP_SRV::OpenMultiCommitManager>, "OpenMultiCommitManager"},
        {1300, nullptr, "OpenBisWiper"},
    };
    // clang-format on
    RegisterHandlers(functions);

    if (Settings::values.enable_fs_access_log) {
        access_log_mode = AccessLogMode::SdCard;
    }
}

FSP_SRV::~FSP_SRV() = default;

void FSP_SRV::PulseCapture(Pulse::HandlerCapture& capture) const {
    capture.state.U64(current_process_id);
    capture.state.U32(static_cast<u32>(access_log_mode));
    capture.state.U32(access_log_program_index);
}

SessionRequestHandlerPtr FSP_SRV::PulseRestore(std::string_view kind, Pulse::HandlerRestore& restore) {
    auto& in = restore.state;
    if (kind == PulseKind()) {
        const u64 pid = in.U64();
        access_log_mode = static_cast<AccessLogMode>(in.U32());
        access_log_program_index = in.U32();
        in.End();
        if (pid && SetCurrentProcess(ClientProcessId{pid}).IsError()) {
            throw std::invalid_argument("Pulse: fsp-srv does not know the restored process");
        }
        return shared_from_this();
    }
    if (kind == "IFileSystem") {
        const auto origin = static_cast<IFileSystem::PulseOrigin>(in.U32());
        std::vector<u8> args(in.Count(1));
        in.Bytes(args);
        in.End();
        Pulse::StateReader arg{args};
        std::shared_ptr<IFileSystem> filesystem;
        Result result = ResultSuccess;
        switch (origin) {
        case IFileSystem::PulseOrigin::SdCard:
            result = OpenSdCardFileSystem(&filesystem);
            break;
        case IFileSystem::PulseOrigin::DataCurrentProcess:
            if (!romfs_controller) {
                throw std::invalid_argument("Pulse: fsp-srv content without a process");
            }
            result = OpenDataFileSystemByCurrentProcess(&filesystem);
            break;
        case IFileSystem::PulseOrigin::SaveData:
        case IFileSystem::PulseOrigin::SystemSaveData: {
            const auto space_id = arg.Raw<FileSys::SaveDataSpaceId>();
            const auto attribute = arg.Raw<FileSys::SaveDataAttribute>();
            if (!save_data_controller) {
                throw std::invalid_argument("Pulse: fsp-srv save data without a process");
            }
            result = origin == IFileSystem::PulseOrigin::SaveData
                         ? OpenSaveDataFileSystem(&filesystem, space_id, attribute)
                         : OpenSaveDataFileSystemBySystemSaveDataId(&filesystem, space_id, attribute);
            break;
        }
        default:
            throw std::invalid_argument("Pulse: unknown file system origin");
        }
        arg.End();
        if (result.IsError() || !filesystem) {
            throw std::invalid_argument("Pulse: fsp-srv could not reopen a file system");
        }
        return filesystem;
    }
    if (kind == "IFile") {
        const auto parent = restore.Resolve(in.U64());
        const auto path = in.String();
        const u32 mode = in.U32();
        in.End();
        if (!parent || parent->PulseKind() != "IFileSystem") {
            throw std::invalid_argument("Pulse: file without its file system");
        }
        std::shared_ptr<IFile> file;
        if (std::static_pointer_cast<IFileSystem>(parent)->PulseOpenFile(&file, path, mode).IsError()) {
            throw std::invalid_argument("Pulse: fsp-srv could not reopen " + path);
        }
        return file;
    }
    if (kind == "IStorage") {
        const auto opener = restore.Resolve(in.U64());
        const u32 origin = in.U32();
        std::vector<u8> args(in.Count(1));
        in.Bytes(args);
        in.End();
        if (!opener || opener->PulseKind() != PulseKind()) {
            throw std::invalid_argument("Pulse: storage without the fsp-srv session that opened it");
        }
        Pulse::StateReader arg{args};
        auto storage = std::static_pointer_cast<FSP_SRV>(opener)->PulseOpenStorage(origin, arg);
        arg.End();
        return storage;
    }
    return nullptr;
}

std::shared_ptr<IStorage> FSP_SRV::PulseOpenStorage(u32 origin, Pulse::StateReader& args) {
    if (!romfs_controller) throw std::invalid_argument("Pulse: fsp-srv content without a process");
    std::shared_ptr<IStorage> storage;
    Result result = ResultSuccess;
    switch (static_cast<IStorage::PulseOrigin>(origin)) {
    case IStorage::PulseOrigin::CurrentProcess:
        result = OpenDataStorageByCurrentProcess(&storage);
        break;
    case IStorage::PulseOrigin::DataId: {
        const auto storage_id = args.Raw<FileSys::StorageId>();
        const u32 unknown = args.U32();
        const u64 title_id = args.U64();
        result = OpenDataStorageByDataId(&storage, storage_id, unknown, title_id);
        break;
    }
    case IStorage::PulseOrigin::ProgramIndex:
        result = OpenDataStorageWithProgramIndex(&storage, args.U8());
        break;
    default:
        throw std::invalid_argument("Pulse: unknown storage origin");
    }
    if (result.IsError() || !storage) {
        throw std::invalid_argument("Pulse: fsp-srv could not reopen a storage");
    }
    return storage;
}

Result FSP_SRV::SetCurrentProcess(ClientProcessId pid) {
    current_process_id = *pid;

    LOG_DEBUG(Service_FS, "called. current_process_id={:#016x}", current_process_id);

    R_RETURN(
        fsc.OpenProcess(&program_id, &save_data_controller, &romfs_controller, current_process_id));
}

Result FSP_SRV::OpenFileSystemWithPatch(OutInterface<IFileSystem> out_interface,
                                        FileSystemProxyType type, u64 open_program_id) {
    LOG_ERROR(Service_FS, "(STUBBED) called with type={}, program_id={:016X}", type,
              open_program_id);

    // FIXME: many issues with this
    ASSERT(type == FileSystemProxyType::Manual);
    const auto manual_romfs = romfs_controller->OpenPatchedRomFS(
        open_program_id, FileSys::ContentRecordType::HtmlDocument);

    ASSERT(manual_romfs != nullptr);

    const auto extracted_romfs = FileSys::ExtractRomFS(manual_romfs);
    ASSERT(extracted_romfs != nullptr);

    *out_interface = std::make_shared<IFileSystem>(
        system, extracted_romfs, SizeGetter::FromStorageId(fsc, FileSys::StorageId::NandUser));

    R_SUCCEED();
}

Result FSP_SRV::OpenSdCardFileSystem(OutInterface<IFileSystem> out_interface) {
    LOG_DEBUG(Service_FS, "called");

    FileSys::VirtualDir sdmc_dir{};
    fsc.OpenSDMC(&sdmc_dir);

    *out_interface = std::make_shared<IFileSystem>(
        system, sdmc_dir, SizeGetter::FromStorageId(fsc, FileSys::StorageId::SdCard));
    (*out_interface)->SetPulseOrigin(IFileSystem::PulseOrigin::SdCard, {});

    R_SUCCEED();
}

Result FSP_SRV::CreateSaveDataFileSystem(FileSys::SaveDataCreationInfo save_create_struct,
                                         FileSys::SaveDataAttribute save_struct, u128 uid) {
    LOG_DEBUG(Service_FS, "called save_struct = {}, uid = {:016X}{:016X}", save_struct.DebugInfo(),
              uid[1], uid[0]);

    FileSys::VirtualDir save_data_dir{};
    R_RETURN(save_data_controller->CreateSaveData(&save_data_dir, FileSys::SaveDataSpaceId::User,
                                                  save_struct));
}

Result FSP_SRV::CreateSaveDataFileSystemBySystemSaveDataId(
    FileSys::SaveDataAttribute save_struct, FileSys::SaveDataCreationInfo save_create_struct) {
    LOG_DEBUG(Service_FS, "called save_struct = {}", save_struct.DebugInfo());

    FileSys::VirtualDir save_data_dir{};
    R_RETURN(save_data_controller->CreateSaveData(&save_data_dir, FileSys::SaveDataSpaceId::System,
                                                  save_struct));
}

Result FSP_SRV::CreateSaveDataFileSystemWithCreationInfo2(
    FileSys::SaveDataCreationInfo2 save_data_creation_info) {
    FileSys::VirtualDir save_data_dir{};
    R_RETURN(save_data_controller->CreateSaveData(&save_data_dir, save_data_creation_info.space_id,
                                                  save_data_creation_info.attribute));
}

Result FSP_SRV::IsExFatSupported(Out<bool> out_is_supported) {
    LOG_WARNING(Service_FS, "(STUBBED) called");

    *out_is_supported = true;

    R_SUCCEED();
}

Result FSP_SRV::OpenSaveDataFileSystem(OutInterface<IFileSystem> out_interface,
                                       FileSys::SaveDataSpaceId space_id,
                                       FileSys::SaveDataAttribute attribute) {
    LOG_INFO(Service_FS, "called.");

    FileSys::VirtualDir dir{};
    R_TRY(save_data_controller->OpenSaveData(&dir, space_id, attribute));

    FileSys::StorageId id{};
    switch (space_id) {
    case FileSys::SaveDataSpaceId::User:
        id = FileSys::StorageId::NandUser;
        break;
    case FileSys::SaveDataSpaceId::SdSystem:
    case FileSys::SaveDataSpaceId::SdUser:
        id = FileSys::StorageId::SdCard;
        break;
    case FileSys::SaveDataSpaceId::System:
        id = FileSys::StorageId::NandSystem;
        break;
    case FileSys::SaveDataSpaceId::Temporary:
        // ok this is definitely wrong. ASSERT(false) here just kills the whole game the first
        // time it opens cache storage, and plenty of games do that (TOTK for one). there is
        // user-space scratch storage so it belongs on user nand. map it, do not crash.
        id = FileSys::StorageId::NandUser;
        break;
    case FileSys::SaveDataSpaceId::ProperSystem:
    case FileSys::SaveDataSpaceId::SafeMode:
        // same deal for these two. they are system-level spaces so they go on system nand.
        // way better than nuking the title over a save-space id we just did not list out.
        id = FileSys::StorageId::NandSystem;
        break;
    }

    *out_interface =
        std::make_shared<IFileSystem>(system, std::move(dir), SizeGetter::FromStorageId(fsc, id));
    Pulse::StateWriter pulse_args;
    pulse_args.Raw(space_id);
    pulse_args.Raw(attribute);
    (*out_interface)->SetPulseOrigin(IFileSystem::PulseOrigin::SaveData, std::move(pulse_args.bytes));

    R_SUCCEED();
}

Result FSP_SRV::OpenSaveDataFileSystemBySystemSaveDataId(OutInterface<IFileSystem> out_interface,
                                                         FileSys::SaveDataSpaceId space_id,
                                                         FileSys::SaveDataAttribute attribute) {
    LOG_INFO(Service_FS, "called, space_id={}, {}",
             space_id, attribute.DebugInfo());

    R_UNLESS(attribute.system_save_data_id != FileSys::InvalidSystemSaveDataId,
             FileSys::ResultInvalidArgument);

    if (attribute.program_id == 0) {
        attribute.program_id = program_id;
    }

    FileSys::VirtualDir dir{};
    R_TRY(save_data_controller->OpenSaveData(&dir, space_id, attribute));

    FileSys::StorageId id{};
    switch (space_id) {
    case FileSys::SaveDataSpaceId::User:
        id = FileSys::StorageId::NandUser;
        break;
    case FileSys::SaveDataSpaceId::SdSystem:
    case FileSys::SaveDataSpaceId::SdUser:
        id = FileSys::StorageId::SdCard;
        break;
    case FileSys::SaveDataSpaceId::System:
        id = FileSys::StorageId::NandSystem;
        break;
    case FileSys::SaveDataSpaceId::Temporary:
        // same broken switch as OpenSaveDataFileSystem above. do not ASSERT(false) and kill the
        // game over a save-space id, just map Temporary to user nand like it should be.
        id = FileSys::StorageId::NandUser;
        break;
    case FileSys::SaveDataSpaceId::ProperSystem:
    case FileSys::SaveDataSpaceId::SafeMode:
        // system spaces -> system nand. handled, not crashed.
        id = FileSys::StorageId::NandSystem;
        break;
    }

    *out_interface =
        std::make_shared<IFileSystem>(system, std::move(dir), SizeGetter::FromStorageId(fsc, id));
    Pulse::StateWriter pulse_args;
    pulse_args.Raw(space_id);
    pulse_args.Raw(attribute);
    (*out_interface)->SetPulseOrigin(IFileSystem::PulseOrigin::SystemSaveData, std::move(pulse_args.bytes));

    R_SUCCEED();
}

Result FSP_SRV::OpenReadOnlySaveDataFileSystem(OutInterface<IFileSystem> out_interface,
                                               FileSys::SaveDataSpaceId space_id,
                                               FileSys::SaveDataAttribute attribute) {
    LOG_WARNING(Service_FS, "(STUBBED) called, delegating to 51 OpenSaveDataFilesystem");
    R_RETURN(OpenSaveDataFileSystem(out_interface, space_id, attribute));
}

Result FSP_SRV::OpenSaveDataInfoReaderBySaveDataSpaceId(
    OutInterface<ISaveDataInfoReader> out_interface, FileSys::SaveDataSpaceId space) {
    LOG_INFO(Service_FS, "called, space={}", space);

    *out_interface = std::make_shared<ISaveDataInfoReader>(system, save_data_controller, space);

    R_SUCCEED();
}

Result FSP_SRV::OpenSaveDataInfoReaderOnlyCacheStorage(
    OutInterface<ISaveDataInfoReader> out_interface) {
    LOG_WARNING(Service_FS, "(STUBBED) called");

    *out_interface = std::make_shared<ISaveDataInfoReader>(system, save_data_controller,
                                                           FileSys::SaveDataSpaceId::Temporary);

    R_SUCCEED();
}

Result FSP_SRV::FindSaveDataWithFilter(Out<s64> out_count,
                                       OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                       FileSys::SaveDataSpaceId space_id,
                                       FileSys::SaveDataFilter filter) {
    LOG_WARNING(Service_FS, "(STUBBED) called");
    R_THROW(FileSys::ResultTargetNotFound);
}

Result FSP_SRV::WriteSaveDataFileSystemExtraData(InBuffer<BufferAttr_HipcMapAlias> buffer,
                                                 FileSys::SaveDataSpaceId space_id,
                                                 u64 save_data_id) {
    LOG_WARNING(Service_FS, "(STUBBED) called, space_id={}, save_data_id={:016X}", space_id,
                save_data_id);
    R_SUCCEED();
}

Result FSP_SRV::WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute(
    InBuffer<BufferAttr_HipcMapAlias> buffer, InBuffer<BufferAttr_HipcMapAlias> mask_buffer,
    FileSys::SaveDataSpaceId space_id, FileSys::SaveDataAttribute attribute) {
    LOG_WARNING(Service_FS,
                "(STUBBED) called, space_id={}, attribute.program_id={:016X}\n"
                "attribute.user_id={:016X}{:016X}, attribute.save_id={:016X}\n"
                "attribute.type={}, attribute.rank={}, attribute.index={}",
                space_id, attribute.program_id, attribute.user_id[1], attribute.user_id[0],
                attribute.system_save_data_id, attribute.type, attribute.rank, attribute.index);
    R_SUCCEED();
}

Result FSP_SRV::ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute(
    FileSys::SaveDataSpaceId space_id, FileSys::SaveDataAttribute attribute,
    InBuffer<BufferAttr_HipcMapAlias> mask_buffer, OutBuffer<BufferAttr_HipcMapAlias> out_buffer) {
    // Stub this to None for now, backend needs an impl to read/write the SaveDataExtraData
    // In an earlier version of the code, this was returned as an out argument, but this is not
    // correct
    [[maybe_unused]] constexpr auto flags = static_cast<u32>(FileSys::SaveDataFlags::None);

    LOG_WARNING(Service_FS,
                "(STUBBED) called, flags={}, space_id={}, attribute.program_id={:016X}\n"
                "attribute.user_id={:016X}{:016X}, attribute.save_id={:016X}\n"
                "attribute.type={}, attribute.rank={}, attribute.index={}",
                flags, space_id, attribute.program_id, attribute.user_id[1], attribute.user_id[0],
                attribute.system_save_data_id, attribute.type, attribute.rank, attribute.index);

    R_SUCCEED();
}

Result FSP_SRV::ReadSaveDataFileSystemExtraData(OutBuffer<BufferAttr_HipcMapAlias> out_buffer,
                                                u64 save_data_id) {
    // Stub, backend needs an impl to read/write the SaveDataExtraData
    LOG_WARNING(Service_FS, "(STUBBED) called, save_data_id={:016X}", save_data_id);
    std::memset(out_buffer.data(), 0, out_buffer.size());
    R_SUCCEED();
}

Result FSP_SRV::ReadSaveDataFileSystemExtraDataBySaveDataAttribute(
    OutBuffer<BufferAttr_HipcMapAlias> out_buffer, FileSys::SaveDataSpaceId space_id,
    FileSys::SaveDataAttribute attribute) {
    // Stub, backend needs an impl to read/write the SaveDataExtraData
    LOG_WARNING(Service_FS,
                "(STUBBED) called, space_id={}, attribute.program_id={:016X}\n"
                "attribute.user_id={:016X}{:016X}, attribute.save_id={:016X}\n"
                "attribute.type={}, attribute.rank={}, attribute.index={}",
                space_id, attribute.program_id, attribute.user_id[1], attribute.user_id[0],
                attribute.system_save_data_id, attribute.type, attribute.rank, attribute.index);
    std::memset(out_buffer.data(), 0, out_buffer.size());
    R_SUCCEED();
}

Result FSP_SRV::ReadSaveDataFileSystemExtraDataBySaveDataSpaceId(
    OutBuffer<BufferAttr_HipcMapAlias> out_buffer, FileSys::SaveDataSpaceId space_id,
    u64 save_data_id) {
    // Stub, backend needs an impl to read/write the SaveDataExtraData
    LOG_WARNING(Service_FS, "(STUBBED) called, space_id={}, save_data_id={:016X}", space_id,
                save_data_id);
    std::memset(out_buffer.data(), 0, out_buffer.size());
    R_SUCCEED();
}

Result FSP_SRV::OpenSaveDataTransferProhibiter(
    OutInterface<ISaveDataTransferProhibiter> out_prohibiter, u64 id) {
    LOG_WARNING(Service_FS, "(STUBBED) called, id={:016X}", id);
    *out_prohibiter = std::make_shared<ISaveDataTransferProhibiter>(system);
    R_SUCCEED();
}

Result FSP_SRV::OpenDataFileSystemByCurrentProcess(OutInterface<IFileSystem> out_interface) {
    LOG_DEBUG(Service_FS, "called");

    if (!romfs) {
        auto current_romfs = romfs_controller->OpenRomFSCurrentProcess();
        if (!current_romfs) {
            LOG_CRITICAL(Service_FS, "No file system interface available!");
            R_RETURN(ResultUnknown);
        }

        romfs = current_romfs;
    }

    auto extracted_romfs = FileSys::ExtractRomFS(romfs);
    if (!extracted_romfs) {
        LOG_CRITICAL(Service_FS, "Failed to extract RomFS for the current process!");
        R_RETURN(ResultUnknown);
    }

    *out_interface = std::make_shared<IFileSystem>(
        system, extracted_romfs, SizeGetter::FromStorageId(fsc, FileSys::StorageId::NandUser));
    (*out_interface)->SetPulseOrigin(IFileSystem::PulseOrigin::DataCurrentProcess, {});

    R_SUCCEED();
}

Result FSP_SRV::OpenDataStorageByCurrentProcess(OutInterface<IStorage> out_interface) {
    LOG_DEBUG(Service_FS, "called");

    if (!romfs) {
        auto current_romfs = romfs_controller->OpenRomFSCurrentProcess();
        if (!current_romfs) {
            // TODO (bunnei): Find the right error code to use here
            LOG_CRITICAL(Service_FS, "No file system interface available!");
            R_RETURN(ResultUnknown);
        }

        romfs = current_romfs;
    }

    *out_interface = std::make_shared<IStorage>(system, romfs);
    (*out_interface)->SetPulseOrigin(shared_from_this(), IStorage::PulseOrigin::CurrentProcess, {});

    R_SUCCEED();
}

Result FSP_SRV::OpenDataStorageByDataId(OutInterface<IStorage> out_interface,
                                        FileSys::StorageId storage_id, u32 unknown, u64 title_id) {
    LOG_DEBUG(Service_FS, "called with storage_id={:02X}, unknown={:08X}, title_id={:016X}",
              storage_id, unknown, title_id);

    Pulse::StateWriter pulse_args;
    pulse_args.Raw(storage_id);
    pulse_args.U32(unknown);
    pulse_args.U64(title_id);

    auto data = romfs_controller->OpenRomFS(title_id, storage_id, FileSys::ContentRecordType::Data);

    if (!data) {
        const auto archive = FileSys::SystemArchive::SynthesizeSystemArchive(title_id);

        if (archive != nullptr) {
            *out_interface = std::make_shared<IStorage>(system, archive);
            (*out_interface)->SetPulseOrigin(shared_from_this(), IStorage::PulseOrigin::DataId,
                                             std::move(pulse_args.bytes));
            R_SUCCEED();
        }

        // TODO(DarkLordZach): Find the right error code to use here
        LOG_ERROR(Service_FS,
                  "Could not open data storage with title_id={:016X}, storage_id={:02X}", title_id,
                  storage_id);
        R_RETURN(ResultUnknown);
    }

    const FileSys::PatchManager pm{title_id, fsc, content_provider};

    auto base =
        romfs_controller->OpenBaseNca(title_id, storage_id, FileSys::ContentRecordType::Data);
    auto storage = std::make_shared<IStorage>(
        system, pm.PatchRomFS(base.get(), std::move(data), FileSys::ContentRecordType::Data));
    storage->SetPulseOrigin(shared_from_this(), IStorage::PulseOrigin::DataId,
                            std::move(pulse_args.bytes));

    *out_interface = std::move(storage);
    R_SUCCEED();
}

Result FSP_SRV::OpenPatchDataStorageByCurrentProcess(OutInterface<IStorage> out_interface,
                                                     FileSys::StorageId storage_id, u64 title_id) {
    LOG_WARNING(Service_FS, "(STUBBED) called with storage_id={:02X}, title_id={:016X}", storage_id,
                title_id);

    R_RETURN(FileSys::ResultTargetNotFound);
}

Result FSP_SRV::OpenDataStorageWithProgramIndex(OutInterface<IStorage> out_interface,
                                                u8 program_index) {
    LOG_DEBUG(Service_FS, "called, program_index={}", program_index);

    auto patched_romfs = romfs_controller->OpenPatchedRomFSWithProgramIndex(
        program_id, program_index, FileSys::ContentRecordType::Program);

    if (!patched_romfs) {
        // TODO: Find the right error code to use here
        LOG_ERROR(Service_FS, "Could not open storage with program_index={}", program_index);
        R_RETURN(ResultUnknown);
    }

    *out_interface = std::make_shared<IStorage>(system, std::move(patched_romfs));
    Pulse::StateWriter pulse_args;
    pulse_args.U8(program_index);
    (*out_interface)->SetPulseOrigin(shared_from_this(), IStorage::PulseOrigin::ProgramIndex,
                                     std::move(pulse_args.bytes));

    R_SUCCEED();
}

Result FSP_SRV::IsSdCardAccessible(Out<bool> out_is_accessible) {
    LOG_DEBUG(Service_FS, "(STUBBED) called");

    *out_is_accessible = true;

    R_SUCCEED();
}

Result FSP_SRV::DisableAutoSaveDataCreation() {
    LOG_DEBUG(Service_FS, "called");

    save_data_controller->SetAutoCreate(false);

    R_SUCCEED();
}

Result FSP_SRV::SetGlobalAccessLogMode(AccessLogMode access_log_mode_) {
    LOG_DEBUG(Service_FS, "called, access_log_mode={}", access_log_mode_);

    access_log_mode = access_log_mode_;

    R_SUCCEED();
}

Result FSP_SRV::GetGlobalAccessLogMode(Out<AccessLogMode> out_access_log_mode) {
    LOG_DEBUG(Service_FS, "called");

    *out_access_log_mode = access_log_mode;

    R_SUCCEED();
}

Result FSP_SRV::OutputAccessLogToSdCard(InBuffer<BufferAttr_HipcMapAlias> log_message_buffer) {
    LOG_DEBUG(Service_FS, "called");

    auto log = Common::StringFromFixedZeroTerminatedBuffer(
        reinterpret_cast<const char*>(log_message_buffer.data()), log_message_buffer.size());
    reporter.SaveFSAccessLog(log);

    R_SUCCEED();
}

Result FSP_SRV::GetProgramIndexForAccessLog(Out<AccessLogVersion> out_access_log_version,
                                            Out<u32> out_access_log_program_index) {
    LOG_DEBUG(Service_FS, "(STUBBED) called");

    *out_access_log_version = AccessLogVersion::Latest;
    *out_access_log_program_index = access_log_program_index;

    R_SUCCEED();
}

Result FSP_SRV::FlushAccessLogOnSdCard() {
    LOG_DEBUG(Service_FS, "(STUBBED) called");

    R_SUCCEED();
}

Result FSP_SRV::ExtendSaveDataFileSystem(FileSys::SaveDataSpaceId space_id, u64 save_data_id,
                                         s64 available_size, s64 journal_size) {
    // We don't have an index of save data ids, so we can't implement this.
    LOG_WARNING(Service_FS,
                "(STUBBED) called, space_id={}, save_data_id={:016X}, available_size={:#x}, "
                "journal_size={:#x}",
                space_id, save_data_id, available_size, journal_size);
    R_SUCCEED();
}

Result FSP_SRV::GetCacheStorageSize(s32 index, Out<s64> out_data_size, Out<s64> out_journal_size) {
    LOG_WARNING(Service_FS, "(STUBBED) called with index={}", index);

    *out_data_size = 0;
    *out_journal_size = 0;

    R_SUCCEED();
}

Result FSP_SRV::OpenMultiCommitManager(OutInterface<IMultiCommitManager> out_interface) {
    LOG_DEBUG(Service_FS, "called");

    *out_interface = std::make_shared<IMultiCommitManager>(system);

    R_SUCCEED();
}

} // namespace Service::FileSystem

# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later

# libarchive, read side only: the zip, 7z, rar and rar5 readers the Xbox mod manager opens. Its own
# CMake probes the system for zlib, zstd and liblzma at configure time, before this tree has built
# them, so this file compiles the reader sources against a fixed config.h instead.

set(LA "${libarchive_SOURCE_DIR}/libarchive")

add_library(archive_read STATIC
    # Core reader, entries and strings.
    ${LA}/archive_acl.c
    ${LA}/archive_check_magic.c
    ${LA}/archive_cmdline.c
    ${LA}/archive_entry.c
    ${LA}/archive_entry_sparse.c
    ${LA}/archive_entry_stat.c
    ${LA}/archive_entry_strmode.c
    ${LA}/archive_entry_xattr.c
    ${LA}/archive_options.c
    ${LA}/archive_pack_dev.c
    ${LA}/archive_rb.c
    ${LA}/archive_read.c
    ${LA}/archive_read_add_passphrase.c
    ${LA}/archive_read_open_memory.c
    ${LA}/archive_read_set_format.c
    ${LA}/archive_read_set_options.c
    ${LA}/archive_read_support_filter_none.c
    ${LA}/archive_string.c
    ${LA}/archive_string_sprintf.c
    ${LA}/archive_time.c
    ${LA}/archive_util.c
    ${LA}/archive_version_details.c
    ${LA}/archive_virtual.c
    ${LA}/archive_windows.c
    # Formats, and the codecs and checksums they carry themselves.
    ${LA}/archive_read_support_format_7zip.c
    ${LA}/archive_read_support_format_rar.c
    ${LA}/archive_read_support_format_rar5.c
    ${LA}/archive_read_support_format_zip.c
    ${LA}/archive_blake2s_ref.c
    ${LA}/archive_blake2sp_ref.c
    ${LA}/archive_cryptor.c
    ${LA}/archive_digest.c
    ${LA}/archive_hmac.c
    ${LA}/archive_ppmd7.c
    ${LA}/archive_ppmd8.c
    ${LA}/archive_random.c
)

target_include_directories(archive_read
    PUBLIC "${LA}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(archive_read
    PUBLIC LIBARCHIVE_STATIC
    PRIVATE HAVE_CONFIG_H LZMA_API_STATIC _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
if (MSVC)
    # Third-party C: keep the build log for our own warnings.
    target_compile_options(archive_read PRIVATE /W0)
endif()
target_link_libraries(archive_read PRIVATE ZLIB::ZLIB zstd::zstd liblzma bcrypt)

add_library(LibArchive::Read ALIAS archive_read)

# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later

# A nested job limits process commit; it does not emulate Xbox unified GPU memory.
# No KILL_ON_JOB_CLOSE: the limit survives this launcher until the app exits itself.
if (-not ('EdenXbox.ProcessMemoryLimit' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace EdenXbox {
    public static class ProcessMemoryLimit {
        [StructLayout(LayoutKind.Sequential)]
        struct Basic {
            public long ProcessTime, JobTime;
            public uint Flags;
            public UIntPtr MinimumWorkingSet, MaximumWorkingSet;
            public uint ActiveProcesses;
            public UIntPtr Affinity;
            public uint Priority, Scheduling;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct Io {
            public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct Extended {
            public Basic Basic;
            public Io Io;
            public UIntPtr ProcessMemory, JobMemory, PeakProcess, PeakJob;
        }
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern IntPtr CreateJobObject(IntPtr attributes, string name);
        [DllImport("kernel32.dll", SetLastError=true)]
        static extern bool SetInformationJobObject(IntPtr job, int kind, ref Extended info, uint bytes);
        [DllImport("kernel32.dll", SetLastError=true)]
        static extern bool QueryInformationJobObject(IntPtr job, int kind, out Extended info, uint bytes, IntPtr returned);
        [DllImport("kernel32.dll", SetLastError=true)]
        static extern IntPtr OpenProcess(uint access, bool inherit, int processId);
        [DllImport("kernel32.dll", SetLastError=true)]
        static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
        [DllImport("kernel32.dll", SetLastError=true)]
        static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool present);
        [DllImport("kernel32.dll")]
        static extern bool CloseHandle(IntPtr handle);
        static void Check(bool ok) {
            if (!ok) throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        public static void Apply(int processId, ulong bytes) {
            if (bytes == 0 || IntPtr.Size != 8) throw new ArgumentException("A positive limit and x64 launcher are required.");
            IntPtr job = CreateJobObject(IntPtr.Zero, null);
            Check(job != IntPtr.Zero);
            IntPtr process = IntPtr.Zero;
            try {
                Extended limits = new Extended();
                limits.Basic.Flags = 0x100; // JOB_OBJECT_LIMIT_PROCESS_MEMORY
                limits.ProcessMemory = new UIntPtr(bytes);
                uint size = (uint)Marshal.SizeOf(typeof(Extended));
                Check(SetInformationJobObject(job, 9, ref limits, size));
                // Assignment needs SET_QUOTA/TERMINATE; verification needs QUERY_INFORMATION.
                process = OpenProcess(0x501, false, processId);
                Check(process != IntPtr.Zero);
                Check(AssignProcessToJobObject(job, process));
                bool present;
                Check(IsProcessInJob(process, job, out present));
                Extended actual;
                Check(QueryInformationJobObject(job, 9, out actual, size, IntPtr.Zero));
                if (!present || actual.ProcessMemory.ToUInt64() != bytes)
                    throw new InvalidOperationException("Memory limit verification failed.");
            } finally {
                if (process != IntPtr.Zero) CloseHandle(process);
                CloseHandle(job);
            }
        }
    }
}
'@
}

function Set-EdenProcessMemoryLimit {
    param([int] $ProcessId, [ValidateRange(1, 1048576)] [int] $LimitMiB)
    [EdenXbox.ProcessMemoryLimit]::Apply($ProcessId, [uint64]$LimitMiB * 1MB)
}


/**
 * @file dll/fuse/fuse.c
 *
 * @copyright 2015-2016 Bill Zissimopoulos
 */
/*
 * This file is part of WinFsp.
 *
 * You can redistribute it and/or modify it under the terms of the
 * GNU Affero General Public License version 3 as published by the
 * Free Software Foundation.
 *
 * Licensees holding a valid commercial license may use this file in
 * accordance with the commercial license agreement provided with the
 * software.
 */

#include <dll/library.h>
#include <fuse/fuse.h>

struct fuse_chan
{
    PWSTR MountPoint;
    UINT8 Buffer[];
};

struct fuse
{
    FSP_FILE_SYSTEM *FileSystem;
    FSP_SERVICE *Service;
    const struct fuse_operations *Ops;
    size_t OpSize;
    void *Data;
    int Environment;
    CRITICAL_SECTION Lock;
};

static DWORD fsp_fuse_tlskey = TLS_OUT_OF_INDEXES;
static INIT_ONCE fsp_fuse_initonce_v = INIT_ONCE_STATIC_INIT;

VOID fsp_fuse_initialize(BOOLEAN Dynamic)
{
}

VOID fsp_fuse_finalize(BOOLEAN Dynamic)
{
    /*
     * This function is called during DLL_PROCESS_DETACH. We must therefore keep
     * finalization tasks to a minimum.
     *
     * We must free our TLS key (if any). We only do so if the library
     * is being explicitly unloaded (rather than the process exiting).
     */

    if (Dynamic && TLS_OUT_OF_INDEXES != fsp_fuse_tlskey)
    {
        /* !!!:
         * We should also free all thread local contexts, which means putting them in a list,
         * protected with a critical section, etc. Arghhh!
         *
         * I am too lazy and I am not going to do that, unless people start using this
         * DLL dynamically (LoadLibrary/FreeLibrary).
         */
        TlsFree(fsp_fuse_tlskey);
    }
}

VOID fsp_fuse_finalize_thread(VOID)
{
    struct fuse_context *context;

    if (TLS_OUT_OF_INDEXES != fsp_fuse_tlskey)
    {
        context = TlsGetValue(fsp_fuse_tlskey);
        if (0 != context)
        {
            MemFree(context);
            TlsSetValue(fsp_fuse_tlskey, 0);
        }
    }
}

static BOOL WINAPI fsp_fuse_initonce_f(
    PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context)
{
    fsp_fuse_tlskey = TlsAlloc();
    return TRUE;
}

static inline VOID fsp_fuse_initonce(VOID)
{
    InitOnceExecuteOnce(&fsp_fuse_initonce_v, fsp_fuse_initonce_f, 0, 0);
}

FSP_FUSE_API int fsp_fuse_version(void)
{
    return FUSE_VERSION;
}

FSP_FUSE_API struct fuse_chan *fsp_fuse_mount(const char *mountpoint, struct fuse_args *args)
{
    struct fuse_chan *ch = 0;
    int Size;

    Size = MultiByteToWideChar(CP_UTF8, 0, mountpoint, -1, 0, 0);
    if (0 == Size)
        goto fail;

    ch = MemAlloc(sizeof *ch + Size);
    if (0 == ch)
        goto fail;

    ch->MountPoint = (PVOID)ch->Buffer;
    Size = MultiByteToWideChar(CP_UTF8, 0, mountpoint, -1, ch->MountPoint, Size);
    if (0 == Size)
        goto fail;

    return ch;

fail:
    FspServiceLog(EVENTLOG_ERROR_TYPE, L"Invalid mount point.");

    MemFree(ch);

    return 0;
}

FSP_FUSE_API void fsp_fuse_unmount(const char *mountpoint, struct fuse_chan *ch)
{
    MemFree(ch);
}

FSP_FUSE_API int fsp_fuse_parse_cmdline(struct fuse_args *args, char **mountpoint,
    int *multithreaded, int *foreground,
    FSP_FUSE_MEMFN_P)
{
    // !!!: NEEDIMPL
    return 0;
}

FSP_FUSE_API int fsp_fuse_main_real(int argc, char *argv[],
    const struct fuse_operations *ops, size_t opsize, void *data,
    int environment)
{
    // !!!: NEEDIMPL
    return 0;
}

FSP_FUSE_API int fsp_fuse_is_lib_option(const char *opt,
    FSP_FUSE_MEMFN_P)
{
    // !!!: NEEDIMPL
    return 0;
}

static NTSTATUS fsp_fuse_svcstart(FSP_SERVICE *Service, ULONG argc, PWSTR *argv)
{
    struct fuse *f = Service->UserContext;

    return FspFileSystemStartDispatcher(f->FileSystem, 0);
}

static NTSTATUS fsp_fuse_svcstop(FSP_SERVICE *Service)
{
    struct fuse *f = Service->UserContext;

    FspFileSystemStopDispatcher(f->FileSystem);

    return STATUS_SUCCESS;
}

FSP_FUSE_API struct fuse *fsp_fuse_new(struct fuse_chan *ch, struct fuse_args *args,
    const struct fuse_operations *ops, size_t opsize, void *data,
    int environment)
{
    struct fuse *f = 0;
    PWSTR ServiceName = FspDiagIdent();
    FSP_FSCTL_VOLUME_PARAMS VolumeParams;
    NTSTATUS Result;

    memset(&VolumeParams, 0, sizeof VolumeParams);
#if 0
    /* initialize VolumeParams from command line args */
    VolumeParams.SectorSize = FSP_FUSE_SECTOR_SIZE;
    VolumeParams.SectorsPerAllocationUnit = FSP_FUSE_SECTORS_PER_ALLOCATION_UNIT;
    VolumeParams.VolumeCreationTime = MemfsGetSystemTime();
    VolumeParams.VolumeSerialNumber = (UINT32)(MemfsGetSystemTime() / (10000 * 1000));
    VolumeParams.FileInfoTimeout = FileInfoTimeout;
    VolumeParams.CaseSensitiveSearch = 1;
    VolumeParams.CasePreservedNames = 1;
    VolumeParams.UnicodeOnDisk = 1;
    VolumeParams.PersistentAcls = 1;
    if (0 != VolumePrefix)
        wcscpy_s(VolumeParams.Prefix, sizeof VolumeParams.Prefix / sizeof(WCHAR), VolumePrefix);
#endif

    f = MemAlloc(sizeof *f);
    if (0 == f)
        goto fail;
    memset(f, 0, sizeof *f);

    Result = FspServiceCreate(ServiceName, fsp_fuse_svcstart, fsp_fuse_svcstop, 0, &f->Service);
    if (!NT_SUCCESS(Result))
        goto fail;
    FspServiceAllowConsoleMode(f->Service);
    f->Service->UserContext = f;

    Result = FspFileSystemCreate(L"" FSP_FSCTL_NET_DEVICE_NAME, &VolumeParams, 0, &f->FileSystem);
    if (!NT_SUCCESS(Result))
        goto fail;

    Result = FspFileSystemSetMountPoint(f->FileSystem, ch->MountPoint);
    if (!NT_SUCCESS(Result))
        goto fail;

    f->Ops = ops;
    f->OpSize = opsize;
    f->Data = data;
    f->Environment = environment;

    InitializeCriticalSection(&f->Lock);

    return f;

fail:
    FspServiceLog(EVENTLOG_ERROR_TYPE, L"Unable to create FUSE file system.");

    if (0 != f)
    {
        if (0 != f->FileSystem)
            FspFileSystemDelete(f->FileSystem);

        if (0 != f->Service)
            FspServiceDelete(f->Service);

        MemFree(f);
    }

    return 0;
}

FSP_FUSE_API void fsp_fuse_destroy(struct fuse *f)
{
    DeleteCriticalSection(&f->Lock);

    FspFileSystemDelete(f->FileSystem);

    FspServiceDelete(f->Service);

    MemFree(f);
}

FSP_FUSE_API int fsp_fuse_loop(struct fuse *f)
{
    NTSTATUS Result;
    ULONG ExitCode;

    Result = FspServiceLoop(f->Service);
    ExitCode = FspServiceGetExitCode(f->Service);

    if (!NT_SUCCESS(Result))
        goto fail;

    if (0 != ExitCode)
        FspServiceLog(EVENTLOG_WARNING_TYPE,
            L"The service %s has exited (ExitCode=%ld).", f->Service->ServiceName, ExitCode);

    return 0;

fail:
    FspServiceLog(EVENTLOG_ERROR_TYPE,
        L"The service %s has failed to run (Status=%lx).", f->Service->ServiceName, Result);

    return -1;
}

FSP_FUSE_API int fsp_fuse_loop_mt(struct fuse *f)
{
    return fsp_fuse_loop(f);
}

FSP_FUSE_API void fsp_fuse_exit(struct fuse *f)
{
    FspServiceStop(f->Service);
}

FSP_FUSE_API struct fuse_context *fsp_fuse_get_context(void)
{
    struct fuse_context *context;

    fsp_fuse_initonce();
    if (TLS_OUT_OF_INDEXES == fsp_fuse_tlskey)
        return 0;

    context = TlsGetValue(fsp_fuse_tlskey);
    if (0 == context)
    {
        context = MemAlloc(sizeof *context);
        if (0 == context)
            return 0;

        memset(context, 0, sizeof *context);

        TlsSetValue(fsp_fuse_tlskey, context);
    }

    return context;
}
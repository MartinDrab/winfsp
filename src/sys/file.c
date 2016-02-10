/**
 * @file sys/file.c
 *
 * @copyright 2015 Bill Zissimopoulos
 */

#include <sys/driver.h>

NTSTATUS FspFileNodeCreate(PDEVICE_OBJECT DeviceObject,
    ULONG ExtraSize, FSP_FILE_NODE **PFileNode);
VOID FspFileNodeDelete(FSP_FILE_NODE *FileNode);
VOID FspFileNodeAcquireSharedF(FSP_FILE_NODE *FileNode, ULONG Flags);
BOOLEAN FspFileNodeTryAcquireSharedF(FSP_FILE_NODE *FileNode, ULONG Flags);
VOID FspFileNodeAcquireExclusiveF(FSP_FILE_NODE *FileNode, ULONG Flags);
BOOLEAN FspFileNodeTryAcquireExclusiveF(FSP_FILE_NODE *FileNode, ULONG Flags);
VOID FspFileNodeSetOwnerF(FSP_FILE_NODE *FileNode, ULONG Flags, PVOID Owner);
VOID FspFileNodeReleaseF(FSP_FILE_NODE *FileNode, ULONG Flags);
VOID FspFileNodeReleaseOwnerF(FSP_FILE_NODE *FileNode, ULONG Flags, PVOID Owner);
FSP_FILE_NODE *FspFileNodeOpen(FSP_FILE_NODE *FileNode, PFILE_OBJECT FileObject,
    UINT32 GrantedAccess, UINT32 ShareAccess, NTSTATUS *PResult);
VOID FspFileNodeClose(FSP_FILE_NODE *FileNode, PFILE_OBJECT FileObject,
    PBOOLEAN PDeletePending);
VOID FspFileNodeRename(FSP_FILE_NODE *FileNode, PUNICODE_STRING NewFileName);
VOID FspFileNodeGetFileInfo(FSP_FILE_NODE *FileNode, FSP_FSCTL_FILE_INFO *FileInfo);
BOOLEAN FspFileNodeTryGetFileInfo(FSP_FILE_NODE *FileNode, FSP_FSCTL_FILE_INFO *FileInfo);
VOID FspFileNodeSetFileInfo(FSP_FILE_NODE *FileNode, PFILE_OBJECT CcFileObject,
    const FSP_FSCTL_FILE_INFO *FileInfo);
BOOLEAN FspFileNodeTrySetFileInfo(FSP_FILE_NODE *FileNode, PFILE_OBJECT CcFileObject,
    const FSP_FSCTL_FILE_INFO *FileInfo, ULONG InfoChangeNumber);
NTSTATUS FspFileDescCreate(FSP_FILE_DESC **PFileDesc);
VOID FspFileDescDelete(FSP_FILE_DESC *FileDesc);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, FspFileNodeCreate)
#pragma alloc_text(PAGE, FspFileNodeDelete)
#pragma alloc_text(PAGE, FspFileNodeAcquireSharedF)
#pragma alloc_text(PAGE, FspFileNodeTryAcquireSharedF)
#pragma alloc_text(PAGE, FspFileNodeAcquireExclusiveF)
#pragma alloc_text(PAGE, FspFileNodeTryAcquireExclusiveF)
#pragma alloc_text(PAGE, FspFileNodeSetOwnerF)
#pragma alloc_text(PAGE, FspFileNodeReleaseF)
#pragma alloc_text(PAGE, FspFileNodeReleaseOwnerF)
#pragma alloc_text(PAGE, FspFileNodeOpen)
#pragma alloc_text(PAGE, FspFileNodeClose)
#pragma alloc_text(PAGE, FspFileNodeRename)
#pragma alloc_text(PAGE, FspFileNodeGetFileInfo)
#pragma alloc_text(PAGE, FspFileNodeTryGetFileInfo)
#pragma alloc_text(PAGE, FspFileNodeSetFileInfo)
#pragma alloc_text(PAGE, FspFileNodeTrySetFileInfo)
#pragma alloc_text(PAGE, FspFileDescCreate)
#pragma alloc_text(PAGE, FspFileDescDelete)
#endif

NTSTATUS FspFileNodeCreate(PDEVICE_OBJECT DeviceObject,
    ULONG ExtraSize, FSP_FILE_NODE **PFileNode)
{
    PAGED_CODE();

    *PFileNode = 0;

    FSP_FILE_NODE_NONPAGED *NonPaged = FspAllocNonPaged(sizeof *NonPaged);
    if (0 == NonPaged)
        return STATUS_INSUFFICIENT_RESOURCES;

    FSP_FILE_NODE *FileNode = FspAlloc(sizeof *FileNode + ExtraSize);
    if (0 == FileNode)
    {
        FspFree(NonPaged);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(NonPaged, sizeof *NonPaged);
    ExInitializeResourceLite(&NonPaged->Resource);
    ExInitializeResourceLite(&NonPaged->PagingIoResource);
    ExInitializeFastMutex(&NonPaged->HeaderFastMutex);

    RtlZeroMemory(FileNode, sizeof *FileNode + ExtraSize);
    FileNode->Header.NodeTypeCode = FspFileNodeFileKind;
    FileNode->Header.NodeByteSize = sizeof *FileNode;
    FileNode->Header.IsFastIoPossible = FastIoIsQuestionable;
    FileNode->Header.Resource = &NonPaged->Resource;
    FileNode->Header.PagingIoResource = &NonPaged->PagingIoResource;
    FileNode->Header.ValidDataLength.QuadPart = MAXLONGLONG;
        /* disable ValidDataLength functionality */
    FsRtlSetupAdvancedHeader(&FileNode->Header, &NonPaged->HeaderFastMutex);
    FileNode->NonPaged = NonPaged;
    FileNode->RefCount = 1;
    FileNode->FsvolDeviceObject = DeviceObject;
    FspDeviceReference(FileNode->FsvolDeviceObject);
    RtlInitEmptyUnicodeString(&FileNode->FileName, FileNode->FileNameBuf, (USHORT)ExtraSize);

    *PFileNode = FileNode;

    return STATUS_SUCCESS;
}

VOID FspFileNodeDelete(FSP_FILE_NODE *FileNode)
{
    PAGED_CODE();

    FsRtlTeardownPerStreamContexts(&FileNode->Header);

    FspDeviceDereference(FileNode->FsvolDeviceObject);

    if (0 != FileNode->ExternalFileName)
        FspFree(FileNode->ExternalFileName);

    ExDeleteResourceLite(&FileNode->NonPaged->PagingIoResource);
    ExDeleteResourceLite(&FileNode->NonPaged->Resource);
    FspFree(FileNode->NonPaged);

    FspFree(FileNode);
}

VOID FspFileNodeAcquireSharedF(FSP_FILE_NODE *FileNode, ULONG Flags)
{
    PAGED_CODE();

    if (Flags & FspFileNodeAcquireMain)
        ExAcquireResourceSharedLite(FileNode->Header.Resource, TRUE);

    if (Flags & FspFileNodeAcquirePgio)
        ExAcquireResourceSharedLite(FileNode->Header.PagingIoResource, TRUE);
}

BOOLEAN FspFileNodeTryAcquireSharedF(FSP_FILE_NODE *FileNode, ULONG Flags)
{
    PAGED_CODE();

    BOOLEAN Result = FALSE;

    if (Flags & FspFileNodeAcquireMain)
    {
        Result = ExAcquireResourceSharedLite(FileNode->Header.Resource, FALSE);
        if (!Result)
            return FALSE;
    }

    if (Flags & FspFileNodeAcquirePgio)
    {
        Result = ExAcquireResourceSharedLite(FileNode->Header.PagingIoResource, FALSE);
        if (!Result)
        {
            if (Flags & FspFileNodeAcquireMain)
                ExReleaseResourceLite(FileNode->Header.Resource);
            return FALSE;
        }
    }

    return Result;
}

VOID FspFileNodeAcquireExclusiveF(FSP_FILE_NODE *FileNode, ULONG Flags)
{
    PAGED_CODE();

    if (Flags & FspFileNodeAcquireMain)
        ExAcquireResourceExclusiveLite(FileNode->Header.Resource, TRUE);

    if (Flags & FspFileNodeAcquirePgio)
        ExAcquireResourceExclusiveLite(FileNode->Header.PagingIoResource, TRUE);
}

BOOLEAN FspFileNodeTryAcquireExclusiveF(FSP_FILE_NODE *FileNode, ULONG Flags)
{
    PAGED_CODE();

    BOOLEAN Result = FALSE;

    if (Flags & FspFileNodeAcquireMain)
    {
        Result = ExAcquireResourceExclusiveLite(FileNode->Header.Resource, FALSE);
        if (!Result)
            return FALSE;
    }

    if (Flags & FspFileNodeAcquirePgio)
    {
        Result = ExAcquireResourceExclusiveLite(FileNode->Header.PagingIoResource, FALSE);
        if (!Result)
        {
            if (Flags & FspFileNodeAcquireMain)
                ExReleaseResourceLite(FileNode->Header.Resource);
            return FALSE;
        }
    }

    return Result;
}

VOID FspFileNodeSetOwnerF(FSP_FILE_NODE *FileNode, ULONG Flags, PVOID Owner)
{
    PAGED_CODE();

    Owner = (PVOID)((UINT_PTR)Owner | 3);

    if (Flags & FspFileNodeAcquireMain)
        ExSetResourceOwnerPointer(FileNode->Header.Resource, Owner);

    if (Flags & FspFileNodeAcquirePgio)
        ExSetResourceOwnerPointer(FileNode->Header.PagingIoResource, Owner);
}

VOID FspFileNodeReleaseF(FSP_FILE_NODE *FileNode, ULONG Flags)
{
    PAGED_CODE();

    if (Flags & FspFileNodeAcquirePgio)
        ExReleaseResourceLite(FileNode->Header.PagingIoResource);

    if (Flags & FspFileNodeAcquireMain)
        ExReleaseResourceLite(FileNode->Header.Resource);
}

VOID FspFileNodeReleaseOwnerF(FSP_FILE_NODE *FileNode, ULONG Flags, PVOID Owner)
{
    PAGED_CODE();

    Owner = (PVOID)((UINT_PTR)Owner | 3);

    if (Flags & FspFileNodeAcquirePgio)
    {
        if (ExIsResourceAcquiredLite(FileNode->Header.PagingIoResource))
            ExReleaseResourceLite(FileNode->Header.PagingIoResource);
        else
            ExReleaseResourceForThreadLite(FileNode->Header.PagingIoResource, (ERESOURCE_THREAD)Owner);
    }

    if (Flags & FspFileNodeAcquireMain)
    {
        if (ExIsResourceAcquiredLite(FileNode->Header.Resource))
            ExReleaseResourceLite(FileNode->Header.Resource);
        else
            ExReleaseResourceForThreadLite(FileNode->Header.Resource, (ERESOURCE_THREAD)Owner);
    }
}

FSP_FILE_NODE *FspFileNodeOpen(FSP_FILE_NODE *FileNode, PFILE_OBJECT FileObject,
    UINT32 GrantedAccess, UINT32 ShareAccess, NTSTATUS *PResult)
{
    /*
     * Attempt to insert our FileNode into the volume device's generic table.
     * If an FileNode with the same UserContext already exists, then use that
     * FileNode instead.
     */

    PAGED_CODE();

    PDEVICE_OBJECT FsvolDeviceObject = FileNode->FsvolDeviceObject;
    FSP_FILE_NODE *OpenedFileNode;
    BOOLEAN Inserted, DeletePending;
    NTSTATUS Result;

    FspFsvolDeviceLockContextTable(FsvolDeviceObject);

    OpenedFileNode = FspFsvolDeviceInsertContextByName(FsvolDeviceObject,
        &FileNode->FileName, FileNode, &FileNode->ContextByNameElementStorage, &Inserted);
    ASSERT(0 != OpenedFileNode);

    if (Inserted)
    {
        /*
         * The new FileNode was inserted into the Context table. Set its share access
         * and reference and open it. There should be (at least) two references to this
         * FileNode, one from our caller and one from the Context table.
         */
        ASSERT(OpenedFileNode == FileNode);

        IoSetShareAccess(GrantedAccess, ShareAccess, FileObject,
            &OpenedFileNode->ShareAccess);
    }
    else
    {
        /*
         * The new FileNode was NOT inserted into the Context table. Instead we are
         * opening a prior FileNode that we found in the table.
         */
        ASSERT(OpenedFileNode != FileNode);

        DeletePending = 0 != OpenedFileNode->DeletePending;
        MemoryBarrier();
        if (DeletePending)
        {
            Result = STATUS_DELETE_PENDING;
            goto exit;
        }

        /*
         * FastFat says to do the following on Vista and above.
         *
         * Quote:
         *     Do an extra test for writeable user sections if the user did not allow
         *     write sharing - this is neccessary since a section may exist with no handles
         *     open to the file its based against.
         */
        if (!FlagOn(ShareAccess, FILE_SHARE_WRITE) &&
            FlagOn(GrantedAccess,
                FILE_EXECUTE | FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE) &&
            MmDoesFileHaveUserWritableReferences(&OpenedFileNode->NonPaged->SectionObjectPointers))
        {
            Result = STATUS_SHARING_VIOLATION;
            goto exit;
        }

        /* share access check */
        Result = IoCheckShareAccess(GrantedAccess, ShareAccess, FileObject,
            &OpenedFileNode->ShareAccess, TRUE);

    exit:
        if (!NT_SUCCESS(Result))
        {
            if (0 != PResult)
                *PResult = Result;

            OpenedFileNode = 0;
        }
    }

    if (0 != OpenedFileNode)
    {
        FspFileNodeReference(OpenedFileNode);
        OpenedFileNode->OpenCount++;
    }

    FspFsvolDeviceUnlockContextTable(FsvolDeviceObject);

    return OpenedFileNode;
}

VOID FspFileNodeClose(FSP_FILE_NODE *FileNode, PFILE_OBJECT FileObject,
    PBOOLEAN PDeletePending)
{
    /*
     * Close the FileNode. If the OpenCount becomes zero remove it
     * from the Context table.
     */

    PAGED_CODE();

    PDEVICE_OBJECT FsvolDeviceObject = FileNode->FsvolDeviceObject;
    FSP_FILE_DESC *FileDesc = FileObject->FsContext2;
    BOOLEAN Deleted = FALSE, DeletePending;

    FspFsvolDeviceLockContextTable(FsvolDeviceObject);

    if (FileDesc->DeleteOnClose)
        FileNode->DeletePending = TRUE;
    DeletePending = 0 != FileNode->DeletePending;
    MemoryBarrier();

    IoRemoveShareAccess(FileObject, &FileNode->ShareAccess);
    if (0 == --FileNode->OpenCount)
        FspFsvolDeviceDeleteContextByName(FsvolDeviceObject, &FileNode->FileName, &Deleted);

    FspFsvolDeviceUnlockContextTable(FsvolDeviceObject);

    if (Deleted)
        FspFileNodeDereference(FileNode);

    if (0 != PDeletePending)
        *PDeletePending = Deleted && DeletePending;
}

VOID FspFileNodeRename(FSP_FILE_NODE *FileNode, PUNICODE_STRING NewFileName)
{
    PAGED_CODE();

    PDEVICE_OBJECT FsvolDeviceObject = FileNode->FsvolDeviceObject;
    BOOLEAN Deleted, Inserted;

    FspFsvolDeviceLockContextTable(FsvolDeviceObject);

    FspFsvolDeviceDeleteContextByName(FsvolDeviceObject, &FileNode->FileName, &Deleted);
    ASSERT(Deleted);

    if (0 != FileNode->ExternalFileName)
        FspFree(FileNode->ExternalFileName);
    FileNode->FileName = *NewFileName;
    FileNode->ExternalFileName = NewFileName->Buffer;

    FspFsvolDeviceInsertContextByName(FsvolDeviceObject, &FileNode->FileName, FileNode,
        &FileNode->ContextByNameElementStorage, &Inserted);
    ASSERT(Inserted);

    FspFsvolDeviceUnlockContextTable(FsvolDeviceObject);
}

VOID FspFileNodeGetFileInfo(FSP_FILE_NODE *FileNode, FSP_FSCTL_FILE_INFO *FileInfo)
{
    PAGED_CODE();

    FileInfo->AllocationSize = FileNode->Header.AllocationSize.QuadPart;
    FileInfo->FileSize = FileNode->Header.FileSize.QuadPart;

    FileInfo->FileAttributes = FileNode->FileAttributes;
    FileInfo->ReparseTag = FileNode->ReparseTag;
    FileInfo->CreationTime = FileNode->CreationTime;
    FileInfo->LastAccessTime = FileNode->LastAccessTime;
    FileInfo->LastWriteTime = FileNode->LastWriteTime;
    FileInfo->ChangeTime = FileNode->ChangeTime;
}

BOOLEAN FspFileNodeTryGetFileInfo(FSP_FILE_NODE *FileNode, FSP_FSCTL_FILE_INFO *FileInfo)
{
    PAGED_CODE();

    BOOLEAN Result;

    if (0 < FileNode->InfoExpirationTime && KeQueryInterruptTime() < FileNode->InfoExpirationTime)
    {
        FileInfo->AllocationSize = FileNode->Header.AllocationSize.QuadPart;
        FileInfo->FileSize = FileNode->Header.FileSize.QuadPart;

        FileInfo->FileAttributes = FileNode->FileAttributes;
        FileInfo->ReparseTag = FileNode->ReparseTag;
        FileInfo->CreationTime = FileNode->CreationTime;
        FileInfo->LastAccessTime = FileNode->LastAccessTime;
        FileInfo->LastWriteTime = FileNode->LastWriteTime;
        FileInfo->ChangeTime = FileNode->ChangeTime;
        Result = TRUE;
    }
    else
        Result = FALSE;

    return Result;
}

VOID FspFileNodeSetFileInfo(FSP_FILE_NODE *FileNode, PFILE_OBJECT CcFileObject,
    const FSP_FSCTL_FILE_INFO *FileInfo)
{
    PAGED_CODE();

    FSP_FSVOL_DEVICE_EXTENSION *FsvolDeviceExtension =
        FspFsvolDeviceExtension(FileNode->FsvolDeviceObject);
    UINT64 FileInfoTimeout = FsvolDeviceExtension->VolumeParams.FileInfoTimeout * 10000ULL;
    UINT64 AllocationSize = FileInfo->AllocationSize > FileInfo->FileSize ?
        FileInfo->AllocationSize : FileInfo->FileSize;
    UINT64 AllocationUnit;

    AllocationUnit = FsvolDeviceExtension->VolumeParams.SectorSize *
        FsvolDeviceExtension->VolumeParams.SectorsPerAllocationUnit;
    AllocationSize = (AllocationSize + AllocationUnit - 1) / AllocationUnit * AllocationUnit;

    FileNode->Header.AllocationSize.QuadPart = AllocationSize;
    FileNode->Header.FileSize.QuadPart = FileInfo->FileSize;

    FileNode->FileAttributes = FileInfo->FileAttributes;
    FileNode->ReparseTag = FileInfo->ReparseTag;
    FileNode->CreationTime = FileInfo->CreationTime;
    FileNode->LastAccessTime = FileInfo->LastAccessTime;
    FileNode->LastWriteTime = FileInfo->LastWriteTime;
    FileNode->ChangeTime = FileInfo->ChangeTime;
    FileNode->InfoExpirationTime = 0 != FileInfoTimeout ?
        KeQueryInterruptTime() + FileInfoTimeout : 0;
    FileNode->InfoChangeNumber++;

    if (0 != CcFileObject)
        FileNode->CcStatus = FspCcSetFileSizes(
            CcFileObject, (PCC_FILE_SIZES)&FileNode->Header.AllocationSize);
}

BOOLEAN FspFileNodeTrySetFileInfo(FSP_FILE_NODE *FileNode, PFILE_OBJECT CcFileObject,
    const FSP_FSCTL_FILE_INFO *FileInfo, ULONG InfoChangeNumber)
{
    PAGED_CODE();

    if (FileNode->InfoChangeNumber != InfoChangeNumber)
        return FALSE;

    FspFileNodeSetFileInfo(FileNode, CcFileObject, FileInfo);
    return TRUE;
}

NTSTATUS FspFileDescCreate(FSP_FILE_DESC **PFileDesc)
{
    PAGED_CODE();

    *PFileDesc = FspAlloc(sizeof(FSP_FILE_DESC));
    if (0 == *PFileDesc)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(*PFileDesc, sizeof(FSP_FILE_DESC));

    return STATUS_SUCCESS;
}

VOID FspFileDescDelete(FSP_FILE_DESC *FileDesc)
{
    PAGED_CODE();

    FspFree(FileDesc);
}
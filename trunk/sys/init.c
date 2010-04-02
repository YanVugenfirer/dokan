/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2008 Hiroki Asakawa info@dokan-dev.net

  http://dokan-dev.net/en

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 3 of the License, or (at your option) any
later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program. If not, see <http://www.gnu.org/licenses/>.
*/


#include "dokan.h"
#include <initguid.h>
#include <wdmsec.h>
#include <mountmgr.h>
#include <ntddstor.h>

NTSTATUS
DokanFilterCallbackAcquireForCreateSection(
	__in PFS_FILTER_CALLBACK_DATA CallbackData,
    __out PVOID *CompletionContext
	)
{
	PFSRTL_ADVANCED_FCB_HEADER	header;
	DDbgPrint("DokanFilterCallbackAcquireForCreateSection\n");

	header = CallbackData->FileObject->FsContext;

	if (header && header->Resource) {
		ExAcquireResourceExclusiveLite(header->Resource, TRUE);
	}

	if (CallbackData->Parameters.AcquireForSectionSynchronization.SyncType
		!= SyncTypeCreateSection) {
		return STATUS_FSFILTER_OP_COMPLETED_SUCCESSFULLY;
	} else {
		return STATUS_FILE_LOCKED_WITH_WRITERS;
	}
}


NTSTATUS
DokanSendIoContlToMountManager(
	__in PVOID	InputBuffer,
	__in ULONG	Length
	)
{
	NTSTATUS		status;
	UNICODE_STRING	mountManagerName;
	PFILE_OBJECT    mountFileObject;
	PDEVICE_OBJECT  mountDeviceObject;
	PIRP			irp;
	KEVENT			driverEvent;
	IO_STATUS_BLOCK	iosb;

	DDbgPrint("=> DokanSnedIoContlToMountManager\n");

	RtlInitUnicodeString(&mountManagerName, MOUNTMGR_DEVICE_NAME);


	status = IoGetDeviceObjectPointer(
				&mountManagerName,
				FILE_READ_ATTRIBUTES,
				&mountFileObject,
				&mountDeviceObject);

	if (!NT_SUCCESS(status)) {
		DDbgPrint("  IoGetDeviceObjectPointer failed: 0x%x\n", status);
		return status;
	}

	KeInitializeEvent(&driverEvent, NotificationEvent, FALSE);

	irp = IoBuildDeviceIoControlRequest(
			IOCTL_MOUNTMGR_VOLUME_ARRIVAL_NOTIFICATION,
			mountDeviceObject,
			InputBuffer,
			Length,
			NULL,
			0,
			FALSE,
			&driverEvent,
			&iosb);

	if (irp == NULL) {
		DDbgPrint("  IoBuildDeviceIoControlRequest failed\n");
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	status = IoCallDriver(mountDeviceObject, irp);

	if (status == STATUS_PENDING) {
		KeWaitForSingleObject(
			&driverEvent, Executive, KernelMode, FALSE, NULL);
	}
	status = iosb.Status;

	ObDereferenceObject(mountFileObject);
	ObDereferenceObject(mountDeviceObject);

	if (NT_SUCCESS(status)) {
		DDbgPrint("  IoCallDriver success\n");
	} else {
		DDbgPrint("  IoCallDriver faield: 0x%x\n", status);
	}

	DDbgPrint("<= DokanSendIoContlToMountManager\n");

	return status;
}

NTSTATUS
DokanSendVolumeArrivalNotification(
	PUNICODE_STRING		DeviceName
	)
{
	NTSTATUS		status;
	PMOUNTMGR_TARGET_NAME targetName;
	ULONG			length;

	DDbgPrint("=> DokanSendVolumeArrivalNotification\n");

	length = sizeof(MOUNTMGR_TARGET_NAME) + DeviceName->Length - 1;
	targetName = ExAllocatePool(length);

	if (targetName == NULL) {
		DDbgPrint("  can't allocate MOUNTMGR_TARGET_NAME\n");
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	RtlZeroMemory(targetName, length);

	targetName->DeviceNameLength = DeviceName->Length;
	RtlCopyMemory(targetName->DeviceName, DeviceName->Buffer, DeviceName->Length);
	
	status = DokanSendIoContlToMountManager(targetName, length);

	if (NT_SUCCESS(status)) {
		DDbgPrint("  IoCallDriver success\n");
	} else {
		DDbgPrint("  IoCallDriver faield: 0x%x\n", status);
	}

	ExFreePool(targetName);

	DDbgPrint("<= DokanSendVolumeArrivalNotification\n");

	return status;
}


NTSTATUS
DokanRegisterMountedDeviceInterface(
	__in PDEVICE_OBJECT	DeviceObject,
	__in PDokanDCB		Dcb
	)
{
	NTSTATUS		status;
	UNICODE_STRING	interfaceName;
	DDbgPrint("=> DokanRegisterMountedDeviceInterface\n");

	status = IoRegisterDeviceInterface(
                DeviceObject,
                &MOUNTDEV_MOUNTED_DEVICE_GUID,
                NULL,
                &interfaceName
                );

    if(NT_SUCCESS(status)) {
		DDbgPrint("  InterfaceName:%wZ\n", &interfaceName);

        Dcb->MountedDeviceInterfaceName = interfaceName;
        status = IoSetDeviceInterfaceState(&interfaceName, TRUE);

        if(!NT_SUCCESS(status)) {
			DDbgPrint("  IoSetDeviceInterfaceState failed: 0x%x\n", status);
            RtlFreeUnicodeString(&interfaceName);
        }
	} else {
		DDbgPrint("  IoRegisterDeviceInterface failed: 0x%x\n", status);
	}

    if(!NT_SUCCESS(status)) {
        RtlInitUnicodeString(&(Dcb->MountedDeviceInterfaceName),
                             NULL);
    }
	DDbgPrint("<= DokanRegisterMountedDeviceInterface\n");
	return status;
}


NTSTATUS
DokanRegisterDeviceInterface(
	__in PDRIVER_OBJECT		DriverObject,
	__in PDEVICE_OBJECT		DeviceObject,
	__in PDokanDCB			Dcb
	)
{
	PDEVICE_OBJECT	pnpDeviceObject = NULL;
	NTSTATUS		status;

	status = IoReportDetectedDevice(
				DriverObject,
				InterfaceTypeUndefined,
				0,
				0,
				NULL,
				NULL,
				FALSE,
				&pnpDeviceObject);

	if (NT_SUCCESS(status)) {
		DDbgPrint("  IoReportDetectedDevice success\n");
	} else {
		DDbgPrint("  IoReportDetectedDevice failed: 0x%x\n", status);
		return status;
	}

	if (IoAttachDeviceToDeviceStack(pnpDeviceObject, DeviceObject) != NULL) {
		DDbgPrint("  IoAttachDeviceToDeviceStack success\n");
	} else {
		DDbgPrint("  IoAttachDeviceToDeviceStack failed\n");
	}

	status = IoRegisterDeviceInterface(
				pnpDeviceObject,
				&GUID_DEVINTERFACE_DISK,
				NULL,
				&Dcb->DiskDeviceInterfaceName);

	if (NT_SUCCESS(status)) {
		DDbgPrint("  IoRegisterDeviceInterface success: %wZ\n", &Dcb->DiskDeviceInterfaceName);
	} else {
		DDbgPrint("  IoRegisterDeviceInterface failed: 0x%x\n", status);
		return status;
	}

	status = IoSetDeviceInterfaceState(&Dcb->DiskDeviceInterfaceName, TRUE);

	if (NT_SUCCESS(status)) {
		DDbgPrint("  IoSetDeviceInterfaceState success\n");
	} else {
		DDbgPrint("  IoSetDeviceInterfaceState failed: 0x%x\n", status);
		return status;
	}

	status = IoRegisterDeviceInterface(
				pnpDeviceObject,
				&MOUNTDEV_MOUNTED_DEVICE_GUID,
				NULL,
				&Dcb->MountedDeviceInterfaceName);

	if (NT_SUCCESS(status)) {
		DDbgPrint("  IoRegisterDeviceInterface success: %wZ\n", &Dcb->MountedDeviceInterfaceName);
	} else {
		DDbgPrint("  IoRegisterDeviceInterface failed: 0x%x\n", status);
		return status;
	}

	status = IoSetDeviceInterfaceState(&Dcb->MountedDeviceInterfaceName, TRUE);

	if (NT_SUCCESS(status)) {
		DDbgPrint("  IoSetDeviceInterfaceState success\n");
	} else {
		DDbgPrint("  IoSetDeviceInterfaceState failed: 0x%x\n", status);
		return status;
	}

	return status;
}


VOID
DokanInitIrpList(
	 __in PIRP_LIST		IrpList
	 )
{
	InitializeListHead(&IrpList->ListHead);
	KeInitializeSpinLock(&IrpList->ListLock);
	KeInitializeEvent(&IrpList->NotEmpty, NotificationEvent, FALSE);
}


NTSTATUS
DokanCreateGlobalDiskDevice(
	__in PDRIVER_OBJECT DriverObject
	)
{
	WCHAR	deviceNameBuf[] = DOKAN_GLOBAL_DEVICE_NAME; 
	WCHAR	symbolicLinkBuf[] = DOKAN_GLOBAL_SYMBOLIC_LINK_NAME;
	NTSTATUS		status;
	UNICODE_STRING	deviceName;
	UNICODE_STRING	symbolicLinkName;
	PDEVICE_OBJECT	deviceObject;
	PDOKAN_GLOBAL	dokanGlobal;

	RtlInitUnicodeString(&deviceName, deviceNameBuf);
	RtlInitUnicodeString(&symbolicLinkName, symbolicLinkBuf);

	status = IoCreateDeviceSecure(
				DriverObject,		// DriverObject
				sizeof(DOKAN_GLOBAL),// DeviceExtensionSize
				&deviceName,		// DeviceName
				FILE_DEVICE_UNKNOWN,// DeviceType
				0,					// DeviceCharacteristics
				FALSE,				// Not Exclusive
				&SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R, // Default SDDL String
				NULL,				// Device Class GUID
				&deviceObject);		// DeviceObject

	if (!NT_SUCCESS(status)) {
		DDbgPrint("  IoCreateDevice returned 0x%x\n", status);
		return status;
	}
	DDbgPrint("DokanGlobalDevice: %wZ created\n", &deviceName);
	ObReferenceObject(deviceObject);

	status = IoCreateSymbolicLink(&symbolicLinkName, &deviceName);
	if (!NT_SUCCESS(status)) {
		DDbgPrint("  IoCreateSymbolicLink returned 0x%x\n", status);
		IoDeleteDevice(deviceObject);
		return status;
	}
	DDbgPrint("SymbolicLink: %wZ -> %wZ created\n", &deviceName, &symbolicLinkName);
	dokanGlobal = deviceObject->DeviceExtension;

	RtlZeroMemory(dokanGlobal, sizeof(DOKAN_GLOBAL));
	DokanInitIrpList(&dokanGlobal->PendingService);
	DokanInitIrpList(&dokanGlobal->NotifyService);

	dokanGlobal->Identifier.Type = DGL;
	dokanGlobal->Identifier.Size = sizeof(DOKAN_GLOBAL);

	return STATUS_SUCCESS;
}


PUNICODE_STRING
AllocateUnicodeString(
	__in PCWSTR String)
{
	PUNICODE_STRING	unicode;
	PWSTR 	buffer;
	ULONG	length;

	unicode = ExAllocatePool(sizeof(UNICODE_STRING));
	if (unicode == NULL) {
		return NULL;
	}

	length = (wcslen(String) + 1) * sizeof(WCHAR);
	buffer = ExAllocatePool(length);
	if (buffer == NULL) {
		ExFreePool(unicode);
		return NULL;
	}
	RtlCopyMemory(buffer, String, length);
	RtlInitUnicodeString(unicode, buffer);
	return unicode;
}

VOID
FreeUnicodeString(
	PUNICODE_STRING	UnicodeString)
{
	if (UnicodeString != NULL) {
		ExFreePool(UnicodeString->Buffer);
		ExFreePool(UnicodeString);
	}
}


NTSTATUS
DokanCreateDiskDevice(
	__in PDRIVER_OBJECT DriverObject,
	__in ULONG			MountId,
	__in PWCHAR			BaseGuid,
	__in PDOKAN_GLOBAL	DokanGlobal,
	__in DEVICE_TYPE	DeviceType,
	__in ULONG			DeviceCharacteristics,
	__out PDokanDCB*	Dcb
	)
{
	WCHAR				diskDeviceNameBuf[MAXIMUM_FILENAME_LENGTH];
	WCHAR				fsDeviceNameBuf[MAXIMUM_FILENAME_LENGTH];
	WCHAR				symbolicLinkNameBuf[MAXIMUM_FILENAME_LENGTH];
	PDEVICE_OBJECT		diskDeviceObject;
	PDEVICE_OBJECT		fsDeviceObject;
	PDokanDCB			dcb;
	PDokanVCB			vcb;
	UNICODE_STRING		diskDeviceName;
	NTSTATUS			status;
	PUNICODE_STRING		symbolicLinkTarget;

	FS_FILTER_CALLBACKS filterCallbacks;

	// make DeviceName and SymboliLink
	swprintf(diskDeviceNameBuf, DOKAN_DISK_DEVICE_NAME L"%s", BaseGuid);
	swprintf(fsDeviceNameBuf, DOKAN_FS_DEVICE_NAME L"%s", BaseGuid);
	swprintf(symbolicLinkNameBuf, DOKAN_SYMBOLIC_LINK_NAME L"%s", BaseGuid);

	RtlInitUnicodeString(&diskDeviceName, diskDeviceNameBuf);

	//
	// make a DeviceObject for Disk Device
	//
	if (DeviceType != FILE_DEVICE_NETWORK_FILE_SYSTEM) {
		status = IoCreateDeviceSecure(
					DriverObject,		// DriverObject
					sizeof(DokanDCB),	// DeviceExtensionSize
					&diskDeviceName,	// DeviceName
					FILE_DEVICE_DISK,	// DeviceType
					DeviceCharacteristics,	// DeviceCharacteristics
					FALSE,				// Not Exclusive
					&SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R, // Default SDDL String
					NULL,				// Device Class GUID
					&diskDeviceObject); // DeviceObject
	} else {
		status = IoCreateDevice(
					DriverObject,			// DriverObject
					sizeof(DokanDCB),		// DeviceExtensionSize
					NULL,					// DeviceName
					FILE_DEVICE_UNKNOWN,	// DeviceType
					DeviceCharacteristics,	// DeviceCharacteristics
					FALSE,					// Not Exclusive
					&diskDeviceObject);		// DeviceObject
	}


	if (!NT_SUCCESS(status)) {
		DDbgPrint("  IoCreateDevice (DISK_DEVICE) failed: 0x%x\n", status);
		return status;
	}
	DDbgPrint("DokanDiksDevice: %wZ created\n", &diskDeviceName);

	//
	// Initialize the device extension.
	//
	dcb = diskDeviceObject->DeviceExtension;
	*Dcb = dcb;
	dcb->DeviceObject = diskDeviceObject;
	dcb->Global = DokanGlobal;

	dcb->Identifier.Type = DCB;
	dcb->Identifier.Size = sizeof(DokanDCB);

	dcb->MountId = MountId;
	dcb->DeviceType = FILE_DEVICE_DISK;
	dcb->DeviceCharacteristics = DeviceCharacteristics;
	KeInitializeEvent(&dcb->KillEvent, NotificationEvent, FALSE);

	//
	// Establish user-buffer access method.
	//
	diskDeviceObject->Flags |= DO_DIRECT_IO;

	// initialize Event and Event queue
	DokanInitIrpList(&dcb->PendingIrp);
	DokanInitIrpList(&dcb->PendingEvent);
	DokanInitIrpList(&dcb->NotifyEvent);

	KeInitializeEvent(&dcb->ReleaseEvent, NotificationEvent, FALSE);

	// "0" means not mounted
	dcb->Mounted = 0;

	ExInitializeResourceLite(&dcb->Resource);

	dcb->CacheManagerNoOpCallbacks.AcquireForLazyWrite  = &DokanNoOpAcquire;
	dcb->CacheManagerNoOpCallbacks.ReleaseFromLazyWrite = &DokanNoOpRelease;
	dcb->CacheManagerNoOpCallbacks.AcquireForReadAhead  = &DokanNoOpAcquire;
	dcb->CacheManagerNoOpCallbacks.ReleaseFromReadAhead = &DokanNoOpRelease;

	dcb->SymbolicLinkName = AllocateUnicodeString(symbolicLinkNameBuf);
	if (DeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM) {
		// use same name since there is no disk device for network file system.
		dcb->DiskDeviceName = AllocateUnicodeString(diskDeviceNameBuf);
		dcb->FileSystemDeviceName = AllocateUnicodeString(diskDeviceNameBuf);
	} else {
		dcb->DiskDeviceName =  AllocateUnicodeString(diskDeviceNameBuf);
		dcb->FileSystemDeviceName = AllocateUnicodeString(fsDeviceNameBuf);
	}

	status = IoCreateDeviceSecure(
				DriverObject,		// DriverObject
				sizeof(DokanVCB),	// DeviceExtensionSize
				dcb->FileSystemDeviceName, // DeviceName
				DeviceType,			// DeviceType
				DeviceCharacteristics,	// DeviceCharacteristics
				FALSE,				// Not Exclusive
				&SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R, // Default SDDL String
				NULL,				// Device Class GUID
				&fsDeviceObject);	// DeviceObject

	if (!NT_SUCCESS(status)) {
		DDbgPrint("  IoCreateDevice (FILE_SYSTEM_DEVICE) failed: 0x%x\n", status);
		IoDeleteDevice(diskDeviceObject);
		return status;
	}
	DDbgPrint("DokanFileSystemDevice: %wZ created\n", dcb->FileSystemDeviceName);

	vcb = fsDeviceObject->DeviceExtension;

	vcb->Identifier.Type = VCB;
	vcb->Identifier.Size = sizeof(DokanVCB);

	vcb->DeviceObject = fsDeviceObject;
	vcb->Dcb = dcb;

	dcb->Vcb = vcb;
	
	InitializeListHead(&vcb->NextFCB);

	InitializeListHead(&vcb->DirNotifyList);
	FsRtlNotifyInitializeSync(&vcb->NotifySync);

	ExInitializeFastMutex(&vcb->AdvancedFCBHeaderMutex);
#if _WIN32_WINNT >= 0x0501
	FsRtlSetupAdvancedHeader(&vcb->VolumeFileHeader, &vcb->AdvancedFCBHeaderMutex);
#else
	if (DokanFsRtlTeardownPerStreamContexts) {
		FsRtlSetupAdvancedHeader(&vcb->VolumeFileHeader, &vcb->AdvancedFCBHeaderMutex);
	}
#endif

    RtlZeroMemory(&filterCallbacks, sizeof(FS_FILTER_CALLBACKS));

	// only be used by filter driver?
	filterCallbacks.SizeOfFsFilterCallbacks = sizeof(FS_FILTER_CALLBACKS);
	filterCallbacks.PreAcquireForSectionSynchronization = DokanFilterCallbackAcquireForCreateSection;

	FsRtlRegisterFileSystemFilterCallbacks(DriverObject, &filterCallbacks);

	//
	// Establish user-buffer access method.
	//
	fsDeviceObject->Flags |= DO_DIRECT_IO;

	/*
	if (diskDeviceObject->Vpb) {
		diskDeviceObject->Vpb->DeviceObject = fsDeviceObject;
		diskDeviceObject->Vpb->RealDevice = fsDeviceObject;
		diskDeviceObject->Vpb->Flags = VPB_MOUNTED;
		diskDeviceObject->Vpb->VolumeLabelLength = wcslen(VOLUME_LABEL) * sizeof(WCHAR);
		swprintf(diskDeviceObject->Vpb->VolumeLabel, VOLUME_LABEL);
		diskDeviceObject->Vpb->SerialNumber = 0x19831116;
	}
	*/

	ObReferenceObject(fsDeviceObject);
	ObReferenceObject(diskDeviceObject);

	//
	// Create a symbolic link for userapp to interact with the driver.
	//
	status = IoCreateSymbolicLink(dcb->SymbolicLinkName, dcb->DiskDeviceName);

	if (!NT_SUCCESS(status)) {
		IoDeleteDevice(diskDeviceObject);
		IoDeleteDevice(fsDeviceObject);
		DDbgPrint("  IoCreateSymbolicLink returned 0x%x\n", status);
		return status;
	}
	DDbgPrint("SymbolicLink: %wZ -> %w Zcreated\n", dcb->SymbolicLinkName, dcb->DiskDeviceName);

	// Mark devices as initialized
	diskDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
	fsDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

	IoRegisterFileSystem(fsDeviceObject);

	if (DeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM) {
		status = FsRtlRegisterUncProvider(&(dcb->MupHandle), dcb->FileSystemDeviceName, FALSE);
		if (NT_SUCCESS(status)) {
			DDbgPrint("  FsRtlRegisterUncProvider success\n");
		} else {
			DDbgPrint("  FsRtlRegisterUncProvider failed: 0x%x\n", status);
			dcb->MupHandle = 0;
		}
	}
	//DokanRegisterMountedDeviceInterface(diskDeviceObject, dcb);
	
	dcb->Mounted = 1;

	//DokanSendVolumeArrivalNotification(&deviceName);
	//DokanRegisterDeviceInterface(DriverObject, diskDeviceObject, dcb);

	return STATUS_SUCCESS;
}


VOID
DokanDeleteDeviceObject(
	__in PDokanDCB Dcb)
{
	UNICODE_STRING		symbolicLinkName;
	WCHAR				symbolicLinkBuf[MAXIMUM_FILENAME_LENGTH];
	PDokanVCB			vcb;

	ASSERT(GetIdentifierType(Dcb) == DCB);
	vcb = Dcb->Vcb;

	if (Dcb->MupHandle) {
		FsRtlDeregisterUncProvider(Dcb->MupHandle);
	}

	DDbgPrint("  Delete Symbolic Name: %wZ\n", Dcb->SymbolicLinkName);
	IoDeleteSymbolicLink(Dcb->SymbolicLinkName);

	FreeUnicodeString(Dcb->SymbolicLinkName);
	FreeUnicodeString(Dcb->DiskDeviceName);
	FreeUnicodeString(Dcb->FileSystemDeviceName);
	
	Dcb->SymbolicLinkName = NULL;
	Dcb->DiskDeviceName = NULL;
	Dcb->FileSystemDeviceName = NULL;

	IoUnregisterFileSystem(vcb->DeviceObject);

	if (Dcb->DeviceObject->Vpb) {
		Dcb->DeviceObject->Vpb->DeviceObject = NULL;
		Dcb->DeviceObject->Vpb->RealDevice = NULL;
		Dcb->DeviceObject->Vpb->Flags = 0;
	}

	// delete diskDeviceObject
	DDbgPrint("  Delete DeviceObject\n");
	IoDeleteDevice(vcb->DeviceObject);

	// delete DeviceObject
	DDbgPrint("  Delete Disk DeviceObject\n");
	IoDeleteDevice(Dcb->DeviceObject);
}


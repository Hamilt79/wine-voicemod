/*
 * Wine Voicemod control-device bridge
 *
 * Copyright 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/wdm.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(voicemod);

static UNICODE_STRING legacy_link = RTL_CONSTANT_STRING(L"\\??\\VMDriver");
static UNICODE_STRING modern_link = RTL_CONSTANT_STRING(L"\\??\\voicemodvad");

static NTSTATUS complete_irp(IRP *irp, NTSTATUS status, ULONG_PTR information)
{
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS dispatch_open_close(DEVICE_OBJECT *device, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);

    TRACE("device %p major %#x file %p\n", device, stack->MajorFunction, stack->FileObject);
    return complete_irp(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS dispatch_ioctl(DEVICE_OBJECT *device, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG input_len = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG output_len = stack->Parameters.DeviceIoControl.OutputBufferLength;
    BYTE *buffer = irp->AssociatedIrp.SystemBuffer;
    ULONG dump_len = min(input_len, 64);

    TRACE("device %p ioctl %#lx type %#lx function %#lx method %#lx access %#lx in %lu out %lu\n",
            device, code, code >> 16, (code >> 2) & 0xfff,
            code & 3, (code >> 14) & 3, input_len, output_len);
    if (buffer && dump_len) TRACE("input %s\n", debugstr_an((const char *)buffer, dump_len));

    /*
     * Voicemod issues two requests.  Function 0x801 registers an event handle
     * for driver notifications, and function 0x807 reads or resets the
     * driver's buffer underrun statistics.  The audio itself travels through
     * the host sound server, so there is nothing to notify and no underruns
     * to report: succeed with zero-filled output.
     */
    if (buffer && output_len) memset(buffer, 0, output_len);
    return complete_irp(irp, STATUS_SUCCESS, output_len);
}

static NTSTATUS create_control_device(DRIVER_OBJECT *driver, const WCHAR *device_name,
        UNICODE_STRING *link)
{
    UNICODE_STRING name;
    DEVICE_OBJECT *device;
    NTSTATUS status;

    RtlInitUnicodeString(&name, device_name);
    if ((status = IoCreateDevice(driver, 0, &name, FILE_DEVICE_UNKNOWN,
            FILE_DEVICE_SECURE_OPEN, FALSE, &device)))
    {
        ERR("failed to create %s, status %#lx\n", debugstr_w(device_name), status);
        return status;
    }

    if ((status = IoCreateSymbolicLink(link, &name)))
    {
        ERR("failed to link %s to %s, status %#lx\n", debugstr_w(link->Buffer),
                debugstr_w(device_name), status);
        IoDeleteDevice(device);
        return status;
    }

    device->Flags &= ~DO_DEVICE_INITIALIZING;
    TRACE("created %s as %s\n", debugstr_w(device_name), debugstr_w(link->Buffer));
    return STATUS_SUCCESS;
}

static void WINAPI driver_unload(DRIVER_OBJECT *driver)
{
    DEVICE_OBJECT *device = driver->DeviceObject;

    IoDeleteSymbolicLink(&legacy_link);
    IoDeleteSymbolicLink(&modern_link);
    while (device)
    {
        DEVICE_OBJECT *next = device->NextDevice;
        IoDeleteDevice(device);
        device = next;
    }
}

static NTSTATUS WINAPI add_device(DRIVER_OBJECT *driver, DEVICE_OBJECT *physical_device)
{
    TRACE("driver %p physical device %p\n", driver, physical_device);
    /* The host audio nodes are enumerated by winepulse; no PortCls FDO is
     * needed for the ROOT\\MEDIA node installed by Voicemod's INF. */
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI DriverEntry(DRIVER_OBJECT *driver, UNICODE_STRING *path)
{
    NTSTATUS status;

    TRACE("driver %p path %s\n", driver, debugstr_w(path->Buffer));

    driver->MajorFunction[IRP_MJ_CREATE] = dispatch_open_close;
    driver->MajorFunction[IRP_MJ_CLOSE] = dispatch_open_close;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = dispatch_ioctl;
    driver->DriverUnload = driver_unload;
    driver->DriverExtension->AddDevice = add_device;

    if ((status = create_control_device(driver, L"\\Device\\VMDriver", &legacy_link)))
        return status;
    if ((status = create_control_device(driver, L"\\Device\\voicemodvad", &modern_link)))
    {
        driver_unload(driver);
        return status;
    }

    return STATUS_SUCCESS;
}

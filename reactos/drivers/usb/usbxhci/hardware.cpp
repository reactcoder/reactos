/*
 * PROJECT:     ReactOS Universal Serial Bus Bulk Extensible Host Controller Interface
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        drivers/usb/usbxhci/hardware.cpp
 * PURPOSE:     USB XHCI device driver(based on Haiku XHCI driver and ReactOS EHCI)
 * PROGRAMMERS: reactcoder@gmail.com
 */
#include "usbxhci.h"

#define YDEBUG
#include <debug.h>

//
// implementation of the interface
//
class CUSBHardwareDevice : public IXHCIHardwareDevice
{
public:
    STDMETHODIMP QueryInterface(REFIID InterfaceId, PVOID *Interface);
    STDMETHODIMP_(ULONG) AddRef()
    {
        InterlockedIncrement(&m_Ref);
        return m_Ref;
    }
    STDMETHODIMP_(ULONG) Release()
    {
        InterlockedDecrement(&m_Ref);
        if (!m_Ref)
        {
            delete this;
            return 0;
        }
        return m_Ref;
    }

    // com interfaces
    IMP_IUSBHARDWAREDEVICE
    IMP_IUSBXHCIHARDWARE

    CUSBHardwareDevice(IUnknown *OuterUnknown) {}
    virtual ~CUSBHardwareDevice() {}
protected:
    LONG m_Ref;
};

//
// UNKNOWN
//
NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::QueryInterface(
    IN REFIID refiid,
    OUT PVOID *OutInterface)
{
    if (IsEqualGUIDAligned(refiid, IID_IUnknown))
    {
        *OutInterface = PVOID(PUNKNOWN(this));
        PUNKNOWN(*OutInterface)->AddRef();
        return STATUS_SUCCESS;
    }
    // bad IID
    return STATUS_UNSUCCESSFUL;
}

//
// IMP_IUSBHARDWAREDEVICE
//
NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::Initialize(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT FunctionalDeviceObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject, 
    IN PDEVICE_OBJECT LowerDeviceObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(FunctionalDeviceObject);
    UNREFERENCED_PARAMETER(PhysicalDeviceObject);
    UNREFERENCED_PARAMETER(LowerDeviceObject);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::PnpStart(
    IN PCM_RESOURCE_LIST RawResources,
    IN PCM_RESOURCE_LIST TranslatedResources)
{
    UNREFERENCED_PARAMETER(RawResources);
    UNREFERENCED_PARAMETER(TranslatedResources);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::PnpStop(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::GetDeviceDetails(
    OUT OPTIONAL PUSHORT VendorId,
    OUT OPTIONAL PUSHORT DeviceId,
    OUT OPTIONAL PULONG NumberOfPorts,
    OUT OPTIONAL PULONG Speed)
{
    UNREFERENCED_PARAMETER(VendorId);
    UNREFERENCED_PARAMETER(DeviceId);
    UNREFERENCED_PARAMETER(NumberOfPorts);
    UNREFERENCED_PARAMETER(Speed);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::GetUSBQueue(
    OUT struct IUSBQueue **OutUsbQueue)
{
    UNREFERENCED_PARAMETER(OutUsbQueue);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::GetDMA(
    OUT struct IDMAMemoryManager **OutDMAMemoryManager)
{
    UNREFERENCED_PARAMETER(OutDMAMemoryManager);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::ResetPort(
    IN ULONG PortIndex)
{
    UNREFERENCED_PARAMETER(PortIndex);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::GetPortStatus(
    IN ULONG PortId,
    OUT USHORT *PortStatus, 
    OUT USHORT *PortChange)
{
    UNREFERENCED_PARAMETER(PortId);
    UNREFERENCED_PARAMETER(PortStatus);
    UNREFERENCED_PARAMETER(PortChange);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::ClearPortStatus(
    IN ULONG PortId,
    IN ULONG Status)
{
    UNREFERENCED_PARAMETER(PortId);
    UNREFERENCED_PARAMETER(Status);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::SetPortFeature(
    IN ULONG PortId,
    IN ULONG Feature)
{
    UNREFERENCED_PARAMETER(PortId);
    UNREFERENCED_PARAMETER(Feature);
    
    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

VOID
STDMETHODCALLTYPE
CUSBHardwareDevice::SetStatusChangeEndpointCallBack(
    IN PVOID CallBack,
    IN PVOID Context)
{
    UNREFERENCED_PARAMETER(CallBack);
    UNREFERENCED_PARAMETER(Context);

    UNIMPLEMENTED_DBGBREAK();
    return;
}

LPCSTR
STDMETHODCALLTYPE
CUSBHardwareDevice::GetUSBType(VOID)
{
    return "USBXHCI";
}

//
// IMP_IUSBXHCIHARDWARE
//

NTSTATUS
NTAPI
CreateUSBHardware(PUSBHARDWAREDEVICE *OutHardware)
{
    PUSBHARDWAREDEVICE This;
    
    This = new(NonPagedPool, TAG_USBXHCI) CUSBHardwareDevice(0);
    if (!This)
        return STATUS_INSUFFICIENT_RESOURCES;

    This->AddRef();
    *OutHardware = (PUSBHARDWAREDEVICE)This;

    return STATUS_SUCCESS;
}

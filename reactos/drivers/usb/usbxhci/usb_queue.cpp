/*
 * PROJECT:     ReactOS Universal Serial Bus Bulk Extensible Host Controller Interface
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        drivers/usb/usbxhci/usb_queue.cpp
 * PURPOSE:     USB XHCI device driver(based on Haiku XHCI driver and ReactOS EHCI)
 * PROGRAMMERS: reactcoder@gmail.com
 */
#include "usbxhci.h"

#define YDEBUG
#include <debug.h>

class CUSBQueue : public IXHCIQueue
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
    IMP_IUSBQUEUE
    IMP_IXHCIQUEUE
    
    CUSBQueue(IUnknown *OuterUnknown) {}
    virtual ~CUSBQueue() {}
protected:
    LONG m_Ref;
};

//
// UNKNOWN
//
NTSTATUS
STDMETHODCALLTYPE
CUSBQueue::QueryInterface(
    IN REFIID refiid,
    OUT PVOID *Output)
{
    if (IsEqualGUIDAligned(refiid, IID_IUnknown))
    {
        *Output = PVOID(PUNKNOWN(this));
        PUNKNOWN(*Output)->AddRef();
        return STATUS_SUCCESS;
    }
    return STATUS_UNSUCCESSFUL;
}

//
// IMP_IUSBQUEUE
//
NTSTATUS
STDMETHODCALLTYPE
CUSBQueue::Initialize(
    IN PUSBHARDWAREDEVICE Hardware,
    IN PDMA_ADAPTER AdapterObject,
    IN PDMAMEMORYMANAGER MemManager,
    IN OPTIONAL PKSPIN_LOCK Lock)
{
    UNREFERENCED_PARAMETER(Hardware);
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(MemManager);
    UNREFERENCED_PARAMETER(Lock);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBQueue::AddUSBRequest(
    IN IUSBRequest *Request)
{
    UNREFERENCED_PARAMETER(Request);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBQueue::CreateUSBRequest(
    IN IUSBRequest **OutRequest)
{
    UNREFERENCED_PARAMETER(OutRequest);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBQueue::AbortDevicePipe(
    IN UCHAR DeviceAddress,
    IN PUSB_ENDPOINT_DESCRIPTOR EndpointDescriptor)
{
    UNREFERENCED_PARAMETER(DeviceAddress);
    UNREFERENCED_PARAMETER(EndpointDescriptor);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

//
// IMP_IXHCIQUEUE
//

NTSTATUS
NTAPI
CreateUSBQueue(
    PUSBQUEUE *OutUsbQueue)
{
    PUSBQUEUE This;

    This = new(NonPagedPool, TAG_USBXHCI) CUSBQueue(0);
    if (!This)
        return STATUS_INSUFFICIENT_RESOURCES;
    
    This->AddRef();
    *OutUsbQueue = (PUSBQUEUE)This;
    
    return STATUS_SUCCESS;
}

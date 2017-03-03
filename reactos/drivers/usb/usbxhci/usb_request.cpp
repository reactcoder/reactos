/*
 * PROJECT:     ReactOS Universal Serial Bus Bulk Extensible Host Controller Interface
 * LICENSE:     GPL - See COPYING in the top level directory
 * PURPOSE:     USB XHCI device driver(based on Haiku XHCI driver and ReactOS EHCI)
 * PROGRAMMERS: reactcoder@gmail.com
 */

#include "usbxhci.h"

#define YDEBUG
#include <debug.h>

class CUSBRequest : public IXHCIRequest
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
    IMP_IUSBREQUEST
    IMP_IXHCIREQUEST
    
    CUSBRequest(IUnknown *OuterUnknown) {}
    virtual ~CUSBRequest() {}
protected:
    LONG m_Ref;
};

//
// UNKNOWN
//
NTSTATUS
STDMETHODCALLTYPE
CUSBRequest::QueryInterface(
    IN REFIID reffid,
    OUT PVOID *Output)
{
    return STATUS_UNSUCCESSFUL;
}

//
// IMP_IUSBREQUEST
//
NTSTATUS
STDMETHODCALLTYPE
CUSBRequest::InitializeWithSetupPacket(
    IN PDMAMEMORYMANAGER DmaManager,
    IN PUSB_DEFAULT_PIPE_SETUP_PACKET SetupPacket,
    IN PUSBDEVICE Device,
    IN OPTIONAL PUSB_ENDPOINT EndpointDescriptor,
    IN OUT ULONG TransferBufferLength,
    IN OUT PMDL TransferBuffer)
{
    UNREFERENCED_PARAMETER(DmaManager);
    UNREFERENCED_PARAMETER(SetupPacket);
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(EndpointDescriptor);
    UNREFERENCED_PARAMETER(TransferBufferLength);
    UNREFERENCED_PARAMETER(TransferBuffer);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBRequest::InitializeWithIrp(
    IN PDMAMEMORYMANAGER DmaManager,
    IN PUSBDEVICE Device,
    IN OUT PIRP Irp)
{
    UNREFERENCED_PARAMETER(DmaManager);
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Irp);

    UNIMPLEMENTED_DBGBREAK();
    return STATUS_UNSUCCESSFUL;
}

BOOLEAN
STDMETHODCALLTYPE
CUSBRequest::IsRequestComplete()
{
    UNIMPLEMENTED_DBGBREAK();
    return FALSE;
}

ULONG
STDMETHODCALLTYPE
CUSBRequest::GetTransferType()
{
    UNIMPLEMENTED_DBGBREAK();
    return 0;
}

VOID
STDMETHODCALLTYPE
CUSBRequest::GetResultStatus(
    OUT OPTIONAL NTSTATUS *NtStatusCode,
    OUT OPTIONAL PULONG UrbStatusCode)
{
    UNREFERENCED_PARAMETER(NtStatusCode);
    UNREFERENCED_PARAMETER(UrbStatusCode);

    UNIMPLEMENTED_DBGBREAK();
    return;
}

//
// IMP_IXHCIREQUEST
//

NTSTATUS
NTAPI
InternalCreateUSBRequest(
    PUSBREQUEST *OutRequest)
{
    PUSBREQUEST This;

    This = new(NonPagedPool, TAG_USBXHCI) CUSBRequest(0);
    if (!This)
        return STATUS_INSUFFICIENT_RESOURCES;

    This->AddRef();
    *OutRequest = (PUSBREQUEST)This;

    return STATUS_SUCCESS;
}

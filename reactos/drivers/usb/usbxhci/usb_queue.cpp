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

    // initialize event and command ringbuffers
    NTSTATUS InitializeRingbuffers(IN PXHCIHARDWAREDEVICE Hardware, IN PDMAMEMORYMANAGER MemoryManager);
    
    CUSBQueue(IUnknown *OuterUnknown) {}
    virtual ~CUSBQueue() {}
protected:
    LONG m_Ref;
    PKSPIN_LOCK m_Lock;
    PXHCIHARDWAREDEVICE m_Hardware;
    PDMAMEMORYMANAGER m_MemoryManager;
    LIST_ENTRY m_CommandRingList;
    PHYSICAL_ADDRESS m_EventRingPhysicalAddress;
    PVOID m_EventRingVirtualAddress;
    ULONG m_EventRingCycleState;
    //PTRB m_EventRingDequeueAddress;
    PHYSICAL_ADDRESS m_CommandRingPhysicalAddress;
    PVOID m_CommandRingVirtualAddress;
    PTRB m_EventRingDequeueAddress;
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
    NTSTATUS Status = STATUS_SUCCESS;

    //
    // store the device lock
    //
    m_Lock = Lock;

    //
    // save for later use
    //
    m_Hardware = PXHCIHARDWAREDEVICE(Hardware);
    m_MemoryManager = PDMAMEMORYMANAGER(MemManager);

    //
    // initialize command list
    //
    InitializeListHead(&m_CommandRingList);

    //
    // create event and command ringbuffers
    //
    Status = InitializeRingbuffers(m_Hardware, MemManager);
    
    //
    // initialization done
    //
    return Status;
}

NTSTATUS
CUSBQueue::InitializeRingbuffers(
    IN PXHCIHARDWAREDEVICE Hardware,
    IN PDMAMEMORYMANAGER MemManager)
{
    NTSTATUS Status;
    PERST_ELEMENT VirtualAddressErstElement = NULL;
    PHYSICAL_ADDRESS PhysicalAddressErstElement;
    PTRB LastTransferRequestBlock;

    //
    // allocate memory for event ringbuffer
    //
    Status = m_MemoryManager->Allocate(XHCI_MAX_EVENTS * sizeof(TRB), &m_EventRingVirtualAddress, &m_EventRingPhysicalAddress);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to allocate memory for event ringbuffer. \n");
        return Status;
    }

    //
    // allocate memory for event ring segment table
    //
    Status = m_MemoryManager->Allocate(sizeof(ERST_ELEMENT), (PVOID*)&VirtualAddressErstElement, &PhysicalAddressErstElement);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to allocate memory for event ring segment table. \n");
        m_MemoryManager->Release(m_EventRingVirtualAddress, (XHCI_MAX_EVENTS * sizeof(TRB)));
        return Status;
    }

    //
    // initialize event ring segment table(only one segment for event ringbuffer)
    //
    VirtualAddressErstElement->Address.QuadPart = m_EventRingPhysicalAddress.QuadPart;
    VirtualAddressErstElement->Size = XHCI_MAX_EVENTS;
    VirtualAddressErstElement->Reserved = 0;

    //
    // get event ring dequeue pointer
    //
    m_EventRingDequeueAddress = (PTRB)m_EventRingVirtualAddress;

    //
    // ring cycle state at init is always one
    //
    m_EventRingCycleState = 1;

    //
    // one segment for event ringbuffer
    //
    Hardware->SetRuntimeRegister(XHCI_ERSTSZ_BASE, 1);

    //
    // set Event Ring Segment Table Base Address Register
    //
    Hardware->SetRuntimeRegister(XHCI_ERSTBA_LOW, PhysicalAddressErstElement.LowPart);
    Hardware->SetRuntimeRegister(XHCI_ERSTBA_HIGH, PhysicalAddressErstElement.HighPart);

    //
    // set Event Ring Dequeue Pointer Register
    //
    Hardware->SetRuntimeRegister(XHCI_ERDP_BASE_LOW, m_EventRingPhysicalAddress.LowPart);

    //
    // allocate memory for command ringbuffer
    //
    Status = m_MemoryManager->Allocate(XHCI_MAX_COMMANDS * sizeof(TRB), &m_CommandRingVirtualAddress, &m_CommandRingPhysicalAddress);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to allocate memory for command ringbuffer. \n");

        //
        // oops
        //
        m_MemoryManager->Release(m_EventRingVirtualAddress, (XHCI_MAX_EVENTS * sizeof(TRB)));
        m_MemoryManager->Release(m_EventRingVirtualAddress, sizeof(ERST_ELEMENT));
        return Status;
    }

    //
    // last trb is a link trb (in order to construct a ringbuffer)
    //
    LastTransferRequestBlock = (PTRB)((ULONG_PTR)m_CommandRingVirtualAddress + (XHCI_MAX_COMMANDS * sizeof(TRB)));
    LastTransferRequestBlock->Field[0] = m_CommandRingPhysicalAddress.LowPart;

    //
    // set Command Ring Control Register. also set up Ring Cycle State 
    //
    Hardware->SetOperationalRegister(XHCI_CRCR_LOW, m_CommandRingPhysicalAddress.LowPart | XHCI_CRCR_RCS);
    Hardware->SetOperationalRegister(XHCI_CRCR_HIGH, m_CommandRingPhysicalAddress.HighPart);

    //
    // great success
    //
    return Status;
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

VOID
STDMETHODCALLTYPE
CUSBQueue::CompleteCommandRequest(PTRB TransferRequestBlock)
{
    DPRINT("CompleteCommandRequest called!\n");
    UNIMPLEMENTED_DBGBREAK();
    return;
}

VOID
STDMETHODCALLTYPE
CUSBQueue::CompleteTransferRequest(PTRB TransferRequestBlock)
{
    DPRINT("CompleteCommandRequest called!\n");
    UNIMPLEMENTED_DBGBREAK();
    return;
}

BOOLEAN
STDMETHODCALLTYPE
CUSBQueue::IsEventRingEmpty(VOID)
{
    DPRINT("IsEventRingEmpty called.\n");

    //
    // check if ring is empty(cycle bit) TODO: use macro to get cycle bit?
    //
    return (m_EventRingCycleState == (m_EventRingDequeueAddress->Field[3] & XHCI_TRB_CYCLE_BIT)) ? FALSE : TRUE;
}

PTRB
STDMETHODCALLTYPE
CUSBQueue::GetEventRingDequeuePointer(VOID)
{
    PHYSICAL_ADDRESS PhysicalAddress;
    PTRB TransferRequestBlock;

    //
    // check if ring is empty(cycle bit)
    //
    if (IsEventRingEmpty())
        return NULL;

    DPRINT("TRB completion code is %lx.\n", XHCI_TRB_COMP_CODE(m_EventRingDequeueAddress->Field[2]));

    //
    // go to next transfer request block
    //
    TransferRequestBlock = m_EventRingDequeueAddress++;

    //
    // end of ringbuffer?
    //
    if (m_EventRingDequeueAddress == (PTRB)((ULONG_PTR)m_EventRingVirtualAddress + (sizeof(TRB) * XHCI_MAX_EVENTS)))
    {
        //
        // every time we start from the beginning, we have to toggle cycle bit
        // 
        m_EventRingCycleState ^= 1;
        m_EventRingDequeueAddress = (PTRB)m_EventRingVirtualAddress;
    }

    //
    // set dequeue pointer. Refer to section 5.5.2.3.3
    //
    PhysicalAddress = MmGetPhysicalAddress(m_EventRingDequeueAddress);

    //
    // setup dequeue pointer(mark event as processed)
    //
    m_Hardware->SetRuntimeRegister(XHCI_ERDP_BASE_LOW, PhysicalAddress.LowPart | XHCI_ERST_EHB);
    
    //
    // found trb
    //
    return TransferRequestBlock;
}

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

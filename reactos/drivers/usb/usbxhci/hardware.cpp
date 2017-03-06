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

typedef VOID NTAPI HD_INIT_CALLBACK(IN PVOID CallBackContext);

BOOLEAN
NTAPI
InterruptServiceRoutine(
    IN PKINTERRUPT Interrupt,
    IN PVOID ServiceContext);

VOID
NTAPI
XhciDeferredRoutine(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2);

VOID
NTAPI
StatusChangeWorkItemRoutine(PVOID Context);

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

    // friend function
    friend BOOLEAN NTAPI InterruptServiceRoutine(IN PKINTERRUPT  Interrupt, IN PVOID  ServiceContext);
    friend VOID NTAPI XhciDeferredRoutine(IN PKDPC Dpc, IN PVOID DeferredContext, IN PVOID SystemArgument1, IN PVOID SystemArgument2);
    friend  VOID NTAPI StatusChangeWorkItemRoutine(PVOID Context);

    // start/stop/reset controller
    NTSTATUS StartController(void);
    NTSTATUS StopController(void);
    NTSTATUS ResetController(void);

    // read register
    ULONG READ_CAPABILITY_REG_ULONG(ULONG Offset);
    ULONG READ_OPERATIONAL_REG_ULONG(ULONG Offset);
    ULONG READ_DOORBELL_REG_ULONG(ULONG Offset);
    ULONG READ_RUNTIME_REG_ULONG(ULONG Offset);

    // write register
    VOID WRITE_CAPABILITY_REG_ULONG(ULONG Offset, ULONG Value);
    VOID WRITE_OPERATIONAL_REG_ULONG(ULONG Offset, ULONG Value);
    VOID WRITE_DOORBELL_REG_ULONG(ULONG Offset, ULONG Value);
    VOID WRITE_RUNTIME_REG_ULONG(ULONG Offset, ULONG Value);

    CUSBHardwareDevice(IUnknown *OuterUnknown) {}
    virtual ~CUSBHardwareDevice() {}
protected:
    LONG m_Ref;
    PDRIVER_OBJECT m_DriverObject;                                                     // driver object
    PDEVICE_OBJECT m_PhysicalDeviceObject;                                             // pdo
    PDEVICE_OBJECT m_FunctionalDeviceObject;                                           // fdo (hcd controller)
    PDEVICE_OBJECT m_NextDeviceObject;                                                 // lower device object
    PDMAMEMORYMANAGER m_MemoryManager;                                                 // memory manager
    PXHCIQUEUE m_UsbQueue;                                                             // usb request queue
    KSPIN_LOCK m_Lock;                                                                 // hardware lock
    WORK_QUEUE_ITEM m_StatusChangeWorkItem;                                            // work item for status change callback
    volatile LONG m_StatusChangeWorkItemStatus;                                        // work item status
    HD_INIT_CALLBACK* m_SCECallBack;                                                   // status change callback routine
    PVOID m_SCEContext;                                                                // status change callback routine context
    USHORT m_VendorID;                                                                 // vendor id
    USHORT m_DeviceID;                                                                 // device id
    BUS_INTERFACE_STANDARD m_BusInterface;                                             // pci bus interface
    KDPC m_IntDpcObject;                                                               // dpc object for deferred isr processing
    PKINTERRUPT m_Interrupt;                                                           // interrupt object
    XHCI_CAPABILITY_REGS m_Capabilities;                                               // controller capabilities
    PULONG m_CapRegBase;                                                               // XHCI capability register
    PULONG m_OpRegBase;                                                                // XHCI operational base registers
    PULONG m_RunTmRegBase;                                                             // XHCI runtime register
    PULONG m_DoorBellRegBase;                                                          // XHCI doorbell register
    PDMA_ADAPTER m_Adapter;                                                            // dma adapter object
    ULONG m_MapRegisters;                                                              // map registers count
    PLARGE_INTEGER m_DeviceContextArray;                                               // device context array virtual address
    PHYSICAL_ADDRESS m_PhysicalDeviceContextArray;                                     // device context array physical address
    PVOID VirtualBase;                                                                 // virtual base for memory manager
    PHYSICAL_ADDRESS PhysicalAddress;                                                  // physical base for memory manager
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
    PCI_COMMON_CONFIG PciConfig;
    NTSTATUS Status;
    ULONG BytesRead;

    DPRINT("CUSBHardwareDevice::Initialize.\n");

    //
    // create DMA Memory Manager
    //
    Status = CreateDMAMemoryManager(&m_MemoryManager);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to create DMA Memory Manager\n");
        return Status;
    }

    //
    // create USBQueue
    //
    Status = CreateUSBQueue((PUSBQUEUE*)&m_UsbQueue);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to create");
        return Status;
    }

    //
    // store device objects
    //
    m_DriverObject = DriverObject;
    m_FunctionalDeviceObject = FunctionalDeviceObject;
    m_PhysicalDeviceObject = PhysicalDeviceObject;
    m_NextDeviceObject = LowerDeviceObject;

    //
    // initialize device lock
    //
    KeInitializeSpinLock(&m_Lock);

    //
    // intialize status change work item
    //
    ExInitializeWorkItem(&m_StatusChangeWorkItem, StatusChangeWorkItemRoutine, PVOID(this));

    m_VendorID = 0;
    m_DeviceID = 0;

    //
    // get bus driver interface
    //
    Status = GetBusInterface(PhysicalDeviceObject, &m_BusInterface);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to get BusInterface");
        return Status;
    }

    //
    // get pci config
    //
    BytesRead = (*m_BusInterface.GetBusData)(m_BusInterface.Context,
                                             PCI_WHICHSPACE_CONFIG,
                                             &PciConfig,
                                             0,
                                             PCI_COMMON_HDR_LENGTH);
    if (BytesRead != PCI_COMMON_HDR_LENGTH)
    {
        DPRINT1("Failed to get pci config information!\n");
        return STATUS_SUCCESS;
    }

    //
    // save vendor and device ID
    //
    m_VendorID = PciConfig.VendorID;
    m_DeviceID = PciConfig.DeviceID;

    //
    // great success
    //
    return Status;
}

// read register
ULONG CUSBHardwareDevice::READ_CAPABILITY_REG_ULONG(ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)((ULONG)m_CapRegBase + Offset));
}

ULONG CUSBHardwareDevice::READ_OPERATIONAL_REG_ULONG(ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)((ULONG)m_OpRegBase + Offset));
}

ULONG CUSBHardwareDevice::READ_DOORBELL_REG_ULONG(ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)((ULONG)m_DoorBellRegBase + Offset));
}

ULONG CUSBHardwareDevice::READ_RUNTIME_REG_ULONG(ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)((ULONG)m_RunTmRegBase + Offset));
}

// write register
VOID
CUSBHardwareDevice::WRITE_CAPABILITY_REG_ULONG(ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)((ULONG)m_CapRegBase + Offset), Value);
}

VOID
CUSBHardwareDevice::WRITE_OPERATIONAL_REG_ULONG(ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)((ULONG)m_OpRegBase + Offset), Value);
}

VOID
CUSBHardwareDevice::WRITE_DOORBELL_REG_ULONG(ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)((ULONG)m_DoorBellRegBase + Offset), Value);
}

VOID
CUSBHardwareDevice::WRITE_RUNTIME_REG_ULONG(ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)((ULONG)m_RunTmRegBase + Offset), Value);
}

NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::PnpStart(
    IN PCM_RESOURCE_LIST RawResources,
    IN PCM_RESOURCE_LIST TranslatedResources)
{
    ULONG Count;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR ResourceDescriptor;
    DEVICE_DESCRIPTION DeviceDescription;
    PVOID ResourceBase;
    NTSTATUS Status;

    ASSERT(KeGetCurrentIrql() <= PASSIVE_LEVEL);

    DPRINT("CUSBHardwareDevice::PnpStart\n");
    for (Count = 0; Count < TranslatedResources->List[0].PartialResourceList.Count; Count++)
    {
        //
        // get resource descriptor
        //
        ResourceDescriptor = &TranslatedResources->List[0].PartialResourceList.PartialDescriptors[Count];

        switch (ResourceDescriptor->Type)
        {
            case CmResourceTypeInterrupt:
            {
                //
                // initialize interrupt DPC object
                //
                KeInitializeDpc(&m_IntDpcObject,
                                XhciDeferredRoutine,
                                this);
                //
                // connect ISR
                //
                Status = IoConnectInterrupt(&m_Interrupt,
                                            InterruptServiceRoutine,
                                            (PVOID)this,
                                            NULL,
                                            ResourceDescriptor->u.Interrupt.Vector,
                                            (KIRQL)ResourceDescriptor->u.Interrupt.Level,
                                            (KIRQL)ResourceDescriptor->u.Interrupt.Level,
                                            (KINTERRUPT_MODE)(ResourceDescriptor->Flags & CM_RESOURCE_INTERRUPT_LATCHED),
                                            (ResourceDescriptor->ShareDisposition != CmResourceShareDeviceExclusive),
                                            ResourceDescriptor->u.Interrupt.Affinity,
                                            FALSE);
                if (!NT_SUCCESS(Status))
                {
                    //
                    // failed to register ISR
                    //
                    DPRINT1("IoConnect Interrupt failed %x\n", Status);
                    return Status;
                }
                break;
            }
            case CmResourceTypeMemory:
            {
                //
                // get resource base
                //
                ResourceBase = MmMapIoSpace(ResourceDescriptor->u.Memory.Start, ResourceDescriptor->u.Memory.Length, MmNonCached);
                if (!ResourceBase)
                {
                    //
                    // failed to map memory
                    //
                    DPRINT1("MmMapIoSpace failed. \n");
                    return STATUS_INSUFFICIENT_RESOURCES;
                }

                //
                // get capabilites
                //
                m_Capabilities.CapLength = READ_REGISTER_UCHAR((PUCHAR)ResourceBase + XHCI_CAPLENGTH);
                m_Capabilities.HciVersion = READ_REGISTER_USHORT((PUSHORT)((ULONG_PTR)ResourceBase + XHCI_HCIVERSION));
                m_Capabilities.HcsParams1Long = READ_REGISTER_ULONG((PULONG)((ULONG_PTR)ResourceBase + XHCI_HCSPARAMS1));
                m_Capabilities.HcsParams2Long = READ_REGISTER_ULONG((PULONG)((ULONG_PTR)ResourceBase + XHCI_HCSPARAMS2));
                m_Capabilities.HcsParams3Long = READ_REGISTER_ULONG((PULONG)((ULONG_PTR)ResourceBase + XHCI_HCSPARAMS3));
                m_Capabilities.HccParams1Long = READ_REGISTER_ULONG((PULONG)((ULONG_PTR)ResourceBase + XHCI_HCCPARAMS1));
                m_Capabilities.DoorBellOffset = READ_REGISTER_ULONG((PULONG)((ULONG_PTR)ResourceBase + XHCI_DBOFF));
                m_Capabilities.RunTmRegSpaceOff = READ_REGISTER_ULONG((PULONG)((ULONG_PTR)ResourceBase + XHCI_RTSOFF));
                m_Capabilities.HccParams2Long = READ_REGISTER_ULONG((PULONG)((ULONG_PTR)ResourceBase + XHCI_HCCPARAMS2));

                DPRINT1("Controller CapLength        : 0x%x\n", m_Capabilities.CapLength);
                DPRINT1("Controller HciVersion       : 0x%x\n", m_Capabilities.HciVersion);
                DPRINT1("Controller HcsParams1Long   : 0x%x\n", m_Capabilities.HcsParams1Long);
                DPRINT1("Controller HcsParams2Long   : 0x%x\n", m_Capabilities.HcsParams2Long);
                DPRINT1("Controller HcsParams3Long   : 0x%x\n", m_Capabilities.HcsParams3Long);
                DPRINT1("Controller HccParams1Long   : 0x%x\n", m_Capabilities.HccParams1Long);
                DPRINT1("Controller DoorBellOffset   : 0x%x\n", m_Capabilities.DoorBellOffset);
                DPRINT1("Controller RunTmRegSpaceOff : 0x%x\n", m_Capabilities.RunTmRegSpaceOff);
                DPRINT1("Controller HccParams2Long   : 0x%x\n", m_Capabilities.HccParams2Long);

                DPRINT1("Controller Port Count       : 0x%x\n", m_Capabilities.HcsParams1.MaxPorts);

                //
                //
                m_CapRegBase = (PULONG)ResourceBase;
                m_OpRegBase = (PULONG)((ULONG)ResourceBase + m_Capabilities.CapLength);
                m_DoorBellRegBase = (PULONG)((ULONG)ResourceBase + m_Capabilities.DoorBellOffset);
                m_RunTmRegBase = (PULONG)((ULONG)ResourceBase + m_Capabilities.RunTmRegSpaceOff);
                break;
            }
        }
    }

    //
    // zero it
    //
    RtlZeroMemory(&DeviceDescription, sizeof(DEVICE_DESCRIPTION));

    //
    // initialize device description
    //
    DeviceDescription.Version           = DEVICE_DESCRIPTION_VERSION;
    DeviceDescription.Master            = TRUE;
    DeviceDescription.ScatterGather     = TRUE;
    DeviceDescription.Dma32BitAddresses = TRUE;
    DeviceDescription.DmaWidth          = Width32Bits;
    DeviceDescription.InterfaceType     = PCIBus;
    DeviceDescription.MaximumLength     = MAXULONG;

    //
    // get dma adapter
    //
    m_Adapter = IoGetDmaAdapter(m_PhysicalDeviceObject, &DeviceDescription, &m_MapRegisters);
    if (!m_Adapter)
    {
        DPRINT("Failed to acquire dma adapter\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // create common buffer
    //
    VirtualBase = m_Adapter->DmaOperations->AllocateCommonBuffer(m_Adapter,
                                                                 PAGE_SIZE * 4,
                                                                 &PhysicalAddress,
                                                                 FALSE);
    if (!VirtualBase)
    {
        DPRINT1("Failed to allocate a common buffer\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // initialize the DMAMemoryManager
    //
    Status = m_MemoryManager->Initialize(this, &m_Lock, PAGE_SIZE * 4, VirtualBase, PhysicalAddress, 32);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Failed to initialize DMAMemoryManager\n");
        return Status;
    }

    //
    // start the controller
    //
    DPRINT1("Start Controller\n");
    Status = StartController();

    return Status;
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
    if (VendorId)
        *VendorId = m_VendorID;
    if (DeviceId)
        *DeviceId = m_DeviceID;
    if (NumberOfPorts)
        *NumberOfPorts = m_Capabilities.HcsParams1.MaxPorts;
    if (Speed)
        *Speed = 0x300; // of course ;)
    return STATUS_SUCCESS;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::GetUSBQueue(
    OUT struct IUSBQueue **OutUsbQueue)
{
    if (!m_UsbQueue)
        return STATUS_UNSUCCESSFUL;
    *OutUsbQueue = m_UsbQueue;
    return STATUS_SUCCESS;
}

NTSTATUS
STDMETHODCALLTYPE
CUSBHardwareDevice::GetDMA(
    OUT struct IDMAMemoryManager **OutDMAMemoryManager)
{
    if (!m_MemoryManager)
        return STATUS_UNSUCCESSFUL;
    *OutDMAMemoryManager = m_MemoryManager;
    return STATUS_SUCCESS;
}

NTSTATUS
CUSBHardwareDevice::StartController(VOID)
{
    UNIMPLEMENTED_DBGBREAK();
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
CUSBHardwareDevice::StopController(void)
{
    ULONG UsbStatus;
    ULONG Index = 0;

    //
    // halt the controller
    //
    WRITE_OPERATIONAL_REG_ULONG(XHCI_USBCMD, 0);

    //
    // wait for controller to halt
    //
    do
    {
        //
        // stall the processor for 10 microseconds
        //
        KeStallExecutionProcessor(10);

        UsbStatus = READ_OPERATIONAL_REG_ULONG(XHCI_USBSTS);

        Index++;
    } while (Index < 100 && !(UsbStatus & XHCI_STS_HCH));

    if (!(UsbStatus & XHCI_STS_HCH))
    {
        DPRINT1("Controller is not responding to stop request.\n");
        return STATUS_UNSUCCESSFUL;
    }

    //
    // great success
    //
    return STATUS_SUCCESS;
}

NTSTATUS
CUSBHardwareDevice::ResetController(void)
{
    ULONG UsbStatus, UsdCommand;
    ULONG Index = 0;

    //
    // reset the controller
    //
    UsdCommand = READ_OPERATIONAL_REG_ULONG(XHCI_USBCMD);
    WRITE_OPERATIONAL_REG_ULONG(XHCI_USBCMD, UsdCommand | XHCI_CMD_HCRST);

    //
    // wait for controller to halt
    //
    do
    {
        //
        // stall the processor for 10 microseconds
        //
        KeStallExecutionProcessor(10);

        UsbStatus = READ_OPERATIONAL_REG_ULONG(XHCI_USBSTS);

    } while (Index < 100 && !(UsbStatus & XHCI_STS_HCH));

    if (!(UsbStatus & XHCI_STS_HCH))
    {
        DPRINT1("Controller is not responding to reset request.\n");
        return STATUS_UNSUCCESSFUL;
    }

    //
    // wait till controller not ready is cleared
    //
    do
    {
        //
        // stall the processor for 10 microseconds
        //
        KeStallExecutionProcessor(10);

        UsbStatus = READ_OPERATIONAL_REG_ULONG(XHCI_USBSTS);

    } while (Index < 100 && (UsbStatus & XHCI_STS_CNR));

    if (UsbStatus & XHCI_STS_CNR)
    {
        //
        // failed
        //
        DPRINT1("Controller is not ready.\n");
        return STATUS_UNSUCCESSFUL;
    }

    //
    // great success
    //
    return STATUS_SUCCESS;
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
    m_SCECallBack = (HD_INIT_CALLBACK*)CallBack;
    m_SCEContext = Context;
    return;
}

LPCSTR
STDMETHODCALLTYPE
CUSBHardwareDevice::GetUSBType(VOID)
{
    return "USBXHCI";
}

BOOLEAN
NTAPI
InterruptServiceRoutine(
    IN PKINTERRUPT Interrupt,
    IN PVOID ServiceContext)
{
    UNREFERENCED_PARAMETER(Interrupt);
    UNREFERENCED_PARAMETER(ServiceContext);

    UNIMPLEMENTED_DBGBREAK();
    return FALSE;
}

VOID
NTAPI
XhciDeferredRoutine(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArgument1,
    IN PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);
   
    UNIMPLEMENTED_DBGBREAK();
    return;
}

VOID
NTAPI
StatusChangeWorkItemRoutine(PVOID Context)
{
    //
    // cast to hardware object
    //
    CUSBHardwareDevice * This = (CUSBHardwareDevice*)Context;

    //
    // is there a callback
    //
    if (This->m_SCECallBack)
    {
        //
        // issue callback
        //
        This->m_SCECallBack(This->m_SCEContext);
    }

    //
    // reset active status
    //
    InterlockedDecrement(&This->m_StatusChangeWorkItemStatus);
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

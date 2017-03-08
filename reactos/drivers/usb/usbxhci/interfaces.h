#ifndef INTERFACES_HPP
#define INTERFACES_HPP

//
// IUSBHardwareDevice
//
#define DEFINE_ABSTRACT_USBXHCIHARDWARE()               \
    STDMETHOD_(VOID, SetRuntimeRegister)(THIS_          \
        IN ULONG Offset,                                \
        IN ULONG Value) PURE;                           \
    STDMETHOD_(ULONG, GetRuntimeRegister)(THIS_         \
        IN ULONG Offset) PURE;                          \
    STDMETHOD_(VOID, SetOperationalRegister)(THIS_      \
        IN ULONG Offset,                                \
        IN ULONG Value) PURE;

#define IMP_IUSBXHCIHARDWARE                            \
    STDMETHODIMP_(VOID) SetRuntimeRegister(             \
        IN ULONG Offset,                                \
        IN ULONG Value);                                \
    STDMETHODIMP_(ULONG) GetRuntimeRegister(            \
        IN ULONG Offset);                               \
    STDMETHODIMP_(VOID) SetOperationalRegister(         \
        IN ULONG Offset,                                \
        IN ULONG Value);

DECLARE_INTERFACE_(IXHCIHardwareDevice, IUSBHardwareDevice)
{
    DEFINE_ABSTRACT_UNKNOWN();
    DEFINE_ABSTRACT_USBHARDWAREDEVICE();
    DEFINE_ABSTRACT_USBXHCIHARDWARE();
};

typedef IXHCIHardwareDevice *PXHCIHARDWAREDEVICE;

//
// IUSBRequest
//
#define DEFINE_ABSTRACT_USBXHCIREQUEST()

#define IMP_IXHCIREQUEST

DECLARE_INTERFACE_(IXHCIRequest, IUSBRequest)
{
    DEFINE_ABSTRACT_UNKNOWN();
    DEFINE_ABSTRACT_USBREQUEST();
    DEFINE_ABSTRACT_USBXHCIREQUEST();
};

typedef IXHCIRequest *PXHCIREQUEST;

//
// IUSBQueue
//
#define DEFINE_ABSTRACT_USBXHCIQUEUE()                      \
    STDMETHOD_(VOID, CompleteCommandRequest)(THIS_          \
        IN PTRB TransferRequestBlock) PURE;                 \
    STDMETHOD_(VOID, CompleteTransferRequest)(THIS_         \
        IN PTRB TransferRequestBlock) PURE;                 \
    STDMETHOD_(BOOLEAN, IsEventRingEmpty)(THIS_) PURE;      \
    STDMETHOD_(PTRB, GetEventRingDequeuePointer)(THIS_) PURE;

#define IMP_IXHCIQUEUE                                      \
    STDMETHODIMP_(VOID) CompleteCommandRequest(             \
        IN PTRB TransferRequestBlock);                      \
    STDMETHODIMP_(VOID) CompleteTransferRequest(            \
        IN PTRB TransferRequestBlock);                      \
    STDMETHODIMP_(BOOLEAN) IsEventRingEmpty(VOID);          \
    STDMETHODIMP_(PTRB) GetEventRingDequeuePointer(VOID);

DECLARE_INTERFACE_(IXHCIQueue, IUSBQueue)
{
    DEFINE_ABSTRACT_UNKNOWN();
    DEFINE_ABSTRACT_USBQUEUE();
    DEFINE_ABSTRACT_USBXHCIQUEUE();
};

typedef IXHCIQueue *PXHCIQUEUE;
#endif // INTERFACES_HPP

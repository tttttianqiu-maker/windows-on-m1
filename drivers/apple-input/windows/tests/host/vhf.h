/* Host-only stand-ins for WDK headers, just enough for tests/avhd_sim_test.c. */
#pragma once
typedef struct VHFHANDLE__*VHFHANDLE; typedef struct VHFOP__*VHFOPERATIONHANDLE;
typedef struct { PUCHAR reportBuffer; ULONG reportBufferLen; UCHAR reportId; } HID_XFER_PACKET,*PHID_XFER_PACKET;
typedef VOID EVT_VHF_ASYNC_OPERATION(PVOID,VHFOPERATIONHANDLE,PVOID,PHID_XFER_PACKET);
typedef struct { PVOID VhfClientContext; EVT_VHF_ASYNC_OPERATION *EvtVhfAsyncOperationGetFeature,*EvtVhfAsyncOperationSetFeature; USHORT VendorID,ProductID,VersionNumber; } VHF_CONFIG;
void VHF_CONFIG_INIT(VHF_CONFIG*,PDEVICE_OBJECT,USHORT,PUCHAR); NTSTATUS VhfCreate(VHF_CONFIG*,VHFHANDLE*); NTSTATUS VhfStart(VHFHANDLE); void VhfDelete(VHFHANDLE,BOOLEAN);
NTSTATUS VhfReadReportSubmit(VHFHANDLE,PHID_XFER_PACKET); NTSTATUS VhfAsyncOperationComplete(VHFOPERATIONHANDLE,NTSTATUS);

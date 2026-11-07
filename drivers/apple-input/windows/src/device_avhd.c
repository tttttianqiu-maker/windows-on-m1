/*
 * J613 (MacBook Air M3) device glue for the AVHD virtual HID page that m1n1
 * exposes (m1n1_windows/src/hv_dchid.h). Replaces device.c, spi.c and gpio.c
 * of the J313 build: m1n1 owns the MTP coprocessor, DockChannel, trackpad
 * firmware and SMC GPIO power sequencing, so Windows only sees one 4 KiB
 * MMIO page and one edge-triggered interrupt.
 */
#include "apple_input_device.h"
#include "apple_input_avhd.h"

static BOOLEAN AiReadParameter(WDFDRIVER Driver, PCWSTR Name, BOOLEAN Default)
{
    UNICODE_STRING value_name;
    WDFKEY parameters_key;
    ULONG value = Default ? 1u : 0u;
    NTSTATUS status;

    status = WdfDriverOpenParametersRegistryKey(
        Driver, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &parameters_key);
    if (!NT_SUCCESS(status))
        return Default;
    RtlInitUnicodeString(&value_name, Name);
    status = WdfRegistryQueryULong(parameters_key, &value_name, &value);
    WdfRegistryClose(parameters_key);
    if (!NT_SUCCESS(status))
        return Default;
    return value == 0 ? FALSE : TRUE;
}

NTSTATUS AppleInputCreateDevice(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS callbacks;
    WDF_INTERRUPT_CONFIG interrupt_config;
    WDF_OBJECT_ATTRIBUTES lock_attributes;
    WDF_OBJECT_ATTRIBUTES timer_attributes;
    WDF_TIMER_CONFIG timer_config;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE device;
    PAI_DEVICE_CONTEXT context;
    NTSTATUS status;

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&callbacks);
    callbacks.EvtDevicePrepareHardware = AppleInputEvtDevicePrepareHardware;
    callbacks.EvtDeviceReleaseHardware = AppleInputEvtDeviceReleaseHardware;
    callbacks.EvtDeviceD0Entry = AppleInputEvtDeviceD0Entry;
    callbacks.EvtDeviceD0EntryPostInterruptsEnabled =
        AppleInputEvtDeviceD0EntryPostInterruptsEnabled;
    callbacks.EvtDeviceD0ExitPreInterruptsDisabled =
        AppleInputEvtDeviceD0ExitPreInterruptsDisabled;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &callbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, AI_DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status))
        return status;

    context = AiGetDeviceContext(device);
    context->Device = device;
    context->KeyboardVhfState = AiVhfAbsent;
    context->TrackpadVhfState = AiVhfAbsent;
    context->Diagnostics.Version = AI_DIAGNOSTIC_SNAPSHOT_VERSION_4;
    context->Diagnostics.Size = sizeof(context->Diagnostics);
    /* J613 defaults to publishing: the INF sets these explicitly as well. */
    context->TransportOnly = AiReadParameter(Driver, L"TransportOnly", FALSE);
    context->PublishKeyboard = AiReadParameter(Driver, L"PublishKeyboard", TRUE);
    context->PublishTrackpad = AiReadParameter(Driver, L"PublishTrackpad", TRUE);

    WDF_OBJECT_ATTRIBUTES_INIT(&lock_attributes);
    lock_attributes.ParentObject = device;
    status = WdfWaitLockCreate(&lock_attributes, &context->TransportLock);
    if (!NT_SUCCESS(status))
        return status;
    status = WdfWaitLockCreate(&lock_attributes, &context->FrontendLock);
    if (!NT_SUCCESS(status))
        return status;

    /* Readiness poll + missed-interrupt safety drain (see transport_avhd.c). */
    WDF_TIMER_CONFIG_INIT(&timer_config, AiTrackpadInitTimer);
    timer_config.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&timer_attributes);
    timer_attributes.ParentObject = device;
    timer_attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timer_config, &timer_attributes,
                            &context->TrackpadInitTimer);
    if (!NT_SUCCESS(status))
        return status;

    /*
     * A plain GIC SPI: DIRQL ISR that only queues the passive work item.
     * (Passive-level ISRs are meant for GPIO/secondary controllers.)
     */
    WDF_INTERRUPT_CONFIG_INIT(&interrupt_config, AiInputInterruptIsr, NULL);
    interrupt_config.EvtInterruptWorkItem = AiTransportWorker;
    interrupt_config.PassiveHandling = FALSE;
    interrupt_config.AutomaticSerialization = FALSE;
    status = WdfInterruptCreate(device, &interrupt_config,
                                WDF_NO_OBJECT_ATTRIBUTES,
                                &context->Interrupt);
    if (!NT_SUCCESS(status))
        return status;
    return AiDiagnosticsInitialize(device, context);
}

NTSTATUS AiDeviceParseResources(WDFCMRESLIST Raw, WDFCMRESLIST Translated,
                                PAI_DEVICE_CONTEXT Context)
{
    ULONG memory = 0;
    ULONG interrupts = 0;
    ULONG raw_count = WdfCmResourceListGetCount(Raw);
    ULONG translated_count = WdfCmResourceListGetCount(Translated);

    RtlZeroMemory(Context->MemoryBase, sizeof(Context->MemoryBase));
    RtlZeroMemory(Context->MemoryLength, sizeof(Context->MemoryLength));
    Context->InterruptVector = 0;
    Context->ResourcesValidated = FALSE;
    if (raw_count != translated_count)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    for (ULONG index = 0; index < translated_count; index++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR raw_resource =
            WdfCmResourceListGetDescriptor(Raw, index);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR translated_resource =
            WdfCmResourceListGetDescriptor(Translated, index);
        if (raw_resource == NULL || translated_resource == NULL ||
            raw_resource->Type != translated_resource->Type)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (translated_resource->Type == CmResourceTypeMemory) {
            if (memory++ != 0 ||
                raw_resource->u.Memory.Start.QuadPart != AI_AVHD_IPA ||
                translated_resource->u.Memory.Length < AI_AVHD_SIZE)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            Context->MemoryBase[0] = translated_resource->u.Memory.Start;
            Context->MemoryLength[0] = AI_AVHD_SIZE;
        } else if (translated_resource->Type == CmResourceTypeInterrupt) {
            if (interrupts++ != 0)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            Context->InterruptVector = translated_resource->u.Interrupt.Vector;
        }
    }
    if (memory != 1 || interrupts != 1)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    Context->ResourcesValidated = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS AppleInputEvtDevicePrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw,
                                             WDFCMRESLIST Translated)
{
    PAI_DEVICE_CONTEXT context = AiGetDeviceContext(Device);
    NTSTATUS status = AiDeviceParseResources(Raw, Translated, context);

    if (!NT_SUCCESS(status))
        return status;
    context->AvhdRegisters = MmMapIoSpaceEx(context->MemoryBase[0],
        context->MemoryLength[0], PAGE_READWRITE | PAGE_NOCACHE);
    if (!context->AvhdRegisters) {
        context->ResourcesValidated = FALSE;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    /* Without m1n1's hook the page is unbacked: refuse to start. */
    if (AiAvhdRead(context, AI_AVHD_MAGIC) != AI_AVHD_MAGIC_VALUE ||
        AiAvhdRead(context, AI_AVHD_VERSION) < 1u) {
        AppleInputEvtDeviceReleaseHardware(Device, Translated);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}

NTSTATUS AppleInputEvtDeviceReleaseHardware(WDFDEVICE Device,
                                             WDFCMRESLIST Translated)
{
    PAI_DEVICE_CONTEXT context = AiGetDeviceContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    AiTransportStop(context);
    if (context->AvhdRegisters)
        MmUnmapIoSpace(context->AvhdRegisters, context->MemoryLength[0]);
    context->AvhdRegisters = NULL;
    context->ResourcesValidated = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS AppleInputEvtDeviceD0Entry(WDFDEVICE Device,
                                    WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    return AiGetDeviceContext(Device)->ResourcesValidated ?
        STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

NTSTATUS AppleInputEvtDeviceD0EntryPostInterruptsEnabled(
    WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    return AiTransportStart(AiGetDeviceContext(Device));
}

NTSTATUS AppleInputEvtDeviceD0ExitPreInterruptsDisabled(
    WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);
    AiTransportStop(AiGetDeviceContext(Device));
    return STATUS_SUCCESS;
}

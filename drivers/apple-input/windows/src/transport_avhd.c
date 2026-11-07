/*
 * J613 transport: drains HID reports from the m1n1 AVHD page and feeds them
 * into the same VHF keyboard / Precision Touchpad pipeline as the J313 SPI
 * transport (transport.c).
 *
 *   keyboard  : MTP keyboard reports are plain HID input reports for the
 *               descriptor m1n1 received in the DockChannel INIT event; they
 *               go to the keyboard VHF unchanged.
 *   trackpad  : MTP report 0x75 is the J313 SPI trackpad report without its
 *               8-byte mouse prefix. ai_mtp_trackpad_to_spi() restores the
 *               prefix and the J313 decoder/tracker/PTP encoder run as-is.
 *   dimensions: AVHD DIMS = multi-touch feature report 0xd9 body, parsed by
 *               the J313 axis code via ai_mtp_dimensions_parse().
 *
 * Diagnostics reuse the J313 snapshot (AppleInputDiag works unchanged):
 *   TransportPhase       AI_DISCOVERY_READY once the keyboard descriptor is in
 *   SpiTransferCount     reports read from the AVHD window
 *   FragmentFailureCount m1n1 RX_DROPS (queue overflow inside the hypervisor)
 *   PacketCrcFailureCount reports with a bad length/interface
 */
#include "apple_input_device.h"
#include "apple_input_avhd.h"
#include "apple_mtp.h"

#define AI_AVHD_MAX_REPORTS_PER_WORKER 64u
#define AI_AVHD_POLL_STARTUP_MS 100u
#define AI_AVHD_POLL_STEADY_MS 500u
#define AI_AVHD_REPORT_MAX 1024u

static UCHAR g_AiAvhdReport[AI_AVHD_REPORT_MAX];
static UCHAR g_AiAvhdSpiReport[AI_AVHD_REPORT_MAX + AI_MTP_SPI_PREFIX_SIZE];
static UCHAR g_AiAvhdDescriptor[AI_DESCRIPTOR_MAX];

static VOID AiCounterIncrement(ULONGLONG *Counter)
{
    InterlockedIncrement64((volatile LONG64 *)Counter);
}

static BOOLEAN AiAvhdOnline(PAI_DEVICE_CONTEXT Context)
{
    return Context->AvhdRegisters && Context->HardwareStarted &&
           !Context->Stopping;
}

/* TransportLock held. Load the keyboard descriptor once m1n1 reports it ready. */
static VOID AiAvhdLoadKeyboard(PAI_DEVICE_CONTEXT Context)
{
    const struct ai_descriptor_slot *descriptor;
    ULONG length;

    if (Context->Descriptors.keyboard.valid)
        return;
    AiAvhdWrite(Context, AI_AVHD_DESC_SEL, 0u);
    length = AiAvhdRead(Context, AI_AVHD_DESC_LEN);
    if (!length || length > AI_DESCRIPTOR_MAX || length > AI_AVHD_DESC_WINDOW)
        return;
    AiAvhdReadWindow(Context, AI_AVHD_DESC, g_AiAvhdDescriptor, length);
    if (ai_descriptor_store_put(&Context->Descriptors, 1u,
                                g_AiAvhdDescriptor, length) != AI_OK)
        return;
    descriptor = ai_descriptor_store_get(&Context->Descriptors, 1u);
    if (!descriptor)
        return;
    if (ai_hid_input_contract_parse(descriptor->bytes, descriptor->length,
                                    &Context->KeyboardInputContract) == AI_OK)
        Context->Diagnostics.KeyboardContractValid = TRUE;
    AiDiagnosticsRecordDescriptor(Context, descriptor);

    /* Keep the raw multi-touch descriptor for diagnostics (not published). */
    AiAvhdWrite(Context, AI_AVHD_DESC_SEL, 1u);
    length = AiAvhdRead(Context, AI_AVHD_DESC_LEN);
    if (length && length <= AI_DESCRIPTOR_MAX && length <= AI_AVHD_DESC_WINDOW) {
        AiAvhdReadWindow(Context, AI_AVHD_DESC, g_AiAvhdDescriptor, length);
        if (ai_descriptor_store_put(&Context->Descriptors, 2u,
                                    g_AiAvhdDescriptor, length) == AI_OK)
            AiDiagnosticsRecordDescriptor(
                Context, ai_descriptor_store_get(&Context->Descriptors, 2u));
    }
    AiAvhdWrite(Context, AI_AVHD_DESC_SEL, 0u);
    Context->Discovery.phase = AI_DISCOVERY_READY;
}

/* TransportLock held. Trackpad becomes usable once firmware + dimensions are in. */
static VOID AiAvhdLoadTrackpad(PAI_DEVICE_CONTEXT Context)
{
    UCHAR dims[AI_MTP_DIMENSIONS_SIZE];

    if (Context->TrackpadInit.phase == AI_TRACKPAD_INIT_READY ||
        Context->TrackpadInit.phase == AI_TRACKPAD_INIT_OFFLINE)
        return;
    AiAvhdReadWindow(Context, AI_AVHD_DIMS, dims, sizeof(dims));
    if (ai_mtp_dimensions_parse(dims, &Context->TrackpadInit.dimensions) != AI_OK ||
        ai_trackpad_axis_contract_from_dimensions(
            &Context->TrackpadInit.dimensions,
            &Context->TrackpadAxisContract) != AI_OK) {
        Context->TrackpadInit.phase = AI_TRACKPAD_INIT_OFFLINE;
        AiCounterIncrement(&Context->Diagnostics.OfflineCount);
        return;
    }
    AiTrackpadRecordAxisDiagnostics(Context);
    Context->TrackpadInit.phase = AI_TRACKPAD_INIT_READY;
}

/* TransportLock held. Returns TRUE once everything m1n1 offers is published. */
static BOOLEAN AiAvhdRefreshReadiness(PAI_DEVICE_CONTEXT Context)
{
    ULONG status = AiAvhdRead(Context, AI_AVHD_STATUS);
    BOOLEAN keyboard_done = TRUE;
    BOOLEAN trackpad_done = TRUE;
    NTSTATUS frontend;

    if (status & AI_AVHD_STATUS_KEYBOARD)
        AiAvhdLoadKeyboard(Context);
    if ((status & (AI_AVHD_STATUS_TRACKPAD | AI_AVHD_STATUS_DIMS)) ==
        (AI_AVHD_STATUS_TRACKPAD | AI_AVHD_STATUS_DIMS))
        AiAvhdLoadTrackpad(Context);

    if (Context->PublishKeyboard && Context->KeyboardVhfState != AiVhfRunning)
        keyboard_done = FALSE;
    if (Context->PublishTrackpad && !Context->TrackpadPublicationFailed &&
        Context->TrackpadInit.phase != AI_TRACKPAD_INIT_OFFLINE &&
        Context->TrackpadVhfState != AiVhfRunning)
        trackpad_done = FALSE;

    if ((!keyboard_done && Context->Descriptors.keyboard.valid) ||
        (!trackpad_done && Context->TrackpadInit.phase == AI_TRACKPAD_INIT_READY)) {
        frontend = AiVhfFrontendStart(Context);
        Context->Diagnostics.KeyboardVhfLastStatus = frontend;
        if (!NT_SUCCESS(frontend))
            AiCounterIncrement(&Context->Diagnostics.KeyboardVhfStartFailureCount);
    }
    Context->Diagnostics.FragmentFailureCount =
        AiAvhdRead(Context, AI_AVHD_RX_DROPS);
    return (Context->KeyboardVhfState == AiVhfRunning || !Context->PublishKeyboard) &&
           (Context->TrackpadVhfState == AiVhfRunning || !Context->PublishTrackpad ||
            Context->TrackpadPublicationFailed ||
            Context->TrackpadInit.phase == AI_TRACKPAD_INIT_OFFLINE);
}

static VOID AiAvhdDispatchKeyboard(PAI_DEVICE_CONTEXT Context,
                                   UCHAR *Report, ULONG Length)
{
    const struct ai_hid_input_contract *contract =
        &Context->KeyboardInputContract;
    NTSTATUS status;

    AiCounterIncrement(&Context->Diagnostics.KeyboardReportCount);
    /*
     * Like Linux hid_report_raw_event(): zero-pad a short report and drop
     * trailing bytes of a long one, so the size matches the descriptor.
     */
    if (contract->valid && Length) {
        ULONG expected = contract->bytes_by_id[
            contract->uses_report_ids ? Report[0] : 0u];
        if (expected && expected <= AI_AVHD_REPORT_MAX && Length != expected) {
            if (Length < expected)
                RtlZeroMemory(Report + Length, expected - Length);
            Length = expected;
        }
    }
    if (!contract->valid ||
        !ai_hid_input_report_valid(&Context->KeyboardInputContract,
                                   Report, Length, NULL)) {
        AiCounterIncrement(&Context->Diagnostics.KeyboardReportRejectedCount);
        return;
    }
    AiCounterIncrement(&Context->Diagnostics.KeyboardReportAcceptedCount);
    if (Context->TransportOnly || !Context->PublishKeyboard)
        return;
    status = AiVhfFrontendSubmitKeyboard(Context, Report, Length);
    Context->Diagnostics.KeyboardVhfLastStatus = status;
    if (NT_SUCCESS(status))
        AiCounterIncrement(&Context->Diagnostics.KeyboardReportSubmittedCount);
    else
        AiCounterIncrement(&Context->Diagnostics.KeyboardVhfSubmissionFailureCount);
}

static VOID AiAvhdDispatchTrackpad(PAI_DEVICE_CONTEXT Context,
                                   const UCHAR *Report, ULONG Length)
{
    size_t spi_length = 0;

    AiCounterIncrement(&Context->Diagnostics.TrackpadReportCount);
    /* Other multi-touch report IDs (status, etc.) are not touch frames. */
    if (Length == 0 || Report[0] != AI_MTP_TRACKPAD_REPORT_ID)
        return;
    if (ai_mtp_trackpad_to_spi(Report, Length, g_AiAvhdSpiReport,
                               sizeof(g_AiAvhdSpiReport), &spi_length) != AI_OK) {
        AiCounterIncrement(&Context->Diagnostics.TrackpadReportRejectedCount);
        return;
    }
    (VOID)AiTrackpadPipelineProcess(Context, g_AiAvhdSpiReport, spi_length);
}

/* TransportLock held. Pop up to Budget reports; returns the number drained. */
static ULONG AiAvhdDrain(PAI_DEVICE_CONTEXT Context, ULONG Budget)
{
    ULONG drained = 0;

    while (drained < Budget && AiAvhdOnline(Context)) {
        ULONG length = AiAvhdRead(Context, AI_AVHD_RX_LEN);
        ULONG iface;

        if (!length)
            break;
        iface = AiAvhdRead(Context, AI_AVHD_RX_IFACE);
        if (length > AI_AVHD_REPORT_MAX || length > AI_AVHD_REPORT_WINDOW) {
            AiCounterIncrement(&Context->Diagnostics.PacketCrcFailureCount);
            AiAvhdWrite(Context, AI_AVHD_RX_POP, 1u);
            drained++;
            continue;
        }
        AiAvhdReadWindow(Context, AI_AVHD_REPORT, g_AiAvhdReport, length);
        AiAvhdWrite(Context, AI_AVHD_RX_POP, 1u);
        AiCounterIncrement(&Context->Diagnostics.SpiTransferCount);
        drained++;

        if (iface == AI_AVHD_IFACE_KEYBOARD)
            AiAvhdDispatchKeyboard(Context, g_AiAvhdReport, length);
        else if (iface == AI_AVHD_IFACE_TRACKPAD)
            AiAvhdDispatchTrackpad(Context, g_AiAvhdReport, length);
        else
            AiCounterIncrement(&Context->Diagnostics.PacketCrcFailureCount);
    }
    return drained;
}

BOOLEAN AiInputInterruptIsr(WDFINTERRUPT Interrupt, ULONG MessageID)
{
    PAI_DEVICE_CONTEXT context = AiGetDeviceContext(
        WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(MessageID);
    /* Edge-triggered and not shared: always ours. */
    AiCounterIncrement(&context->Diagnostics.InterruptCount);
    if (!context->HardwareStarted || context->Stopping) {
        if (context->AvhdRegisters)
            AiAvhdWrite(context, AI_AVHD_IRQ_ACK, 1u);
        return TRUE;
    }
    if (WdfInterruptQueueWorkItemForIsr(Interrupt))
        AiCounterIncrement(&context->Diagnostics.WorkerQueuedCount);
    return TRUE;
}

VOID AiTransportWorker(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject)
{
    PAI_DEVICE_CONTEXT context = AiGetDeviceContext(
        WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(AssociatedObject);
    WdfWaitLockAcquire(context->TransportLock, NULL);
    if (AiAvhdOnline(context)) {
        (VOID)AiAvhdDrain(context, AI_AVHD_MAX_REPORTS_PER_WORKER);
        /*
         * Re-arm after draining. A report queued between the drain and this
         * write is picked up by m1n1's next tick, which re-injects because
         * its queue is non-empty.
         */
        AiAvhdWrite(context, AI_AVHD_IRQ_ACK, 1u);
    }
    WdfWaitLockRelease(context->TransportLock);
    AiCounterIncrement(&context->Diagnostics.WorkerCompletedCount);
    AiDiagnosticsPublish(context);
}

/*
 * Periodic: brings the VHF devices up as m1n1 reports them ready (the
 * trackpad needs its firmware and dimensions first), and drains anything a
 * lost interrupt left behind.
 */
VOID AiTrackpadInitTimer(WDFTIMER Timer)
{
    PAI_DEVICE_CONTEXT context = AiGetDeviceContext(
        (WDFDEVICE)WdfTimerGetParentObject(Timer));
    BOOLEAN settled = FALSE;
    BOOLEAN online;

    WdfWaitLockAcquire(context->TransportLock, NULL);
    online = AiAvhdOnline(context);
    if (online) {
        settled = AiAvhdRefreshReadiness(context);
        if (AiAvhdDrain(context, AI_AVHD_MAX_REPORTS_PER_WORKER))
            AiAvhdWrite(context, AI_AVHD_IRQ_ACK, 1u);
    }
    AiDiagnosticsPublish(context);
    WdfWaitLockRelease(context->TransportLock);
    if (online && !context->Stopping)
        WdfTimerStart(context->TrackpadInitTimer, WDF_REL_TIMEOUT_IN_MS(
            settled ? AI_AVHD_POLL_STEADY_MS : AI_AVHD_POLL_STARTUP_MS));
}

NTSTATUS AiTransportStart(PAI_DEVICE_CONTEXT Context)
{
    if (!Context || !Context->ResourcesValidated || !Context->AvhdRegisters)
        return STATUS_DEVICE_NOT_READY;
    WdfTimerStop(Context->TrackpadInitTimer, TRUE);
    WdfWaitLockAcquire(Context->TransportLock, NULL);
    AI_MEMSET(&Context->Discovery, sizeof(Context->Discovery));
    AI_MEMSET(&Context->TrackpadInit, sizeof(Context->TrackpadInit));
    AI_MEMSET(&Context->TrackpadTracker, sizeof(Context->TrackpadTracker));
    ai_descriptor_store_reset(&Context->Descriptors);
    AI_MEMSET(&Context->KeyboardInputContract,
              sizeof(Context->KeyboardInputContract));
    RtlZeroMemory(&Context->Diagnostics, sizeof(Context->Diagnostics));
    Context->Diagnostics.Version = AI_DIAGNOSTIC_SNAPSHOT_VERSION_4;
    Context->Diagnostics.Size = sizeof(Context->Diagnostics);
    Context->TrackpadScanTime100us = 0;
    Context->TrackpadConsecutiveSubmissionFailures = 0;
    Context->TrackpadPublicationFailed = FALSE;
    Context->Stopping = FALSE;
    Context->HardwareStarted = TRUE;
    AiCounterIncrement(&Context->Diagnostics.ResetCount);
    (VOID)AiAvhdRefreshReadiness(Context);
    /* Release an interrupt m1n1 may have raised before we were listening. */
    AiAvhdWrite(Context, AI_AVHD_IRQ_ACK, 1u);
    AiDiagnosticsPublish(Context);
    WdfWaitLockRelease(Context->TransportLock);
    WdfTimerStart(Context->TrackpadInitTimer,
                  WDF_REL_TIMEOUT_IN_MS(AI_AVHD_POLL_STARTUP_MS));
    return STATUS_SUCCESS;
}

VOID AiTransportStop(PAI_DEVICE_CONTEXT Context)
{
    if (!Context)
        return;
    Context->Stopping = TRUE;
    Context->HardwareStarted = FALSE;
    WdfTimerStop(Context->TrackpadInitTimer, TRUE);
    WdfWaitLockAcquire(Context->TransportLock, NULL);
    Context->TrackpadInit.phase = AI_TRACKPAD_INIT_IDLE;
    AiVhfFrontendStop(Context);
    AiDiagnosticsPublish(Context);
    WdfWaitLockRelease(Context->TransportLock);
}

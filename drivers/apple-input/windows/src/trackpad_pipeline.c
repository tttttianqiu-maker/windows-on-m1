/*
 * Transport-independent trackpad pipeline: Apple native frame ->
 * contact tracker -> Windows Precision Touchpad report -> VHF.
 * Used by the J613 AVHD transport (transport_avhd.c). This is the J313
 * AiTransportProcessTrackpadReport() from transport.c, kept as an exported
 * copy so the hardware-validated J313 build stays byte-identical; merge the
 * two once J613 input is validated on hardware.
 */
#include "apple_input_device.h"

#define AI_TRACKPAD_VHF_FAILURE_LIMIT 3u

static VOID AiCounterIncrement(ULONGLONG *Counter)
{
    InterlockedIncrement64((volatile LONG64 *)Counter);
}

ULONGLONG AiNowMicroseconds(VOID)
{
    LARGE_INTEGER frequency;
    ULONGLONG count = (ULONGLONG)
        KeQueryPerformanceCounter(&frequency).QuadPart;
    ULONGLONG hz = (ULONGLONG)frequency.QuadPart;

    if (!hz)
        return 0;
    return (count / hz) * 1000000ull +
           ((count % hz) * 1000000ull) / hz;
}

static USHORT AiTrackpadNextScanTime(PAI_DEVICE_CONTEXT Context)
{
    USHORT next = (USHORT)(AiNowMicroseconds() / 100u);

    if (next == Context->TrackpadScanTime100us)
        next++;
    Context->TrackpadScanTime100us = next;
    return Context->TrackpadScanTime100us;
}

static VOID AiTrackpadReject(PAI_DEVICE_CONTEXT Context,
                             enum AI_TRACKPAD_REJECTION Reason)
{
    AiCounterIncrement(&Context->Diagnostics.TrackpadReportRejectedCount);
    Context->Diagnostics.TrackpadLastRejection = (ULONG)Reason;
}

NTSTATUS AiTrackpadPipelineProcess(
    PAI_DEVICE_CONTEXT Context, const UCHAR *Payload, SIZE_T Length)
{
    struct ai_apple_trackpad_frame native_frame;
    struct ai_trackpad_output_frame output_frame;
    struct ai_ptp_feature_state features;
    UCHAR report[AI_PTP_INPUT_REPORT_SIZE];
    enum ai_status protocol_status;
    size_t report_length = 0;
    bool neutral_required;
    KIRQL old_irql;
    NTSTATUS status;

    if (!Context || !Payload)
        return STATUS_INVALID_PARAMETER;
    if (Context->TrackpadInit.phase != AI_TRACKPAD_INIT_READY ||
        Context->TransportOnly || !Context->PublishTrackpad ||
        Context->TrackpadPublicationFailed ||
        Context->TrackpadVhfState != AiVhfRunning)
        return STATUS_SUCCESS;

    protocol_status = ai_apple_trackpad_decode(Payload, Length,
                                                &native_frame);
    if (protocol_status != AI_OK) {
        AiTrackpadReject(Context, AiTrackpadRejectDecode);
        return STATUS_SUCCESS;
    }
    AiCounterIncrement(&Context->Diagnostics.TrackpadReportDecodedCount);
    protocol_status = ai_trackpad_tracker_update(
        &Context->TrackpadTracker, &native_frame, &output_frame);
    if (protocol_status != AI_OK) {
        AiTrackpadReject(Context, AiTrackpadRejectTrack);
        return STATUS_SUCCESS;
    }
    Context->Diagnostics.TrackpadActiveCount = output_frame.active_count;
    Context->Diagnostics.TrackpadAdmittedCount = output_frame.active_count;
    Context->Diagnostics.TrackpadSuppressedCount =
        output_frame.suppressed_count;

    KeAcquireSpinLock(&Context->TrackpadVhf.FeatureLock, &old_irql);
    Context->TrackpadVhf.ContactsActive =
        output_frame.active_count != 0u ? TRUE : FALSE;
    (VOID)ai_ptp_feature_contacts_update(
        &Context->TrackpadVhf.Features,
        output_frame.active_count != 0u);
    features = Context->TrackpadVhf.Features;
    neutral_required = Context->TrackpadVhf.NeutralRequired ? true : false;
    Context->TrackpadVhf.NeutralRequired = FALSE;
    if (ai_ptp_feature_take_neutral(&Context->TrackpadVhf.Features))
        neutral_required = true;
    KeReleaseSpinLock(&Context->TrackpadVhf.FeatureLock, old_irql);

    protocol_status = ai_ptp_encode_input(
        &Context->TrackpadAxisContract, &output_frame,
        AiTrackpadNextScanTime(Context), &features,
        report, sizeof(report), &report_length);
    if (protocol_status != AI_OK) {
        AiTrackpadReject(Context, AiTrackpadRejectEncode);
        return STATUS_SUCCESS;
    }
    if (neutral_required) {
        protocol_status = ai_ptp_encode_neutral(
            Context->TrackpadScanTime100us,
            report, sizeof(report), &report_length);
        if (protocol_status != AI_OK) {
            AiTrackpadReject(Context, AiTrackpadRejectEncode);
            return STATUS_SUCCESS;
        }
    }
    if (!report_length)
        return STATUS_SUCCESS;

    status = AiVhfFrontendSubmitTrackpad(
        Context, report, report_length);
    Context->Diagnostics.TrackpadVhfLastStatus = status;
    if (NT_SUCCESS(status)) {
        Context->TrackpadConsecutiveSubmissionFailures = 0;
        Context->Diagnostics.TrackpadLastRejection = AiTrackpadRejectNone;
        AiCounterIncrement(
            &Context->Diagnostics.TrackpadReportSubmittedCount);
        return STATUS_SUCCESS;
    }

    AiCounterIncrement(
        &Context->Diagnostics.TrackpadVhfSubmissionFailureCount);
    AiTrackpadReject(Context, AiTrackpadRejectSubmit);
    Context->TrackpadConsecutiveSubmissionFailures++;
    if (Context->TrackpadConsecutiveSubmissionFailures >=
        AI_TRACKPAD_VHF_FAILURE_LIMIT) {
        Context->TrackpadPublicationFailed = TRUE;
        AiVhfFrontendStopTrackpad(Context);
    }
    return STATUS_SUCCESS;
}

VOID AiTrackpadRecordAxisDiagnostics(PAI_DEVICE_CONTEXT Context)
{
    const struct ai_trackpad_axis_contract *axis =
        &Context->TrackpadAxisContract;

    Context->Diagnostics.TrackpadAxisXValid = axis->x.valid ? TRUE : FALSE;
    Context->Diagnostics.TrackpadAxisYValid = axis->y.valid ? TRUE : FALSE;
    Context->Diagnostics.TrackpadLogicalXMinimum = axis->x.logical_min;
    Context->Diagnostics.TrackpadLogicalXMaximum = axis->x.logical_max;
    Context->Diagnostics.TrackpadLogicalYMinimum = axis->y.logical_min;
    Context->Diagnostics.TrackpadLogicalYMaximum = axis->y.logical_max;
    Context->Diagnostics.TrackpadPhysicalXMinimum = axis->x.physical_min;
    Context->Diagnostics.TrackpadPhysicalXMaximum = axis->x.physical_max;
    Context->Diagnostics.TrackpadPhysicalYMinimum = axis->y.physical_min;
    Context->Diagnostics.TrackpadPhysicalYMaximum = axis->y.physical_max;
    Context->Diagnostics.TrackpadUnit = axis->x.unit;
    Context->Diagnostics.TrackpadUnitExponent = axis->x.unit_exponent;
}

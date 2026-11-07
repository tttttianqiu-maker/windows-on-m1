/*
 * Host simulation of the J613 AVHD transport.
 *
 * Builds the real transport_avhd.c, trackpad_pipeline.c and vhf_*.c against
 * the stand-in WDK headers in tests/host/, with a register model that follows
 * m1n1_windows/src/hv_dchid.c (avhd_hook, vhid_kick), and checks the whole
 * path: readiness -> VHF creation -> keyboard/trackpad reports reaching VHF.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "apple_input_device.h"
#include "apple_input_avhd.h"
#include "apple_mtp.h"
#include "../../protocol/tests/fixtures/j313_trackpad_sanitized.h"

/* ---------------- m1n1 AVHD model ---------------- */

static UCHAR g_page[AI_AVHD_SIZE];
static struct {
    ULONG status, desc_sel, drops;
    BOOLEAN irq_pending;
    ULONG irq_acks;
    UCHAR kbd_desc[64]; ULONG kbd_desc_len;
    UCHAR mt_desc[16]; ULONG mt_desc_len;
    UCHAR dims[16];
    struct { UCHAR data[1024]; ULONG len, iface; } q[8];
    ULONG head, count;
} m;

static void model_push(ULONG iface, const UCHAR *data, ULONG len)
{
    ULONG slot = (m.head + m.count) % 8u;
    assert(m.count < 8u);
    memcpy(m.q[slot].data, data, len);
    m.q[slot].len = len;
    m.q[slot].iface = iface;
    m.count++;
}

static ULONG window_word(const UCHAR *buf, ULONG len, ULONG off)
{
    ULONG v = 0;
    for (ULONG i = 0; i < 4; i++)
        if (off + i < len)
            v |= (ULONG)buf[off + i] << (8 * i);
    return v;
}

ULONG READ_REGISTER_ULONG(volatile ULONG *addr)
{
    ULONG off = (ULONG)((volatile UCHAR *)addr - g_page);
    assert(off < AI_AVHD_SIZE && (off & 3u) == 0);
    switch (off) {
    case AI_AVHD_MAGIC: return AI_AVHD_MAGIC_VALUE;
    case AI_AVHD_VERSION: return 1;
    case AI_AVHD_STATUS: return m.status;
    case AI_AVHD_DESC_LEN: return m.desc_sel ? m.mt_desc_len : m.kbd_desc_len;
    case AI_AVHD_RX_LEN: return m.count ? m.q[m.head].len : 0;
    case AI_AVHD_RX_IFACE: return m.count ? m.q[m.head].iface : 0;
    case AI_AVHD_RX_DROPS: return m.drops;
    default: break;
    }
    if (off >= AI_AVHD_DIMS && off < AI_AVHD_DIMS + 16u)
        return window_word(m.dims, 16, off - AI_AVHD_DIMS);
    if (off >= AI_AVHD_DESC && off < AI_AVHD_REPORT)
        return m.desc_sel ? window_word(m.mt_desc, m.mt_desc_len, off - AI_AVHD_DESC)
                          : window_word(m.kbd_desc, m.kbd_desc_len, off - AI_AVHD_DESC);
    if (off >= AI_AVHD_REPORT)
        return m.count ? window_word(m.q[m.head].data, m.q[m.head].len, off - AI_AVHD_REPORT) : 0;
    return 0;
}

void WRITE_REGISTER_ULONG(volatile ULONG *addr, ULONG value)
{
    ULONG off = (ULONG)((volatile UCHAR *)addr - g_page);
    if (off == AI_AVHD_IRQ_ACK) {
        m.irq_pending = FALSE;
        m.irq_acks++;
    } else if (off == AI_AVHD_DESC_SEL) {
        m.desc_sel = value ? 1u : 0u;
    } else if (off == AI_AVHD_RX_POP && m.count) {
        m.head = (m.head + 1u) % 8u;
        m.count--;
    }
}

/* ---------------- WDK / VHF stand-ins ---------------- */

static AI_DEVICE_CONTEXT g_ctx;
static BOOLEAN g_timer_armed, g_work_queued;
static struct { int created; UCHAR desc[512]; USHORT desc_len; UCHAR last[1024]; ULONG last_len; int submits; } g_vhf[2];

AI_DEVICE_CONTEXT *AiGetDeviceContext(WDFOBJECT o) { (void)o; return &g_ctx; }
void RtlZeroMemory(void *p, size_t n) { memset(p, 0, n); }
void RtlCopyMemory(void *d, const void *s, size_t n) { memcpy(d, s, n); }
LONG64 InterlockedIncrement64(volatile LONG64 *p) { return ++*p; }
LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER *f)
{
    static LONGLONG t;
    LARGE_INTEGER r;
    if (f) f->QuadPart = 1000000;
    r.QuadPart = (t += 8000);
    return r;
}
void KeInitializeSpinLock(KSPIN_LOCK *l) { *l = 0; }
void KeAcquireSpinLock(KSPIN_LOCK *l, KIRQL *i) { (void)l; *i = 0; }
void KeReleaseSpinLock(KSPIN_LOCK *l, KIRQL i) { (void)l; (void)i; }
void WdfWaitLockAcquire(WDFWAITLOCK l, void *t) { (void)l; (void)t; }
void WdfWaitLockRelease(WDFWAITLOCK l) { (void)l; }
BOOLEAN WdfTimerStart(WDFTIMER t, LONGLONG d) { (void)t; (void)d; g_timer_armed = TRUE; return TRUE; }
BOOLEAN WdfTimerStop(WDFTIMER t, BOOLEAN w) { (void)t; (void)w; g_timer_armed = FALSE; return TRUE; }
WDFOBJECT WdfTimerGetParentObject(WDFTIMER t) { (void)t; return NULL; }
WDFDEVICE WdfInterruptGetDevice(WDFINTERRUPT i) { (void)i; return NULL; }
BOOLEAN WdfInterruptQueueWorkItemForIsr(WDFINTERRUPT i) { (void)i; g_work_queued = TRUE; return TRUE; }
PDEVICE_OBJECT WdfDeviceWdmGetDeviceObject(WDFDEVICE d) { (void)d; return NULL; }

static VHF_CONFIG g_pending_cfg;
void VHF_CONFIG_INIT(VHF_CONFIG *c, PDEVICE_OBJECT d, USHORT len, PUCHAR desc)
{
    (void)d;
    memset(c, 0, sizeof(*c));
    g_pending_cfg.VendorID = len;            /* stash length */
    memcpy(g_vhf[1].desc, desc, len < 512 ? len : 512); /* scratch copy */
    g_vhf[1].desc_len = len;
}
NTSTATUS VhfCreate(VHF_CONFIG *c, VHFHANDLE *h)
{
    /* the trackpad VHF registers feature callbacks, the keyboard does not */
    int idx = c->EvtVhfAsyncOperationGetFeature ? 1 : 0;
    if (idx == 0) {
        memcpy(g_vhf[0].desc, g_vhf[1].desc, g_vhf[1].desc_len);
        g_vhf[0].desc_len = g_vhf[1].desc_len;
    }
    g_vhf[idx].created++;
    *h = (VHFHANDLE)(uintptr_t)(idx + 1);
    return STATUS_SUCCESS;
}
NTSTATUS VhfStart(VHFHANDLE h) { (void)h; return STATUS_SUCCESS; }
void VhfDelete(VHFHANDLE h, BOOLEAN w) { (void)h; (void)w; }
NTSTATUS VhfReadReportSubmit(VHFHANDLE h, PHID_XFER_PACKET p)
{
    int idx = (int)(uintptr_t)h - 1;
    memcpy(g_vhf[idx].last, p->reportBuffer, p->reportBufferLen);
    g_vhf[idx].last_len = p->reportBufferLen;
    g_vhf[idx].submits++;
    return STATUS_SUCCESS;
}
NTSTATUS VhfAsyncOperationComplete(VHFOPERATIONHANDLE h, NTSTATUS s) { (void)h; (void)s; return STATUS_SUCCESS; }
VOID AiDiagnosticsRecordDescriptor(PAI_DEVICE_CONTEXT c, const struct ai_descriptor_slot *d) { (void)c; (void)d; }
VOID AiDiagnosticsPublish(PAI_DEVICE_CONTEXT c) { (void)c; }

/* ---------------- driver event helpers ---------------- */

static void tick(void)
{
    assert(g_timer_armed);
    g_timer_armed = FALSE;
    AiTrackpadInitTimer(NULL);
}

static void irq(void)
{
    /* m1n1 vhid_kick: inject only when queue non-empty and not pending */
    if (m.irq_pending || !m.count)
        return;
    m.irq_pending = TRUE;
    g_work_queued = FALSE;
    assert(AiInputInterruptIsr(NULL, 0));
    assert(g_work_queued);
    AiTransportWorker(NULL, NULL);
}

int main(void)
{
    static const UCHAR kbd_desc[] = {
        0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
        0x85, 0x01, 0x75, 0x08, 0x95, 0x08, 0x81, 0x02, 0xc0,
    };
    /* J313 feature report 0xd9 body: 128.40 mm x 80.00 mm, x -3000..3200, y -1992..2100 */
    static const UCHAR dims[16] = {
        0xc8, 0x32, 0x00, 0x00, 0x40, 0x1f, 0x00, 0x00,
        0x48, 0xf4, 0x30, 0xf8, 0x80, 0x0c, 0x34, 0x08,
    };
    UCHAR mtp[256];
    ULONG mtp_len;

    memset(&m, 0, sizeof(m));
    memcpy(m.kbd_desc, kbd_desc, sizeof(kbd_desc));
    m.kbd_desc_len = sizeof(kbd_desc);
    m.mt_desc[0] = 0x06; m.mt_desc_len = 4;
    memcpy(m.dims, dims, sizeof(dims));

    g_ctx.Device = (WDFDEVICE)(uintptr_t)1;
    g_ctx.AvhdRegisters = g_page;
    g_ctx.ResourcesValidated = TRUE;
    g_ctx.PublishKeyboard = TRUE;
    g_ctx.PublishTrackpad = TRUE;
    g_ctx.FrontendLock = (WDFWAITLOCK)1;
    g_ctx.TransportLock = (WDFWAITLOCK)1;

    /* 1. m1n1 has nothing ready yet: nothing is published. */
    m.status = AI_AVHD_STATUS_MTP_UP;
    assert(AiTransportStart(&g_ctx) == STATUS_SUCCESS);
    assert(g_timer_armed && !g_vhf[0].created && !g_vhf[1].created);
    tick();
    assert(!g_vhf[0].created);

    /* 2. keyboard ready -> keyboard VHF built from the MTP descriptor. */
    m.status |= AI_AVHD_STATUS_KEYBOARD;
    tick();
    assert(g_vhf[0].created == 1);
    assert(g_vhf[0].desc_len == sizeof(kbd_desc) && !memcmp(g_vhf[0].desc, kbd_desc, sizeof(kbd_desc)));
    assert(g_ctx.KeyboardVhfState == AiVhfRunning);
    assert(g_ctx.TrackpadVhfState != AiVhfRunning);

    /* 3. keyboard report: short one is zero-padded to the descriptor's 9 bytes. */
    {
        static const UCHAR key_a[] = {0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00};
        model_push(AI_AVHD_IFACE_KEYBOARD, key_a, sizeof(key_a));
        irq();
        assert(g_vhf[0].submits == 1 && g_vhf[0].last_len == 9);
        assert(g_vhf[0].last[3] == 0x04 && g_vhf[0].last[7] == 0 && g_vhf[0].last[8] == 0);
        assert(m.count == 0 && !m.irq_pending);
    }

    /* 4. trackpad firmware up + dims -> Precision Touchpad VHF. */
    m.status |= AI_AVHD_STATUS_TRACKPAD | AI_AVHD_STATUS_DIMS;
    tick();
    assert(g_ctx.TrackpadInit.phase == AI_TRACKPAD_INIT_READY);
    assert(g_ctx.TrackpadAxisContract.valid);
    assert(g_vhf[1].created == 1 && g_ctx.TrackpadVhfState == AiVhfRunning);

    /* 5. Windows' PTP class driver switches the device to PTP mode (input mode 3)... */
    {
        UCHAR mode[AI_PTP_INPUT_MODE_REPORT_SIZE] = {AI_PTP_REPORT_INPUT_MODE, 3};
        HID_XFER_PACKET packet = {mode, sizeof(mode), AI_PTP_REPORT_INPUT_MODE};
        AiTrackpadVhfSetFeature(&g_ctx, NULL, NULL, &packet);
        assert(g_ctx.TrackpadVhf.Features.input_mode == 3u);
    }
    /* ...then MTP 0x75 frames (J313 fixtures minus the SPI prefix) -> PTP reports. */
    mtp_len = sizeof(j313_trackpad_one_contact_x) - AI_MTP_SPI_PREFIX_SIZE;
    memcpy(mtp, j313_trackpad_one_contact_x + AI_MTP_SPI_PREFIX_SIZE, mtp_len);
    mtp[0] = AI_MTP_TRACKPAD_REPORT_ID;
    for (int i = 0; i < 3; i++) {
        model_push(AI_AVHD_IFACE_TRACKPAD, mtp, mtp_len);
        irq();
    }
    assert(g_ctx.Diagnostics.TrackpadReportDecodedCount == 3);
    assert(g_vhf[1].submits >= 1);
    assert(g_vhf[1].last_len == AI_PTP_INPUT_REPORT_SIZE && g_vhf[1].last[0] == AI_PTP_REPORT_INPUT);

    /* 6. non-touch multi-touch report and oversize garbage are dropped safely. */
    {
        static const UCHAR status_report[] = {0x80, 0x01};
        int before = g_vhf[1].submits;
        model_push(AI_AVHD_IFACE_TRACKPAD, status_report, sizeof(status_report));
        model_push(AI_AVHD_IFACE_KEYBOARD, mtp, 4);
        m.q[(m.head + m.count - 1u) % 8u].len = 4000;   /* exceeds the report window */
        irq();
        assert(m.count == 0 && g_vhf[1].submits == before);
        assert(g_ctx.Diagnostics.PacketCrcFailureCount == 1);
    }

    /* 7. lost interrupt: the poll timer drains the queue and re-arms. */
    {
        int before = g_vhf[0].submits;
        static const UCHAR key_up[9] = {0x01};
        model_push(AI_AVHD_IFACE_KEYBOARD, key_up, sizeof(key_up));
        m.irq_pending = TRUE;          /* injected, but Windows never saw it */
        tick();
        assert(m.count == 0 && !m.irq_pending && g_vhf[0].submits == before + 1);
        assert(g_timer_armed);
    }

    /* 8. stop: timer is not re-armed, VHFs torn down. */
    AiTransportStop(&g_ctx);
    assert(!g_timer_armed);
    assert(g_ctx.KeyboardVhfState == AiVhfAbsent && g_ctx.TrackpadVhfState == AiVhfAbsent);

    printf("avhd_sim: all tests passed (kbd submits %d, ptp submits %d, irq acks %lu)\n",
           g_vhf[0].submits, g_vhf[1].submits, (unsigned long)m.irq_acks);
    return 0;
}

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
//
// Durable event queue: RAM ring (default). EFFS-STD path is retained but unused.
// ACK must succeed only after QueueAppend returns true.

#include "gateway_queue.h"
#include "gateway_effs.h"
#include "gateway_serializer.h"

#include <cstdio>
#include <cstring>

#include <file/fsf.h>
#include <file/fwerr.h>

#ifndef FS_NO_ERROR
#define FS_NO_ERROR FS_NOERR
#endif

namespace gateway {
namespace {

constexpr uint32_t kRecMagic = 0x31305147u; // "GQ01" LE
constexpr uint32_t kMetaMagic = 0x544D5147u; // "GQMT" LE
constexpr uint8_t kFlagPending = 0;
constexpr uint8_t kFlagCommitted = 1;
constexpr uint8_t kFlagQuarantined = 2;
constexpr size_t kRecHeaderSize = 24; // magic+flags+pad+id+len+crc
enum { kRamFallbackSlots = 128 };

struct IndexEntry {
    bool used{false};
    uint8_t flags{kFlagPending};
    uint16_t seg_num{0};
    uint32_t file_offset{0};
    uint32_t payload_len{0};
    uint64_t event_id{0};
    char quarantine_reason[64]{};
};

struct RamSlot {
    bool used{false};
    bool committed{false};
    bool quarantined{false};
    uint64_t event_id{0};
    uint8_t blob[kMaxSerializedEvent]{};
    size_t blob_len{0};
    uint32_t crc{0};
    char quarantine_reason[64]{};
};

struct MetaFile {
    uint32_t magic{kMetaMagic};
    uint16_t version{1};
    uint16_t reserved{0};
    uint64_t next_event_id{1};
    uint32_t active_seg{1};
    uint32_t bytes_used{0};
};

bool g_inited = false;
bool g_use_effs = false;
uint64_t g_next_id = 1;
uint32_t g_full_count = 0;
uint32_t g_quarantine_total = 0;
char g_last_quarantine_reason[64]{};
uint32_t g_active_seg = 1;
uint32_t g_bytes_used = 0;
uint32_t g_seg_bytes[256]{}; // approximate size per segment number (1-based index use low)
IndexEntry g_index[kMaxQueueRecords]{};
RamSlot g_ram[kRamFallbackSlots]{};

void SetErr(char *err, size_t errCap, const char *msg)
{
    if (!err || errCap < 2) {
        return;
    }
    if (!msg) {
        err[0] = '\0';
        return;
    }
    strncpy(err, msg, errCap - 1);
    err[errCap - 1] = '\0';
}

void W32(uint8_t *p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void W64(uint8_t *p, uint64_t v)
{
    W32(p, static_cast<uint32_t>(v & 0xFFFFFFFFu));
    W32(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

uint32_t R32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t R64(const uint8_t *p)
{
    return static_cast<uint64_t>(R32(p)) | (static_cast<uint64_t>(R32(p + 4)) << 32);
}

void SegPath(char *out, size_t cap, uint32_t seg)
{
    sniprintf(out, cap, "SQLQUEUE/S%07lu.DAT", static_cast<unsigned long>(seg));
}

const char *MetaPath()
{
    return "SQLQUEUE/META.DAT";
}

bool WriteMetaLocked()
{
    MetaFile m{};
    m.magic = kMetaMagic;
    m.version = 1;
    m.next_event_id = g_next_id;
    m.active_seg = g_active_seg ? g_active_seg : 1;
    m.bytes_used = g_bytes_used;

    FS_FILE *fp = fs_open(MetaPath(), "w+");
    if (!fp) {
        return false;
    }
    const int n = fs_write(&m, 1, sizeof(m), fp);
    fs_flush(fp);
    fs_close(fp);
    return n == static_cast<int>(sizeof(m));
}

bool ReadMetaLocked()
{
    FS_FILE *fp = fs_open(MetaPath(), "r");
    if (!fp) {
        g_next_id = 1;
        g_active_seg = 1;
        g_bytes_used = 0;
        return WriteMetaLocked();
    }
    MetaFile m{};
    const int n = fs_read(&m, 1, sizeof(m), fp);
    fs_close(fp);
    if (n != static_cast<int>(sizeof(m)) || m.magic != kMetaMagic || m.version != 1) {
        g_next_id = 1;
        g_active_seg = 1;
        g_bytes_used = 0;
        return WriteMetaLocked();
    }
    g_next_id = m.next_event_id ? m.next_event_id : 1;
    g_active_seg = m.active_seg ? m.active_seg : 1;
    g_bytes_used = m.bytes_used;
    return true;
}

int FindFreeIndex()
{
    for (int i = 0; i < kMaxQueueRecords; ++i) {
        if (!g_index[i].used) {
            return i;
        }
    }
    return -1;
}

int FindIndexByEventId(uint64_t eventId)
{
    for (int i = 0; i < kMaxQueueRecords; ++i) {
        if (g_index[i].used && g_index[i].event_id == eventId) {
            return i;
        }
    }
    return -1;
}

uint32_t SegBytesGet(uint32_t seg)
{
    if (seg == 0 || seg >= 256) {
        return 0;
    }
    return g_seg_bytes[seg];
}

void SegBytesSet(uint32_t seg, uint32_t bytes)
{
    if (seg > 0 && seg < 256) {
        g_seg_bytes[seg] = bytes;
    }
}

bool ScanSegmentLocked(uint32_t seg)
{
    char path[32]{};
    SegPath(path, sizeof(path), seg);
    FS_FILE *fp = fs_open(path, "r");
    if (!fp) {
        return true; // missing is OK
    }

    uint32_t offset = 0;
    uint32_t liveBytes = 0;
    bool anyLive = false;

    while (true) {
        uint8_t hdr[kRecHeaderSize]{};
        const int nr = fs_read(hdr, 1, kRecHeaderSize, fp);
        if (nr == 0) {
            break;
        }
        if (nr != static_cast<int>(kRecHeaderSize)) {
            break; // truncate incomplete tail
        }
        if (R32(hdr) != kRecMagic) {
            break;
        }
        const uint8_t flags = hdr[4];
        const uint64_t eventId = R64(hdr + 8);
        const uint32_t payloadLen = R32(hdr + 16);
        const uint32_t payloadCrc = R32(hdr + 20);
        if (payloadLen == 0 || payloadLen > kMaxSerializedEvent) {
            break;
        }

        uint8_t payload[kMaxSerializedEvent]{};
        const int np = fs_read(payload, 1, payloadLen, fp);
        if (np != static_cast<int>(payloadLen)) {
            break;
        }
        const uint32_t recSize = static_cast<uint32_t>(kRecHeaderSize + payloadLen);

        if (Crc32(payload, payloadLen) != payloadCrc) {
            // Skip corrupt payload but keep scanning if possible
            offset += recSize;
            continue;
        }

        if (flags == kFlagPending || flags == kFlagQuarantined) {
            const int idx = FindFreeIndex();
            if (idx < 0) {
                fs_close(fp);
                return false;
            }
            IndexEntry &e = g_index[idx];
            e = IndexEntry{};
            e.used = true;
            e.flags = flags;
            e.seg_num = static_cast<uint16_t>(seg);
            e.file_offset = offset;
            e.payload_len = payloadLen;
            e.event_id = eventId;
            liveBytes += recSize;
            anyLive = true;
        }

        offset += recSize;
    }

    fs_close(fp);
    SegBytesSet(seg, offset);
    if (!anyLive && offset > 0) {
        // Fully committed/empty of live records — reclaim.
        fs_delete(path);
        SegBytesSet(seg, 0);
    } else if (anyLive) {
        (void)liveBytes;
    }
    return true;
}

bool RecoverEffsLocked()
{
    memset(g_index, 0, sizeof(g_index));
    memset(g_seg_bytes, 0, sizeof(g_seg_bytes));
    if (!ReadMetaLocked()) {
        return false;
    }

    // Scan segments 1..active_seg (+ a few ahead in case of crash mid-rotate).
    const uint32_t maxScan = g_active_seg + 2;
    for (uint32_t seg = 1; seg <= maxScan && seg < 256; ++seg) {
        if (!ScanSegmentLocked(seg)) {
            return false;
        }
    }

    // Recompute bytes_used from live index.
    g_bytes_used = 0;
    for (int i = 0; i < kMaxQueueRecords; ++i) {
        if (g_index[i].used) {
            g_bytes_used += static_cast<uint32_t>(kRecHeaderSize + g_index[i].payload_len);
        }
    }
    WriteMetaLocked();
    return true;
}

bool RewriteFlagsLocked(uint16_t seg, uint32_t fileOffset, uint8_t flags)
{
    char path[32]{};
    SegPath(path, sizeof(path), seg);
    FS_FILE *fp = fs_open(path, "r+");
    if (!fp) {
        return false;
    }
    if (fs_seek(fp, static_cast<long>(fileOffset + 4), FS_SEEK_SET) != FS_NO_ERROR) {
        fs_close(fp);
        return false;
    }
    const int n = fs_write(&flags, 1, 1, fp);
    fs_flush(fp);
    fs_close(fp);
    return n == 1;
}

bool MaybeDeleteSegmentLocked(uint16_t seg)
{
    for (int i = 0; i < kMaxQueueRecords; ++i) {
        if (g_index[i].used && g_index[i].seg_num == seg) {
            return true; // still live entries
        }
    }
    char path[32]{};
    SegPath(path, sizeof(path), seg);
    fs_delete(path);
    SegBytesSet(seg, 0);
    return true;
}

bool EffsAppendLocked(const CapturedEvent &ev, char *err, size_t errCap)
{
    const int idx = FindFreeIndex();
    if (idx < 0) {
        ++g_full_count;
        SetErr(err, errCap, "Queue index full");
        return false;
    }

    uint8_t blob[kMaxSerializedEvent]{};
    size_t len = 0;
    uint32_t crc = 0;
    if (!SerializeEvent(ev, blob, sizeof(blob), len, crc)) {
        SetErr(err, errCap, "Serialize failed");
        return false;
    }

    const uint32_t recSize = static_cast<uint32_t>(kRecHeaderSize + len);
    if (g_bytes_used + recSize > kQueueBudgetBytesDefault) {
        ++g_full_count;
        SetErr(err, errCap, "Queue budget full");
        return false;
    }

    if (g_active_seg == 0) {
        g_active_seg = 1;
    }
    if (SegBytesGet(g_active_seg) + recSize > kQueueSegmentBytes) {
        ++g_active_seg;
        if (g_active_seg >= 256) {
            ++g_full_count;
            SetErr(err, errCap, "Queue segment limit");
            return false;
        }
    }

    char path[32]{};
    SegPath(path, sizeof(path), g_active_seg);
    const uint32_t fileOffset = SegBytesGet(g_active_seg);

    // Open for append; create if missing.
    FS_FILE *fp = fs_open(path, fileOffset == 0 ? "w+" : "r+");
    if (!fp) {
        SetErr(err, errCap, "Segment open failed");
        return false;
    }
    if (fileOffset > 0) {
        fs_seek(fp, 0, FS_SEEK_END);
    }

    uint8_t hdr[kRecHeaderSize]{};
    W32(hdr, kRecMagic);
    hdr[4] = kFlagPending;
    hdr[5] = hdr[6] = hdr[7] = 0;
    W64(hdr + 8, ev.event_id);
    W32(hdr + 16, static_cast<uint32_t>(len));
    W32(hdr + 20, crc);

    if (fs_write(hdr, 1, kRecHeaderSize, fp) != static_cast<int>(kRecHeaderSize) ||
        fs_write(blob, 1, len, fp) != static_cast<int>(len)) {
        fs_close(fp);
        SetErr(err, errCap, "Segment write failed");
        return false;
    }
    fs_flush(fp);
    fs_close(fp);

    SegBytesSet(g_active_seg, fileOffset + recSize);
    g_bytes_used += recSize;

    IndexEntry &e = g_index[idx];
    e = IndexEntry{};
    e.used = true;
    e.flags = kFlagPending;
    e.seg_num = static_cast<uint16_t>(g_active_seg);
    e.file_offset = fileOffset;
    e.payload_len = static_cast<uint32_t>(len);
    e.event_id = ev.event_id;

    WriteMetaLocked();
    return true;
}

bool EffsPeekLocked(CapturedEvent &ev, uint32_t &slotOut)
{
    for (uint32_t i = 0; i < kMaxQueueRecords; ++i) {
        IndexEntry &e = g_index[i];
        if (!e.used || e.flags != kFlagPending) {
            continue;
        }
        char path[32]{};
        SegPath(path, sizeof(path), e.seg_num);
        FS_FILE *fp = fs_open(path, "r");
        if (!fp) {
            e.flags = kFlagQuarantined;
            strncpy(e.quarantine_reason, "Missing segment", sizeof(e.quarantine_reason) - 1);
            continue;
        }
        if (fs_seek(fp, static_cast<long>(e.file_offset + kRecHeaderSize), FS_SEEK_SET) != FS_NO_ERROR) {
            fs_close(fp);
            e.flags = kFlagQuarantined;
            strncpy(e.quarantine_reason, "Seek failed", sizeof(e.quarantine_reason) - 1);
            continue;
        }
        uint8_t payload[kMaxSerializedEvent]{};
        const int n = fs_read(payload, 1, e.payload_len, fp);
        fs_close(fp);
        if (n != static_cast<int>(e.payload_len) || !DeserializeEvent(payload, e.payload_len, ev)) {
            e.flags = kFlagQuarantined;
            strncpy(e.quarantine_reason, "Deserialize failed", sizeof(e.quarantine_reason) - 1);
            RewriteFlagsLocked(e.seg_num, e.file_offset, kFlagQuarantined);
            continue;
        }
        slotOut = i;
        return true;
    }
    return false;
}

bool EffsMarkCommittedLocked(uint64_t eventId)
{
    const int idx = FindIndexByEventId(eventId);
    if (idx < 0) {
        return false;
    }
    IndexEntry &e = g_index[idx];
    const uint16_t seg = e.seg_num;
    const uint32_t recSize = static_cast<uint32_t>(kRecHeaderSize + e.payload_len);
    RewriteFlagsLocked(e.seg_num, e.file_offset, kFlagCommitted);
    e.used = false;
    if (g_bytes_used >= recSize) {
        g_bytes_used -= recSize;
    } else {
        g_bytes_used = 0;
    }
    MaybeDeleteSegmentLocked(seg);
    WriteMetaLocked();
    return true;
}

bool EffsQuarantineLocked(uint64_t eventId, const char *reason)
{
    const int idx = FindIndexByEventId(eventId);
    if (idx < 0) {
        return false;
    }
    IndexEntry &e = g_index[idx];
    e.flags = kFlagQuarantined;
    if (reason) {
        strncpy(e.quarantine_reason, reason, sizeof(e.quarantine_reason) - 1);
    }
    RewriteFlagsLocked(e.seg_num, e.file_offset, kFlagQuarantined);
    return true;
}

// ----- RAM fallback -----

bool RamAppend(const CapturedEvent &ev, char *err, size_t errCap)
{
    int freeIdx = -1;
    for (int i = 0; i < kRamFallbackSlots; ++i) {
        if (!g_ram[i].used) {
            freeIdx = i;
            break;
        }
    }
    if (freeIdx < 0) {
        ++g_full_count;
        SetErr(err, errCap, "Queue full");
        return false;
    }

    uint8_t blob[kMaxSerializedEvent]{};
    size_t len = 0;
    uint32_t crc = 0;
    if (!SerializeEvent(ev, blob, sizeof(blob), len, crc)) {
        SetErr(err, errCap, "Serialize failed");
        return false;
    }

    RamSlot &s = g_ram[freeIdx];
    s = RamSlot{};
    s.used = true;
    s.event_id = ev.event_id;
    s.blob_len = len;
    s.crc = crc;
    memcpy(s.blob, blob, len);
    return true;
}

bool RamPeek(CapturedEvent &ev, uint32_t &slotOut)
{
    for (uint32_t i = 0; i < kRamFallbackSlots; ++i) {
        RamSlot &s = g_ram[i];
        if (s.used && !s.committed && !s.quarantined) {
            if (Crc32(s.blob, s.blob_len) != s.crc) {
                // Reclaim the slot — quarantined records must not permanently fill the ring.
                s.quarantined = true;
                s.used = false;
                ++g_quarantine_total;
                strncpy(s.quarantine_reason, "CRC mismatch", sizeof(s.quarantine_reason) - 1);
                strncpy(g_last_quarantine_reason, "CRC mismatch", sizeof(g_last_quarantine_reason) - 1);
                continue;
            }
            if (!DeserializeEvent(s.blob, s.blob_len, ev)) {
                s.quarantined = true;
                s.used = false;
                ++g_quarantine_total;
                strncpy(s.quarantine_reason, "Deserialize failed", sizeof(s.quarantine_reason) - 1);
                strncpy(g_last_quarantine_reason, "Deserialize failed",
                        sizeof(g_last_quarantine_reason) - 1);
                continue;
            }
            slotOut = i;
            return true;
        }
    }
    return false;
}

} // namespace

bool QueueInit()
{
    memset(g_index, 0, sizeof(g_index));
    memset(g_ram, 0, sizeof(g_ram));
    memset(g_seg_bytes, 0, sizeof(g_seg_bytes));
    g_next_id = 1;
    g_full_count = 0;
    g_quarantine_total = 0;
    g_last_quarantine_reason[0] = '\0';
    g_active_seg = 1;
    g_bytes_used = 0;
    // Prefer RAM ring for soak / low-latency ACK. EFFS durable path remains in-tree
    // but is not selected here.
    g_use_effs = false;
    g_inited = true;
    return true;
}

void QueueShutdown()
{
    g_inited = false;
    g_use_effs = false;
}

uint64_t QueueAllocateEventId()
{
    if (!g_inited) {
        return g_next_id++;
    }
    if (g_use_effs) {
        EffsLock();
        const uint64_t id = g_next_id++;
        WriteMetaLocked();
        EffsUnlock();
        return id;
    }
    return g_next_id++;
}

bool QueueAppend(const CapturedEvent &ev, char *err, size_t errCap)
{
    if (!g_inited) {
        SetErr(err, errCap, "Queue not initialized");
        return false;
    }
    if (g_use_effs) {
        EffsLock();
        const bool ok = EffsAppendLocked(ev, err, errCap);
        EffsUnlock();
        return ok;
    }
    return RamAppend(ev, err, errCap);
}

bool QueuePeekPending(CapturedEvent &ev, uint32_t &slotOut)
{
    if (!g_inited) {
        return false;
    }
    if (g_use_effs) {
        EffsLock();
        const bool ok = EffsPeekLocked(ev, slotOut);
        EffsUnlock();
        return ok;
    }
    return RamPeek(ev, slotOut);
}

bool QueueMarkCommitted(uint64_t eventId)
{
    if (!g_inited) {
        return false;
    }
    if (g_use_effs) {
        EffsLock();
        const bool ok = EffsMarkCommittedLocked(eventId);
        EffsUnlock();
        return ok;
    }
    for (int i = 0; i < kRamFallbackSlots; ++i) {
        if (g_ram[i].used && g_ram[i].event_id == eventId) {
            g_ram[i].committed = true;
            g_ram[i].used = false;
            return true;
        }
    }
    return false;
}

bool QueueQuarantine(uint64_t eventId, const char *reason)
{
    if (!g_inited) {
        return false;
    }
    if (g_use_effs) {
        EffsLock();
        const bool ok = EffsQuarantineLocked(eventId, reason);
        EffsUnlock();
        return ok;
    }
    for (int i = 0; i < kRamFallbackSlots; ++i) {
        if (g_ram[i].used && g_ram[i].event_id == eventId) {
            // Drop the payload and free the slot. Keeping used=true permanently
            // filled the 16/32-slot ring after soak-test SQL failures → no ACK.
            if (reason) {
                strncpy(g_ram[i].quarantine_reason, reason, sizeof(g_ram[i].quarantine_reason) - 1);
                strncpy(g_last_quarantine_reason, reason, sizeof(g_last_quarantine_reason) - 1);
            } else {
                g_last_quarantine_reason[0] = '\0';
            }
            g_ram[i].quarantined = false;
            g_ram[i].committed = false;
            g_ram[i].used = false;
            g_ram[i].blob_len = 0;
            ++g_quarantine_total;
            return true;
        }
    }
    return false;
}

QueueStats QueueGetStats()
{
    QueueStats st{};
    st.bytes_budget = kQueueBudgetBytesDefault;
    st.next_event_id = g_next_id;
    st.dropped_or_full = g_full_count;
    st.backend = g_use_effs ? QueueBackend::Effs : QueueBackend::Ram;
    st.pending_capacity = g_use_effs ? kMaxQueueRecords : kRamFallbackSlots;

    if (g_use_effs) {
        st.bytes_used = g_bytes_used;
        for (int i = 0; i < kMaxQueueRecords; ++i) {
            if (!g_index[i].used) {
                continue;
            }
            ++st.record_count;
            if (g_index[i].flags == kFlagPending) {
                ++st.pending;
            } else if (g_index[i].flags == kFlagQuarantined) {
                ++st.quarantined;
            } else if (g_index[i].flags == kFlagCommitted) {
                ++st.committed;
            }
        }
    } else {
        st.bytes_budget = static_cast<uint32_t>(kRamFallbackSlots) * kMaxSerializedEvent;
        for (int i = 0; i < kRamFallbackSlots; ++i) {
            if (!g_ram[i].used) {
                continue;
            }
            ++st.record_count;
            st.bytes_used += static_cast<uint32_t>(g_ram[i].blob_len);
            if (!g_ram[i].committed && !g_ram[i].quarantined) {
                ++st.pending;
            }
        }
        st.quarantined = g_quarantine_total;
        st.committed = 0; // committed slots are freed immediately on RAM path
    }

    if (g_use_effs) {
        const uint32_t pct = st.bytes_budget ? (st.bytes_used * 100u / st.bytes_budget) : 0;
        if (st.pending >= kMaxQueueRecords || st.bytes_used >= st.bytes_budget) {
            st.pressure = QueuePressure::Full;
        } else if (pct >= 90 || st.pending > (kMaxQueueRecords * 9) / 10) {
            st.pressure = QueuePressure::Critical;
        } else if (pct >= 75 || st.pending > (kMaxQueueRecords * 3) / 4) {
            st.pressure = QueuePressure::Warning;
        } else {
            st.pressure = QueuePressure::Normal;
        }
    } else {
        // Pressure vs RAM slot capacity (not the EFFS record budget).
        if (st.pending >= kRamFallbackSlots) {
            st.pressure = QueuePressure::Full;
        } else if (st.pending > (kRamFallbackSlots * 9) / 10) {
            st.pressure = QueuePressure::Critical;
        } else if (st.pending > (kRamFallbackSlots * 3) / 4) {
            st.pressure = QueuePressure::Warning;
        } else {
            st.pressure = QueuePressure::Normal;
        }
    }
    return st;
}

} // namespace gateway

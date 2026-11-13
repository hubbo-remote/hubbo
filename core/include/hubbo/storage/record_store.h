#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hubbo/domain/error.h"
#include "hubbo/storage/storage.h"

// The record store: the one way durable state is saved (config, active
// activity, secrets). docs/design/persistence-and-contracts.md, section 1.
//
// A record is a payload saved with a generation. Each record lives in two
// slots; a save writes the slot that does NOT hold the newest intact record,
// reads it back, and only then is acknowledged. If the read-back doesn't
// match, the slot is invalidated (header zeroed) before the error returns,
// so an unacknowledged save can never load later. There are no renames and
// no temp files.
//
// Load returns the intact record with the highest generation. "Intact" is
// header + CRC only: the store never judges content (owners decide whether
// a record is usable; ConfigService's Blocked state).
//
// Absence needs proof (design part 6): a record is NotFound only when no
// slot file exists or every existing slot is an invalidated one (see
// invalidatedSlot()). A slot that exists but isn't intact (bad CRC, unknown
// format, too large, empty) with no intact slot beside it is Corrupt, never
// absent, so an owner can't mistake damage for a first boot and save over
// it. Accepted cost: power lost during the very first save of a record
// leaves a torn slot alone, which loads as Corrupt (needs a reset).
//
// Each slot is either missing, unreadable (an IO error) or read. A read slot
// that isn't intact (bad magic/CRC: a torn write) is skipped when the other
// slot is intact. An unreadable
// slot is never treated as missing: load and save both fail with
// StorageFailed and nothing is written, because that slot may hold the
// newest record.
//
// Health is explicit: when the medium isn't Available, saves fail with
// StorageFailed and loads with StorageFailed (Unavailable) - nothing falls
// back to RAM. If a failed save can't even invalidate its slot, the medium
// is distrusted (Storage::distrust) and every store on it reports ReadOnly
// from then on, so no later save is acknowledged on a medium it can't trust.
// (Residual risk: if the medium fails exactly while invalidating, that
// unacknowledged record may load later; the hub then shows a storage fault
// instead of carrying on.)
//
// The store owns generations: owners ask nextGeneration() rather than
// caching one.
//
// Every platform implementation must pass core/tests/record_store_test.cpp's
// crash-injection suite.
namespace hubbo::storage {

struct Record {
    uint64_t generation = 0;
    std::string payload;
};

class RecordStore {
public:
    virtual ~RecordStore() = default;

    virtual Health health() const = 0;

    // Ok (the newest intact record), NotFound (positively absent: no slot
    // file, or only invalidated ones), Corrupt (a slot exists but none is
    // intact) or StorageFailed (medium unavailable, or any slot unreadable).
    virtual ErrorCode load(std::string_view name, Record *out) = 0;

    // The generation the next save of `name` must use: one above every
    // intact record (1 if none). StorageFailed if a slot can't be read.
    virtual ErrorCode nextGeneration(std::string_view name, uint64_t *out) = 0;

    // Saves `payload` as `generation`, which must be greater than every
    // intact record's (Internal otherwise). Ok only after the read-back
    // verified; StorageFailed otherwise, with the target slot invalidated and
    // the previous record still loading. TooLong over the store's limit.
    virtual ErrorCode save(std::string_view name, uint64_t generation, std::string_view payload) = 0;

    // Removes the record, so the next load is NotFound. Ok (also when there
    // was none) or StorageFailed (the medium isn't writable).
    virtual ErrorCode remove(std::string_view name) = 0;
};

// Slot framing, shared by all implementations (and tools such as
// `hub-sim write-record`). Little-endian header:
//   u32 magic "HBRS" | u16 format (1) | u16 reserved (0) | u64 generation |
//   u32 payload length | u32 CRC-32 (IEEE) over header bytes 0..19 + payload
constexpr uint32_t kSlotMagic = 0x53524248;  // "HBRS"
constexpr uint16_t kSlotFormat = 1;
constexpr size_t kSlotHeaderBytes = 24;

std::string encodeSlot(uint64_t generation, std::string_view payload);
// True if `bytes` is an intact slot; fills `out`.
bool decodeSlot(std::string_view bytes, Record *out);
// What a failed save leaves in its slot: exactly kSlotHeaderBytes zero
// bytes. Unambiguous: every written slot starts with kSlotMagic, so no torn
// write can produce it. The only slot content that counts as "absent".
std::string invalidatedSlot();
bool isInvalidatedSlot(std::string_view bytes);
uint32_t crc32(const uint8_t *data, size_t length, uint32_t crc = 0);

// Over Storage files "<name>.a" and "<name>.b" (hub-sim --data, LittleFS,
// MemoryStorage in tests).
class SlotRecordStore : public RecordStore {
public:
    SlotRecordStore(Storage &storage, size_t maxPayloadBytes);

    Health health() const override;
    ErrorCode load(std::string_view name, Record *out) override;
    ErrorCode nextGeneration(std::string_view name, uint64_t *out) override;
    ErrorCode save(std::string_view name, uint64_t generation, std::string_view payload) override;
    ErrorCode remove(std::string_view name) override;  // both slot files

private:
    enum class Slot : uint8_t { Missing, Invalidated, Intact, Damaged, Unreadable };
    struct Pair {
        Slot a = Slot::Missing, b = Slot::Missing;
        Record recordA, recordB;
        ErrorCode status() const;  // Ok, NotFound, Corrupt or StorageFailed (see load())
        uint64_t newest() const;   // highest intact generation, 0 if none
        bool newestIsA() const;
    };
    Slot readSlot(const std::string &file, Record *out);
    Pair readPair(std::string_view name);

    Storage &storage_;  // owns trust (Storage::distrust), shared by every store on it
    size_t maxPayload_;
};

}  // namespace hubbo::storage

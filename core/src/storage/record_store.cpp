#include "hubbo/storage/record_store.h"

#include <algorithm>
#include <cstring>

namespace hubbo::storage {

namespace {

void put16(std::string &out, uint16_t v) {
    out += static_cast<char>(v & 0xFF);
    out += static_cast<char>(v >> 8);
}

void put32(std::string &out, uint32_t v) {
    for (int i = 0; i < 4; i++) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}

void put64(std::string &out, uint64_t v) {
    for (int i = 0; i < 8; i++) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}

uint64_t get(std::string_view bytes, size_t at, size_t width) {
    uint64_t v = 0;
    for (size_t i = 0; i < width; i++) v |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[at + i])) << (8 * i);
    return v;
}

const uint8_t *bytesOf(std::string_view s) { return reinterpret_cast<const uint8_t *>(s.data()); }

std::string slotName(std::string_view record, char slot) { return std::string(record) + "." + slot; }

}  // namespace

uint32_t crc32(const uint8_t *data, size_t length, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::string encodeSlot(uint64_t generation, std::string_view payload) {
    std::string out;
    out.reserve(kSlotHeaderBytes + payload.size());
    put32(out, kSlotMagic);
    put16(out, kSlotFormat);
    put16(out, 0);
    put64(out, generation);
    put32(out, static_cast<uint32_t>(payload.size()));
    const uint32_t crc = crc32(bytesOf(payload), payload.size(), crc32(bytesOf(out), out.size()));
    put32(out, crc);
    out.append(payload.data(), payload.size());
    return out;
}

bool decodeSlot(std::string_view bytes, Record *out) {
    if (bytes.size() < kSlotHeaderBytes) return false;
    if (get(bytes, 0, 4) != kSlotMagic || get(bytes, 4, 2) != kSlotFormat) return false;
    const uint64_t length = get(bytes, 16, 4);
    if (bytes.size() != kSlotHeaderBytes + length) return false;
    const std::string_view payload = bytes.substr(kSlotHeaderBytes);
    const uint32_t crc = crc32(bytesOf(payload), payload.size(), crc32(bytesOf(bytes), 20));
    if (crc != get(bytes, 20, 4)) return false;
    const uint64_t generation = get(bytes, 8, 8);
    if (generation == 0) return false;
    out->generation = generation;
    out->payload.assign(payload.data(), payload.size());
    return true;
}

SlotRecordStore::SlotRecordStore(Storage &storage, size_t maxPayloadBytes)
    : storage_(storage), maxPayload_(maxPayloadBytes) {}

Health SlotRecordStore::health() const {
    const Health medium = storage_.health();
    return !storage_.trusted() && medium == Health::Available ? Health::ReadOnly : medium;
}

SlotRecordStore::Slot SlotRecordStore::readSlot(const std::string &file, Record *out) {
    std::string bytes;
    const ErrorCode read = storage_.read(file, &bytes, kSlotHeaderBytes + maxPayload_);
    if (read == ErrorCode::NotFound) return Slot::Missing;
    if (read == ErrorCode::TooLong) return Slot::Damaged;
    if (read != ErrorCode::Ok) return Slot::Unreadable;
    if (isInvalidatedSlot(bytes)) return Slot::Invalidated;
    return decodeSlot(bytes, out) ? Slot::Intact : Slot::Damaged;
}

SlotRecordStore::Pair SlotRecordStore::readPair(std::string_view name) {
    Pair p;
    p.a = readSlot(slotName(name, 'a'), &p.recordA);
    p.b = readSlot(slotName(name, 'b'), &p.recordB);
    return p;
}

ErrorCode SlotRecordStore::Pair::status() const {
    // "Can't read" is never "doesn't exist": an unreadable slot may hold the
    // newest record. Absence needs proof (design part 6): only missing or
    // deliberately invalidated slots. A damaged slot with no intact one
    // beside it is Corrupt; next to an intact one it's a torn write and the
    // intact one wins.
    if (a == Slot::Unreadable || b == Slot::Unreadable) return ErrorCode::StorageFailed;
    if (a == Slot::Intact || b == Slot::Intact) return ErrorCode::Ok;
    if (a == Slot::Damaged || b == Slot::Damaged) return ErrorCode::Corrupt;
    return ErrorCode::NotFound;
}

uint64_t SlotRecordStore::Pair::newest() const {
    return std::max(a == Slot::Intact ? recordA.generation : 0, b == Slot::Intact ? recordB.generation : 0);
}

bool SlotRecordStore::Pair::newestIsA() const {
    return a == Slot::Intact && (b != Slot::Intact || recordA.generation > recordB.generation);
}

ErrorCode SlotRecordStore::nextGeneration(std::string_view name, uint64_t *out) {
    if (storage_.health() == Health::Unavailable) return ErrorCode::StorageFailed;
    const Pair p = readPair(name);
    if (p.status() == ErrorCode::StorageFailed) return ErrorCode::StorageFailed;
    *out = p.newest() + 1;
    return ErrorCode::Ok;
}

ErrorCode SlotRecordStore::load(std::string_view name, Record *out) {
    if (storage_.health() == Health::Unavailable) return ErrorCode::StorageFailed;
    Pair p = readPair(name);
    const ErrorCode status = p.status();
    if (status != ErrorCode::Ok) return status;
    *out = std::move(p.newestIsA() ? p.recordA : p.recordB);
    return ErrorCode::Ok;
}

ErrorCode SlotRecordStore::remove(std::string_view name) {
    if (health() != Health::Available) return ErrorCode::StorageFailed;
    for (const char slot : {'a', 'b'}) {
        if (storage_.remove(slotName(name, slot)) != ErrorCode::Ok) return ErrorCode::StorageFailed;
    }
    return ErrorCode::Ok;
}

ErrorCode SlotRecordStore::save(std::string_view name, uint64_t generation, std::string_view payload) {
    if (health() != Health::Available) return ErrorCode::StorageFailed;
    if (payload.size() > maxPayload_) return ErrorCode::TooLong;

    const Pair p = readPair(name);
    if (p.status() == ErrorCode::StorageFailed) return ErrorCode::StorageFailed;  // never overwrite a slot we couldn't read
    if (generation <= p.newest()) return ErrorCode::Internal;
    // Write the slot that doesn't hold the newest intact record. Over a
    // Corrupt record (restore/reset of a Blocked config) that's slot a; the
    // damaged b then loses to the intact a.
    const std::string target = slotName(name, p.newestIsA() ? 'b' : 'a');

    const std::string bytes = encodeSlot(generation, payload);
    std::string readBack;
    if (storage_.write(target, bytes) == ErrorCode::Ok &&
        storage_.read(target, &readBack, kSlotHeaderBytes + maxPayload_) == ErrorCode::Ok && readBack == bytes) {
        return ErrorCode::Ok;
    }
    // Never leave an unacknowledged record that could load later. If even
    // that fails, the medium can't be trusted: no later save on it (by any
    // record store) is acknowledged.
    if (storage_.write(target, invalidatedSlot()) != ErrorCode::Ok) storage_.distrust();
    return ErrorCode::StorageFailed;
}

std::string invalidatedSlot() { return std::string(kSlotHeaderBytes, '\0'); }

bool isInvalidatedSlot(std::string_view bytes) {
    if (bytes.size() != kSlotHeaderBytes) return false;
    for (char c : bytes) {
        if (c != '\0') return false;
    }
    return true;
}

}  // namespace hubbo::storage

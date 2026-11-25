#pragma once

// Read-only diagnostic for this build's bounded-writer assertion. The local
// disassembly establishes the two 40-byte slots, metadata ready/frame/serial
// fields, registry and active index. No title instructions/data are modified.
static void DumpWriterFault(const CONTEXT& context)
{
    const uintptr_t image = (uintptr_t)GetModuleHandleA(nullptr);
    if (context.Rip != image + 0x4dcbc9d) return;
    WriterTaskSnapshot();
    auto read = [](uintptr_t address, void* out, SIZE_T bytes) {
        SIZE_T done = 0;
        return ReadProcessMemory(GetCurrentProcess(), (void*)address, out, bytes, &done) && done == bytes;
    };
    uintptr_t registry = 0;
    uint16_t count = 0;
    if (!read(image + 0x9ae9950, &registry, sizeof registry) ||
        !read(image + 0x9ae9958, &count, sizeof count) || !registry) {
        XS_LOG("WRITER registry unavailable"); return;
    }
    XS_LOG("WRITER registry=%p count=%u destination=%p requested=%#llx", (void*)registry, count, (void*)context.Rcx, (unsigned long long)context.R8);
    bool matched = false;
    for (unsigned i = 0; i < count && i < 128; ++i) {
        uintptr_t entry = 0;
        if (!read(registry + i * sizeof entry, &entry, sizeof entry) || !entry) continue;
        if (context.Rcx != entry && context.Rcx != entry + 40) continue;
        matched = true;
        unsigned char fields[0xd8]{};
        if (!read(entry, fields, sizeof fields)) { XS_LOG("WRITER entry=%p unreadable", (void*)entry); break; }
        uint32_t tag = 0, active = 0, serial = 0, consumed = 0;
        std::memcpy(&tag, fields + 0xc0, 4); std::memcpy(&active, fields + 0xc8, 4);
        std::memcpy(&serial, fields + 0xcc, 4); std::memcpy(&consumed, fields + 0xd0, 4);
        XS_LOG("WRITER entry=%p index=%u tag=%#x active=%u produced=%u consumed=%u", (void*)entry, i, tag, active, serial, consumed);
        for (unsigned slot = 0; slot < 2; ++slot) {
            const auto* data = fields + slot * 40;
            uintptr_t metadata = 0, bytes = 0;
            uint32_t capacity = 0, position = 0, high = 0;
            std::memcpy(&metadata, data, 8); std::memcpy(&bytes, data + 8, 8);
            std::memcpy(&capacity, data + 0x10, 4); std::memcpy(&position, data + 0x18, 4); std::memcpy(&high, data + 0x1c, 4);
            unsigned char status[12]{};
            const bool statusOk = metadata && read(metadata + 0x28, status, sizeof status);
            uint32_t frame = 0, sequence = 0;
            std::memcpy(&frame, status + 4, 4); std::memcpy(&sequence, status + 8, 4);
            XS_LOG("WRITER slot=%u data=%p capacity=%#x position=%#x high=%#x metadata=%p readable=%u ready=%u frame=%u sequence=%u", slot, (void*)bytes, capacity, position, high, (void*)metadata, statusOk, status[0], frame, sequence);
        }
        break;
    }
    if (!matched) XS_LOG("WRITER destination has no matching registry slot (scan limited to 128 entries)");
    uintptr_t gate = 0;
    uint32_t rawFrame = 0;
    const bool gateOk = read(image + 0xa924088, &gate, sizeof gate) && gate && read(gate + 0x1548, &rawFrame, 4);
    XS_LOG("WRITER registry_gate=%p raw_1548=%u readable=%u (consumer identity unconfirmed)", (void*)gate, rawFrame, gateOk);
}

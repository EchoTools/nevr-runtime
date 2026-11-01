// CResourceManager::FillResourceFileRequest(CSymbol64, CSymbol64, ...)
// Quest libr15.so: ReVault VA 0x26439f0 / ELF RVA 0x25439f0, binary 576.
// PC echovr.exe: VA 0x140fa1fe0 / RVA 0xfa1fe0 (ReVault-confirmed twin).
// ReVault raw parent 100319665; reconstruction node 100600170; tier: reconstructed; grade: H, assessed by revault-codex.
// Request size is 0x470. Manifest records are 0x20 bytes, sorted by signed
// (type, name); chunk and package table field meanings remain only partly named.
#include <cstddef>
#include <cstdint>

namespace q15t::r15::reconstruction {
struct FileRequest { std::byte bytes[0x470]; };
struct ResourceManager { std::byte opaque[0x28]; };
struct Ops {
    virtual void clear_file_request(FileRequest*);
    virtual std::int64_t read_manifest_count(const ResourceManager*) const;
    virtual std::byte* manifest_records(const ResourceManager*) const;
    virtual std::byte* chunk_table(const ResourceManager*) const;
    virtual std::byte* package_file_table(const ResourceManager*) const;
    virtual void format_key(char* destination, std::uint64_t name, std::uint64_t type);
    virtual void initialize_error_tls();
    virtual std::uint64_t missing_resource(std::uint64_t type, std::uint64_t name);
};

std::uint64_t FillResourceFileRequest(ResourceManager* manager, std::uint64_t type,
                                      std::uint64_t name, std::uint64_t request_arg,
                                      void* request_data, std::uint64_t file_token,
                                      std::uint64_t buffer_index, FileRequest* out, Ops& ops) {
    ops.clear_file_request(out);
    *reinterpret_cast<std::uint64_t*>(out->bytes + 0x00) = file_token;
    *reinterpret_cast<std::uint64_t*>(out->bytes + 0x08) = buffer_index;
    *reinterpret_cast<void**>(out->bytes + 0x460) = request_data;
    const auto count = ops.read_manifest_count(manager);
    auto* records = ops.manifest_records(manager);
    std::int64_t lo = 0, hi = count;
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        auto* rec = records + mid * 0x20;
        const auto rec_type = *reinterpret_cast<std::int64_t*>(rec + 0x00);
        const auto rec_name = *reinterpret_cast<std::int64_t*>(rec + 0x08);
        if (rec_type < static_cast<std::int64_t>(type) ||
            (rec_type == static_cast<std::int64_t>(type) && rec_name < static_cast<std::int64_t>(name))) {
            lo = mid + 1; continue;
        }
        if (rec_type > static_cast<std::int64_t>(type) || rec_name > static_cast<std::int64_t>(name)) {
            hi = mid; continue;
        }
        const auto chunk_index = *reinterpret_cast<std::uint32_t*>(rec + 0x10);
        const auto record_word = *reinterpret_cast<std::uint64_t*>(rec + 0x18);
        auto* chunk = ops.chunk_table(manager) + static_cast<std::size_t>(chunk_index) * 0x10;
        const auto w0 = *reinterpret_cast<std::uint64_t*>(chunk + 0x00);
        const auto w1 = *reinterpret_cast<std::uint64_t*>(chunk + 0x08);
        auto* file = ops.package_file_table(manager) + (w0 & 0xffffffffu) * 8;
        *reinterpret_cast<std::uint64_t*>(out->bytes + 0x418) = chunk_index;
        *reinterpret_cast<std::uint64_t*>(out->bytes + 0x420) = w0 >> 32;
        *reinterpret_cast<std::uint64_t*>(out->bytes + 0x428) = w1 & 0xffffffffu;
        *reinterpret_cast<std::uint64_t*>(out->bytes + 0x430) = w1 >> 32;
        *reinterpret_cast<std::uint64_t*>(out->bytes + 0x438) = *reinterpret_cast<std::uint32_t*>(rec + 0x14);
        *reinterpret_cast<std::uint64_t*>(out->bytes + 0x440) = record_word & 0xffffffffu;
        *reinterpret_cast<std::uint64_t*>(out->bytes + 0x448) = record_word & 0xffffffffu;
        *reinterpret_cast<std::uint64_t*>(out->bytes + 0x450) = record_word >> 32;
        *reinterpret_cast<std::uint64_t*>(out->bytes + 0x10) = *reinterpret_cast<std::uint64_t*>(file);
        ops.format_key(reinterpret_cast<char*>(out->bytes + 0x218), name, type); // "0x%016llx.0x%016llx", name then type
        *reinterpret_cast<std::uint64_t*>(out->bytes + 0x458) = 0;
        ops.initialize_error_tls();
        *reinterpret_cast<std::uint32_t*>(out->bytes + 0x468) = 1;
        (void)request_arg;
        return 0;
    }
    return ops.missing_resource(type, name);
}
} // namespace q15t::r15::reconstruction

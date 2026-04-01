/* SYNTHESIS -- custom tool code, not from binary */

#include "filesystem_loader.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <MinHook.h>
#else
#include <dirent.h>
#include <sys/stat.h>
/* MinHook stubs for non-Windows builds */
enum MH_STATUS { MH_OK = 0, MH_ERROR_ALREADY_INITIALIZED = 1 };
inline MH_STATUS MH_Initialize()                      { return MH_OK; }
inline MH_STATUS MH_CreateHook(void*, void*, void**)   { return MH_OK; }
inline MH_STATUS MH_EnableHook(void*)                  { return MH_OK; }
inline MH_STATUS MH_DisableHook(void*)                 { return MH_OK; }
inline MH_STATUS MH_RemoveHook(void*)                  { return MH_OK; }
inline MH_STATUS MH_Uninitialize()                     { return MH_OK; }
#endif

#include "address_registry.h"
#include "nevr_common.h"

namespace nevr::filesystem_loader {

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[NEVR.FSLOAD] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

/* ------------------------------------------------------------------ */
/* CSymbol64 hash (from echovr-reconstruction Hash.cpp @ 0x1400ce120) */
/* ------------------------------------------------------------------ */

// CRC-64 lookup table at 0x141ffc480 (256 entries).
// Polynomial 0x95AC9329AC4BC9B5, computed by CSymbol64_InitTable @ 0x1400cfcd0.
// Source: echovr-reconstruction/src/NRadEngine/Core/Hash.cpp
static constexpr uint64_t CSYMBOL64_TABLE[256] = {
    0x0000000000000000ULL, 0x2B5926535897936AULL, 0x56B24CA6B12F26D4ULL, 0x7DEB6AF5E9B8B5BEULL,
    0xAD64994D625E4DA8ULL, 0x863DBF1E3AC9DEC2ULL, 0xFBD6D5EBD3716B7CULL, 0xD08FF3B88BE6F816ULL,
    0x5AC9329AC4BC9B50ULL, 0x719014C99C2B083AULL, 0x0C7B7E3C7593BD84ULL, 0x2722586F2D042EEEULL,
    0xF7ADABD7A6E2D6F8ULL, 0xDCF48D84FE754592ULL, 0xA11FE77117CDF02CULL, 0x8A46C1224F5A6346ULL,
    0xB5926535897936A0ULL, 0x9ECB4366D1EEA5CAULL, 0xE320299338561074ULL, 0xC8790FC060C1831EULL,
    0x18F6FC78EB277B08ULL, 0x33AFDA2BB3B0E862ULL, 0x4E44B0DE5A085DDCULL, 0x651D968D029FCEB6ULL,
    0xEF5B57AF4DC5ADF0ULL, 0xC40271FC15523E9AULL, 0xB9E91B09FCEA8B24ULL, 0x92B03D5AA47D184EULL,
    0x423FCEE22F9BE058ULL, 0x6966E8B1770C7332ULL, 0x148D82449EB4C68CULL, 0x3FD4A417C62355E6ULL,
    0x6B24CA6B12F26D40ULL, 0x407DEC384A65FE2AULL, 0x3D9686CDA3DD4B94ULL, 0x16CFA09EFB4AD8FEULL,
    0xC640532670AC20E8ULL, 0xED197575283BB382ULL, 0x90F21F80C183063CULL, 0xBBAB39D399149556ULL,
    0x31EDF8F1D64EF610ULL, 0x1AB4DEA28ED9657AULL, 0x675FB4576761D0C4ULL, 0x4C0692043FF643AEULL,
    0x9C8961BCB410BBB8ULL, 0xB7D047EFEC8728D2ULL, 0xCA3B2D1A053F9D6CULL, 0xE1620B495DA80E06ULL,
    0xDEB6AF5E9B8B5BE0ULL, 0xF5EF890DC31CC88AULL, 0x8804E3F82AA47D34ULL, 0xA35DC5AB7233EE5EULL,
    0x73D23613F9D51648ULL, 0x588B1040A1428522ULL, 0x25607AB548FA309CULL, 0x0E395CE6106DA3F6ULL,
    0x847F9DC45F37C0B0ULL, 0xAF26BB9707A053DAULL, 0xD2CDD162EE18E664ULL, 0xF994F731B68F750EULL,
    0x291B04893D698D18ULL, 0x024222DA65FE1E72ULL, 0x7FA9482F8C46ABCCULL, 0x54F06E7CD4D138A6ULL,
    0xD64994D625E4DA80ULL, 0xFD10B2857D7349EAULL, 0x80FBD87094CBFC54ULL, 0xABA2FE23CC5C6F3EULL,
    0x7B2D0D9B47BA9728ULL, 0x50742BC81F2D0442ULL, 0x2D9F413DF695B1FCULL, 0x06C6676EAE022296ULL,
    0x8C80A64CE15841D0ULL, 0xA7D9801FB9CFD2BAULL, 0xDA32EAEA50776704ULL, 0xF16BCCB908E0F46EULL,
    0x21E43F0183060C78ULL, 0x0ABD1952DB919F12ULL, 0x775673A732292AACULL, 0x5C0F55F46ABEB9C6ULL,
    0x63DBF1E3AC9DEC20ULL, 0x4882D7B0F40A7F4AULL, 0x3569BD451DB2CAF4ULL, 0x1E309B164525599EULL,
    0xCEBF68AECEC3A188ULL, 0xE5E64EFD965432E2ULL, 0x980D24087FEC875CULL, 0xB354025B277B1436ULL,
    0x3912C37968217770ULL, 0x124BE52A30B6E41AULL, 0x6FA08FDFD90E51A4ULL, 0x44F9A98C8199C2CEULL,
    0x94765A340A7F3AD8ULL, 0xBF2F7C6752E8A9B2ULL, 0xC2C41692BB501C0CULL, 0xE99D30C1E3C78F66ULL,
    0xBD6D5EBD3716B7C0ULL, 0x963478EE6F8124AAULL, 0xEBDF121B86399114ULL, 0xC0863448DEAE027EULL,
    0x1009C7F05548FA68ULL, 0x3B50E1A30DDF6902ULL, 0x46BB8B56E467DCBCULL, 0x6DE2AD05BCF04FD6ULL,
    0xE7A46C27F3AA2C90ULL, 0xCCFD4A74AB3DBFFAULL, 0xB116208142850A44ULL, 0x9A4F06D21A12992EULL,
    0x4AC0F56A91F46138ULL, 0x6199D339C963F252ULL, 0x1C72B9CC20DB47ECULL, 0x372B9F9F784CD486ULL,
    0x08FF3B88BE6F8160ULL, 0x23A61DDBE6F8120AULL, 0x5E4D772E0F40A7B4ULL, 0x7514517D57D734DEULL,
    0xA59BA2C5DC31CCC8ULL, 0x8EC2849684A65FA2ULL, 0xF329EE636D1EEA1CULL, 0xD870C83035897976ULL,
    0x523609127AD31A30ULL, 0x796F2F412244895AULL, 0x048445B4CBFC3CE4ULL, 0x2FDD63E7936BAF8EULL,
    0xFF52905F188D5798ULL, 0xD40BB60C401AC4F2ULL, 0xA9E0DCF9A9A2714CULL, 0x82B9FAAAF135E226ULL,
    0xAC9329AC4BC9B500ULL, 0x87CA0FFF135E266AULL, 0xFA21650AFAE693D4ULL, 0xD1784359A27100BEULL,
    0x01F7B0E12997F8A8ULL, 0x2AAE96B271006BC2ULL, 0x5745FC4798B8DE7CULL, 0x7C1CDA14C02F4D16ULL,
    0xF65A1B368F752E50ULL, 0xDD033D65D7E2BD3AULL, 0xA0E857903E5A0884ULL, 0x8BB171C366CD9BEEULL,
    0x5B3E827BED2B63F8ULL, 0x7067A428B5BCF092ULL, 0x0D8CCEDD5C04452CULL, 0x26D5E88E0493D646ULL,
    0x19014C99C2B083A0ULL, 0x32586ACA9A2710CAULL, 0x4FB3003F739FA574ULL, 0x64EA266C2B08361EULL,
    0xB465D5D4A0EECE08ULL, 0x9F3CF387F8795D62ULL, 0xE2D7997211C1E8DCULL, 0xC98EBF2149567BB6ULL,
    0x43C87E03060C18F0ULL, 0x689158505E9B8B9AULL, 0x157A32A5B7233E24ULL, 0x3E2314F6EFB4AD4EULL,
    0xEEACE74E64525558ULL, 0xC5F5C11D3CC5C632ULL, 0xB81EABE8D57D738CULL, 0x93478DBB8DEAE0E6ULL,
    0xC7B7E3C7593BD840ULL, 0xECEEC59401AC4B2AULL, 0x9105AF61E814FE94ULL, 0xBA5C8932B0836DFEULL,
    0x6AD37A8A3B6595E8ULL, 0x418A5CD963F20682ULL, 0x3C61362C8A4AB33CULL, 0x1738107FD2DD2056ULL,
    0x9D7ED15D9D874310ULL, 0xB627F70EC510D07AULL, 0xCBCC9DFB2CA865C4ULL, 0xE095BBA8743FF6AEULL,
    0x301A4810FFD90EB8ULL, 0x1B436E43A74E9DD2ULL, 0x66A804B64EF6286CULL, 0x4DF122E51661BB06ULL,
    0x722586F2D042EEE0ULL, 0x597CA0A188D57D8AULL, 0x2497CA54616DC834ULL, 0x0FCEEC0739FA5B5EULL,
    0xDF411FBFB21CA348ULL, 0xF41839ECEA8B3022ULL, 0x89F353190333859CULL, 0xA2AA754A5BA416F6ULL,
    0x28ECB46814FE75B0ULL, 0x03B5923B4C69E6DAULL, 0x7E5EF8CEA5D15364ULL, 0x5507DE9DFD46C00EULL,
    0x85882D2576A03818ULL, 0xAED10B762E37AB72ULL, 0xD33A6183C78F1ECCULL, 0xF86347D09F188DA6ULL,
    0x7ADABD7A6E2D6F80ULL, 0x51839B2936BAFCEAULL, 0x2C68F1DCDF024954ULL, 0x0731D78F8795DA3EULL,
    0xD7BE24370C732228ULL, 0xFCE7026454E4B142ULL, 0x810C6891BD5C04FCULL, 0xAA554EC2E5CB9796ULL,
    0x20138FE0AA91F4D0ULL, 0x0B4AA9B3F20667BAULL, 0x76A1C3461BBED204ULL, 0x5DF8E5154329416EULL,
    0x8D7716ADC8CFB978ULL, 0xA62E30FE90582A12ULL, 0xDBC55A0B79E09FACULL, 0xF09C7C5821770CC6ULL,
    0xCF48D84FE7545920ULL, 0xE411FE1CBFC3CA4AULL, 0x99FA94E9567B7FF4ULL, 0xB2A3B2BA0EECEC9EULL,
    0x622C4102850A1488ULL, 0x49756751DD9D87E2ULL, 0x349E0DA43425325CULL, 0x1FC72BF76CB2A136ULL,
    0x9581EAD523E8C270ULL, 0xBED8CC867B7F511AULL, 0xC333A67392C7E4A4ULL, 0xE86A8020CA5077CEULL,
    0x38E5739841B68FD8ULL, 0x13BC55CB19211CB2ULL, 0x6E573F3EF099A90CULL, 0x450E196DA80E3A66ULL,
    0x11FE77117CDF02C0ULL, 0x3AA75142244891AAULL, 0x474C3BB7CDF02414ULL, 0x6C151DE49567B77EULL,
    0xBC9AEE5C1E814F68ULL, 0x97C3C80F4616DC02ULL, 0xEA28A2FAAFAE69BCULL, 0xC17184A9F739FAD6ULL,
    0x4B37458BB8639990ULL, 0x606E63D8E0F40AFAULL, 0x1D85092D094CBF44ULL, 0x36DC2F7E51DB2C2EULL,
    0xE653DCC6DA3DD438ULL, 0xCD0AFA9582AA4752ULL, 0xB0E190606B12F2ECULL, 0x9BB8B63333856186ULL,
    0xA46C1224F5A63460ULL, 0x8F353477AD31A70AULL, 0xF2DE5E82448912B4ULL, 0xD98778D11C1E81DEULL,
    0x09088B6997F879C8ULL, 0x2251AD3ACF6FEAA2ULL, 0x5FBAC7CF26D75F1CULL, 0x74E3E19C7E40CC76ULL,
    0xFEA520BE311AAF30ULL, 0xD5FC06ED698D3C5AULL, 0xA8176C18803589E4ULL, 0x834E4A4BD8A21A8EULL,
    0x53C1B9F35344E298ULL, 0x78989FA00BD371F2ULL, 0x0573F555E26BC44CULL, 0x2E2AD306BAFC5726ULL
};

/*
 * CSymbol64_Hash — compute the engine's 64-bit symbol hash from a string.
 * Case-insensitive. Default seed 0xFFFFFFFFFFFFFFFF.
 * Source: echovr-reconstruction/src/NRadEngine/Core/Hash.cpp @ 0x1400ce120
 */
static uint64_t CSymbol64_Hash(const char* str,
                               uint64_t seed = 0xFFFFFFFFFFFFFFFFULL) {
    if (!str || *str == '\0') return seed;
    uint64_t hash = seed;
    for (const char* p = str; *p != '\0'; ++p) {
        char c = *p;
        /* tolower for A-Z only (matches binary: (byte)(c + 0xbf) <= 0x19) */
        if (static_cast<uint8_t>(c + 0xbf) <= 0x19) c += 0x20;
        hash = static_cast<uint64_t>(c) ^ CSYMBOL64_TABLE[(hash >> 56) & 0xFF] ^ (hash << 8);
    }
    return hash;
}

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

/*
 * All mutable state is heap-allocated and accessed via raw pointers that
 * start as nullptr. This avoids C++ static constructors (std::unordered_map,
 * std::vector, std::mutex) running during DllMain/CRT init, which fails
 * under Wine when multiple MinGW DLLs compete for CRT startup locks.
 *
 * State is allocated in InstallHook() and freed in RemoveHook().
 */

/* Map from name_symbol hash -> override file info */
static std::unordered_map<uint64_t, OverrideEntry>* g_overrides = nullptr;
static uintptr_t g_base_addr = 0;
static uint64_t g_override_hits = 0;
static uint64_t g_override_errors = 0;

/*
 * Persistent override buffers.
 *
 * Resource_InitFromBuffers stores the buffer pointers into the CResource
 * struct and then calls vtable[7] (DeserializeAndUpload) which reads from
 * those pointers. The original buffers come from the archive async I/O
 * system and remain valid until the resource is unloaded. Our replacement
 * buffers must have the same lifetime — they cannot be stack or temp
 * allocations.
 *
 * We allocate override buffers with VirtualAlloc/malloc and track them
 * here so they can be freed on shutdown.
 */
static std::vector<void*>* g_allocated_buffers = nullptr;
#ifdef _WIN32
static CRITICAL_SECTION* g_buffer_cs = nullptr;
#endif

/* ------------------------------------------------------------------ */
/* Config parsing                                                      */
/* ------------------------------------------------------------------ */

LoaderConfig ParseConfig(const std::string& json_text) {
    LoaderConfig cfg;
    /* Extract "override_dir" string value */
    std::string needle = "\"override_dir\"";
    auto pos = json_text.find(needle);
    if (pos != std::string::npos) {
        pos = json_text.find(':', pos + needle.size());
        if (pos != std::string::npos) {
            auto q1 = json_text.find('"', pos + 1);
            if (q1 != std::string::npos) {
                auto q2 = json_text.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                    cfg.override_dir = json_text.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }
    }
    cfg.valid = true;
    return cfg;
}

/* ------------------------------------------------------------------ */
/* Directory scanning                                                  */
/* ------------------------------------------------------------------ */

static bool IsHexHash(const char* name) {
    if (name[0] != '0' || (name[1] != 'x' && name[1] != 'X')) return false;
    for (const char* p = name + 2; *p; ++p) {
        char c = *p;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return name[2] != '\0'; /* must have at least one hex digit */
}

static uint64_t ParseHexHash(const char* name) {
    /* Skip "0x" prefix */
    return std::strtoull(name + 2, nullptr, 16);
}

/*
 * Parse override filename to extract type_hash, name_hash, and display name.
 *
 * Supported formats:
 *   "0xTYPE.name"       → type_hash=TYPE, name_hash=CSymbol64(name)
 *   "0xTYPE.0xNAME"     → type_hash=TYPE, name_hash=NAME
 *   "0xNAME"            → type_hash=0,    name_hash=NAME
 *   "name"              → type_hash=0,    name_hash=CSymbol64(name)
 */
static void ParseOverrideFilename(const char* filename,
                                   uint64_t& name_hash, uint64_t& type_hash,
                                   std::string& display_name)
{
    type_hash = 0;
    display_name = filename;

    /* Check for "0xTYPE.rest" pattern */
    if (filename[0] == '0' && (filename[1] == 'x' || filename[1] == 'X')) {
        const char* dot = std::strchr(filename, '.');
        if (dot) {
            /* Has a dot: "0xTYPE.name" */
            std::string type_str(filename, dot - filename);
            type_hash = std::strtoull(type_str.c_str() + 2, nullptr, 16);

            const char* rest = dot + 1;
            display_name = rest;
            if (IsHexHash(rest)) {
                name_hash = ParseHexHash(rest);
            } else {
                name_hash = CSymbol64_Hash(rest);
            }
            return;
        }
        /* No dot: "0xNAME" */
        name_hash = ParseHexHash(filename);
        return;
    }

    /* Plain name */
    name_hash = CSymbol64_Hash(filename);
}

int ScanOverrideDirectory(const std::string& dir_path) {
    if (!g_overrides) return 0;
    g_overrides->clear();
    int count = 0;

#ifdef _WIN32
    std::string pattern = dir_path;
    if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/')
        pattern += '\\';
    pattern += "*";

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Log("override directory not found or empty: %s", dir_path.c_str());
        return 0;
    }

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        const char* name = fd.cFileName;
        uint64_t name_hash, type_hash;
        std::string display;
        ParseOverrideFilename(name, name_hash, type_hash, display);

        std::string full_path = dir_path;
        if (!full_path.empty() && full_path.back() != '\\' && full_path.back() != '/')
            full_path += '\\';
        full_path += name;

        OverrideEntry entry;
        entry.file_path = full_path;
        entry.display_name = display;
        entry.type_hash = type_hash;
        (*g_overrides)[name_hash] = entry;
        count++;

        if (type_hash) {
            Log("registered override: %s -> name=0x%016llx type=0x%016llx",
                name, static_cast<unsigned long long>(name_hash),
                static_cast<unsigned long long>(type_hash));
        } else {
            Log("registered override: %s -> 0x%016llx",
                name, static_cast<unsigned long long>(name_hash));
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
#else
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) {
        Log("override directory not found or empty: %s", dir_path.c_str());
        return 0;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_type == DT_DIR) continue;

        const char* name = ent->d_name;
        if (name[0] == '.') continue;

        uint64_t name_hash, type_hash;
        std::string display;
        ParseOverrideFilename(name, name_hash, type_hash, display);

        std::string full_path = dir_path;
        if (!full_path.empty() && full_path.back() != '/')
            full_path += '/';
        full_path += name;

        OverrideEntry entry;
        entry.file_path = full_path;
        entry.display_name = display;
        entry.type_hash = type_hash;
        (*g_overrides)[name_hash] = entry;
        count++;

        if (type_hash) {
            Log("registered override: %s -> name=0x%016llx type=0x%016llx",
                name, static_cast<unsigned long long>(name_hash),
                static_cast<unsigned long long>(type_hash));
        } else {
            Log("registered override: %s -> 0x%016llx",
                name, static_cast<unsigned long long>(name_hash));
        }
    }

    closedir(dir);
#endif

    return count;
}

/* ------------------------------------------------------------------ */
/* File reading                                                        */
/* ------------------------------------------------------------------ */

/*
 * Read an entire file into a newly allocated buffer.
 * The caller does NOT free this — it is tracked in g_allocated_buffers
 * and freed on plugin shutdown, because the game holds the pointer.
 */
static void* ReadOverrideFile(const std::string& path, uint64_t* out_size) {
    *out_size = 0;

#ifdef _WIN32
    HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return nullptr;

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(hFile, &file_size)) {
        CloseHandle(hFile);
        return nullptr;
    }

    uint64_t size = static_cast<uint64_t>(file_size.QuadPart);
    if (size == 0) {
        CloseHandle(hFile);
        return nullptr;
    }

    void* buf = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) {
        CloseHandle(hFile);
        return nullptr;
    }

    DWORD bytes_read = 0;
    BOOL ok = ReadFile(hFile, buf, static_cast<DWORD>(size), &bytes_read, nullptr);
    CloseHandle(hFile);

    if (!ok || bytes_read != size) {
        VirtualFree(buf, 0, MEM_RELEASE);
        return nullptr;
    }

    if (g_buffer_cs && g_allocated_buffers) {
        EnterCriticalSection(g_buffer_cs);
        g_allocated_buffers->push_back(buf);
        LeaveCriticalSection(g_buffer_cs);
    }

    *out_size = size;
    return buf;
#else
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return nullptr;

    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        std::fclose(f);
        return nullptr;
    }

    void* buf = std::malloc(static_cast<size_t>(size));
    if (!buf) {
        std::fclose(f);
        return nullptr;
    }

    size_t read = std::fread(buf, 1, static_cast<size_t>(size), f);
    std::fclose(f);

    if (read != static_cast<size_t>(size)) {
        std::free(buf);
        return nullptr;
    }

    if (g_allocated_buffers) {
        g_allocated_buffers->push_back(buf);
    }

    *out_size = static_cast<uint64_t>(size);
    return buf;
#endif
}

/* ------------------------------------------------------------------ */
/* MinHook on Resource_InitFromBuffers @ 0x140fa2510                   */
/* ------------------------------------------------------------------ */

/*
 * Resource_InitFromBuffers signature (from Ghidra decompilation):
 *   void __fastcall Resource_InitFromBuffers(
 *       CResource* resource,    // RCX
 *       void* buf1,             // RDX — primary data buffer
 *       uint64_t size1,         // R8  — primary buffer size
 *       void* buf2,             // R9  — secondary data buffer
 *       uint64_t size2          // [RSP+0x28] — secondary buffer size
 *   )
 *
 * The function stores buf1/size1/buf2/size2 into the CResource struct
 * at offsets +0x40/+0x50/+0x48/+0x58 and then calls vtable[7]
 * (DeserializeAndUpload).
 *
 * We intercept BEFORE the original runs, check if name_symbol matches
 * an override, and if so replace buf1/size1 with our file data.
 */

/*
 * CResource struct layout (from Ghidra FUN_140fa2510):
 *   +0x00: vtable
 *   +0x28: type_symbol (CResourceID, uint64 hash)
 *   +0x38: name_symbol (CResourceID, uint64 hash)
 *   +0x40: buf1 (primary data buffer pointer)
 *   +0x48: buf2 (secondary data buffer pointer)
 *   +0x50: size1 (primary buffer size)
 *   +0x58: size2 (secondary buffer size)
 *   +0x60: load_state (uint32, 4 = loaded)
 *
 * FUN_140fa2510 takes ONLY `this` (RCX). It reads buf1/size1/buf2/size2
 * from the struct, NOT from register parameters. To override, we must
 * write directly into the struct fields before calling the original.
 */

typedef void (__fastcall* ResourceInitFromBuffers_fn)(void* resource);

static ResourceInitFromBuffers_fn orig_InitFromBuffers = nullptr;

static void __fastcall hook_InitFromBuffers(void* resource)
{
    if (resource && g_overrides) {
        auto res = reinterpret_cast<uintptr_t>(resource);

        /* Read name_symbol from CResource+0x38 */
        uint64_t name_hash = *reinterpret_cast<uint64_t*>(res + 0x38);
        uint64_t type_hash_dbg = *reinterpret_cast<uint64_t*>(res + 0x28);
        uint64_t size1_dbg = *reinterpret_cast<uint64_t*>(res + 0x50);

        /* Log the first few resource loads to verify hook is active and check type hashes */
        static int log_count = 0;
        if (log_count < 40) {
            Log("RESOURCE: name=0x%016llx type=0x%016llx size=%llu",
                static_cast<unsigned long long>(name_hash),
                static_cast<unsigned long long>(type_hash_dbg),
                static_cast<unsigned long long>(size1_dbg));
            log_count++;
        }

        if (g_overrides->empty()) goto call_original;

        auto it = g_overrides->find(name_hash);
        if (it != g_overrides->end()) {
            /* Check type_symbol at CResource+0x28.
             * Multiple resource types share the same name (mesh, texture,
             * physics, level data, etc.). Only replace our target type:
             * 0x347869ce492dc7da = CComponentSpaceResource */
            uint64_t type_hash = *reinterpret_cast<uint64_t*>(res + 0x28);
            constexpr uint64_t COMPONENT_SPACE_TYPE = 0x347869ce492dc7daULL;
            if (type_hash != COMPONENT_SPACE_TYPE) {
                goto call_original;
            }

            uint64_t override_size = 0;
            void* override_buf = ReadOverrideFile(it->second.file_path, &override_size);

            if (override_buf && override_size > 0) {
                uint64_t original_size = *reinterpret_cast<uint64_t*>(res + 0x50);
                g_override_hits++;
                Log("OVERRIDE: name=0x%016llx type=0x%016llx (%s) original_size=%llu override_size=%llu",
                    static_cast<unsigned long long>(name_hash),
                    static_cast<unsigned long long>(type_hash),
                    it->second.display_name.c_str(),
                    static_cast<unsigned long long>(original_size),
                    static_cast<unsigned long long>(override_size));

                /* Write override buffer into the struct. The original function
                 * reads buf1/size1 from +0x40/+0x50, not from registers. */
                *reinterpret_cast<void**>(res + 0x40) = override_buf;
                *reinterpret_cast<uint64_t*>(res + 0x50) = override_size;

                orig_InitFromBuffers(resource);
                return;
            } else {
                g_override_errors++;
                Log("ERROR: failed to read override file: %s",
                    it->second.file_path.c_str());
            }
        }
    }

call_original:
    /* No override — call original */
    orig_InitFromBuffers(resource);
}

bool InstallHook(uintptr_t base_addr) {
    g_base_addr = base_addr;

    /* Allocate runtime state on first call (avoids static constructors) */
    if (!g_overrides) {
        g_overrides = new std::unordered_map<uint64_t, OverrideEntry>();
    }
    if (!g_allocated_buffers) {
        g_allocated_buffers = new std::vector<void*>();
    }
#ifdef _WIN32
    if (!g_buffer_cs) {
        g_buffer_cs = new CRITICAL_SECTION();
        InitializeCriticalSection(g_buffer_cs);
    }
#endif

    auto* target = nevr::ResolveVA(base_addr, nevr::addresses::VA_RESOURCE_INIT_FROM_BUFFERS);
    Log("resolved Resource_InitFromBuffers: base=0x%llx va=0x%llx target=%p",
        (unsigned long long)base_addr,
        (unsigned long long)nevr::addresses::VA_RESOURCE_INIT_FROM_BUFFERS,
        target);

    if (!target) {
        Log("ResolveVA returned null — address resolution failed");
        return false;
    }

    /* Initialize this DLL's private MinHook instance.
     * Each statically-linked MinHook has its own global state, so
     * MH_Initialize() here does not conflict with gamepatches core. */
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MH_Initialize failed: %d", status);
        return false;
    }

    status = MH_CreateHook(
        target,
        reinterpret_cast<void*>(&hook_InitFromBuffers),
        reinterpret_cast<void**>(&orig_InitFromBuffers));

    if (status != MH_OK) {
        Log("MH_CreateHook failed for Resource_InitFromBuffers: %d", status);
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        Log("MH_EnableHook failed: %d", status);
        return false;
    }

    Log("hook installed on Resource_InitFromBuffers @ 0x%llx",
        static_cast<unsigned long long>(nevr::addresses::VA_RESOURCE_INIT_FROM_BUFFERS));
    return true;
}

void RemoveHook() {
    if (g_base_addr != 0) {
        auto* target = nevr::ResolveVA(g_base_addr, nevr::addresses::VA_RESOURCE_INIT_FROM_BUFFERS);
        MH_DisableHook(target);
        MH_RemoveHook(target);
        MH_Uninitialize();
        Log("hook removed (overrides applied: %llu, errors: %llu)",
            static_cast<unsigned long long>(g_override_hits),
            static_cast<unsigned long long>(g_override_errors));
    }

    /* Free all override buffers */
    if (g_allocated_buffers) {
#ifdef _WIN32
        if (g_buffer_cs) EnterCriticalSection(g_buffer_cs);
#endif
        for (void* buf : *g_allocated_buffers) {
#ifdef _WIN32
            VirtualFree(buf, 0, MEM_RELEASE);
#else
            std::free(buf);
#endif
        }
#ifdef _WIN32
        if (g_buffer_cs) LeaveCriticalSection(g_buffer_cs);
#endif
        delete g_allocated_buffers;
        g_allocated_buffers = nullptr;
    }

#ifdef _WIN32
    if (g_buffer_cs) {
        DeleteCriticalSection(g_buffer_cs);
        delete g_buffer_cs;
        g_buffer_cs = nullptr;
    }
#endif

    if (g_overrides) {
        delete g_overrides;
        g_overrides = nullptr;
    }

    g_base_addr = 0;
}

} // namespace nevr::filesystem_loader

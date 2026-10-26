// Prototype runner for the probe: maps an ok-format image (ELF ET_DYN, no OS),
// binds its kal_* imports to this process's openkal implementation in the
// architecture's fixed convention, and calls its entry. Prints what it did.
//
// Probe code: the memory calls are the system's (the product design puts them
// behind openkal.exec, design §7.2 L2), selected with the preprocessor.
#if defined(_WIN32)
extern "C" {
__declspec(dllimport) void* __stdcall VirtualAlloc(void*, unsigned long long, unsigned long, unsigned long);
__declspec(dllimport) int __stdcall VirtualProtect(void*, unsigned long long, unsigned long, unsigned long*);
__declspec(dllimport) unsigned long __stdcall GetLastError(void);
}
#else
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#endif
import std;
import openkal.types;
import openkal.stream;
import openkal.env;
import openkal.memory;
import openkal.task;

#if defined(__x86_64__) || defined(_M_X64)
#define OK_ABI __attribute__((sysv_abi))
#define OK_ARCH "x86_64"
constexpr std::uint16_t kMachine = 62;
#else
#define OK_ABI
#define OK_ARCH "aarch64"
constexpr std::uint16_t kMachine = 183;
#endif

namespace {

struct Ehdr { unsigned char ident[16]; std::uint16_t type, machine; std::uint32_t version; std::uint64_t entry, phoff, shoff;
              std::uint32_t flags; std::uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { std::uint32_t type, flags; std::uint64_t offset, vaddr, paddr, filesz, memsz, align; };
struct Dyn  { std::int64_t tag; std::uint64_t val; };
struct Sym  { std::uint32_t name; unsigned char info, other; std::uint16_t shndx; std::uint64_t value, size; };
struct Rela { std::uint64_t offset, info; std::int64_t addend; };

// ── the exports, in the image's convention ──────────────────────────────────
// On x86_64 Windows the implementation speaks the MS convention and the image
// SysV: each export is a SysV entry that calls the implementation. Elsewhere the
// attribute is the platform's own and the entry is a plain forward.
OK_ABI kal_stream t_stdout() { return kal_stdout(); }
OK_ABI kal_intptr t_write(kal_stream s, const void* p, kal_uintptr n) { return kal_stream_write(s, p, n); }
OK_ABI kal_uintptr t_argc() { return kal_env_arg_count(); }
OK_ABI kal_intptr t_arg(kal_uintptr i, char* o, kal_uintptr c) { return kal_env_arg(i, o, c); }
OK_ABI kal_uintptr t_current() { return kal_task_current(); }
OK_ABI void* t_alloc(kal_uintptr s, kal_uintptr a) { return kal_alloc(s, a); }
OK_ABI int t_join(kal_task t) { return kal_task_join(t); }

// A callback the implementation makes into the image crosses the other way:
// the implementation begins the context in ITS convention, at a trampoline that
// enters the image's entry in the image's.
struct Box { void (OK_ABI *entry)(void*); void* arg; };
void trampoline(void* p) {
    auto* b = static_cast<Box*>(p);
    auto entry = b->entry; auto arg = b->arg;
    kal_free(b, sizeof(Box), alignof(Box));
    entry(arg);
}
OK_ABI int t_start(void (OK_ABI *entry)(void*), void* arg, kal_task* out) {
    auto* b = static_cast<Box*>(kal_alloc(sizeof(Box), alignof(Box)));
    b->entry = entry; b->arg = arg;
    return kal_task_start(trampoline, b, out);
}

const std::map<std::string, void*> kExports = {
    {"kal_stdout", reinterpret_cast<void*>(&t_stdout)},
    {"kal_stream_write", reinterpret_cast<void*>(&t_write)},
    {"kal_env_arg_count", reinterpret_cast<void*>(&t_argc)},
    {"kal_env_arg", reinterpret_cast<void*>(&t_arg)},
    {"kal_task_current", reinterpret_cast<void*>(&t_current)},
    {"kal_alloc", reinterpret_cast<void*>(&t_alloc)},
    {"kal_task_join", reinterpret_cast<void*>(&t_join)},
    {"kal_task_start", reinterpret_cast<void*>(&t_start)},
};

void* reserve(std::size_t n) {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, n, 0x1000 | 0x2000, 0x04);   // MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE
#else
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}
std::string protect(void* p, std::size_t n, bool x, bool w) {
#if defined(_WIN32)
    unsigned long old = 0;
    const unsigned long prot = x ? 0x20 : (w ? 0x04 : 0x02);   // EXECUTE_READ, READWRITE, READONLY
    return VirtualProtect(p, n, prot, &old) ? "ok" : std::format("VirtualProtect error {}", GetLastError());
#else
    const int prot = PROT_READ | (x ? PROT_EXEC : 0) | (w ? PROT_WRITE : 0);
    return mprotect(p, n, prot) == 0 ? "ok" : std::format("mprotect: {}", strerror(errno));
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::println("usage: okrunner-probe <image.oke>"); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<unsigned char> file((std::istreambuf_iterator<char>(in)), {});
    if (file.size() < sizeof(Ehdr)) { std::println("runner: cannot read {}", argv[1]); return 2; }
    Ehdr eh; std::memcpy(&eh, file.data(), sizeof eh);
    std::println("runner: host arch {}, image machine {}, type {}", OK_ARCH, eh.machine, eh.type);
    if (eh.machine != kMachine || eh.type != 3) { std::println("runner: refused: not an image for this architecture"); return 2; }

    std::vector<Phdr> ph(eh.phnum);
    std::memcpy(ph.data(), file.data() + eh.phoff, eh.phnum * sizeof(Phdr));
    std::uint64_t span = 0; const Phdr* dynp = nullptr;
    for (auto& p : ph) {
        if (p.type == 1) span = std::max(span, p.vaddr + p.memsz);
        if (p.type == 2) dynp = &p;
    }
    constexpr std::uint64_t kAlign = 65536;
    span = (span + kAlign - 1) & ~(kAlign - 1);
    auto* base = static_cast<unsigned char*>(reserve(span));
    if (!base) { std::println("runner: cannot reserve {} bytes", span); return 2; }
    for (auto& p : ph) if (p.type == 1) std::memcpy(base + p.vaddr, file.data() + p.offset, p.filesz);

    // Dynamic section: symbols, strings, relocations, hash (for the symbol count).
    std::uint64_t symtab = 0, strtab = 0, rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, hash = 0;
    for (auto* d = reinterpret_cast<Dyn*>(base + dynp->vaddr); d->tag != 0; ++d) {
        switch (d->tag) {
            case 6: symtab = d->val; break;  case 5: strtab = d->val; break;
            case 7: rela = d->val; break;    case 8: relasz = d->val; break;
            case 23: jmprel = d->val; break; case 2: pltrelsz = d->val; break;
            case 4: hash = d->val; break;
        }
    }
    auto* syms = reinterpret_cast<Sym*>(base + symtab);
    auto* strs = reinterpret_cast<const char*>(base + strtab);
    std::vector<std::string> missing;
    int bound = 0, relative = 0;
    auto apply = [&](std::uint64_t at, std::uint64_t size) {
        for (auto* r = reinterpret_cast<Rela*>(base + at); reinterpret_cast<unsigned char*>(r) < base + at + size; ++r) {
            const auto type = static_cast<std::uint32_t>(r->info), symi = static_cast<std::uint32_t>(r->info >> 32);
            auto* slot = reinterpret_cast<std::uint64_t*>(base + r->offset);
            const bool isRelative = type == 8 || type == 1027;
            const bool isSymbol = type == 6 || type == 7 || type == 1 || type == 1025 || type == 1026 || type == 257;
            if (isRelative) { *slot = reinterpret_cast<std::uint64_t>(base) + r->addend; ++relative; continue; }
            if (!isSymbol) { missing.push_back(std::format("relocation type {}", type)); continue; }
            const std::string name = strs + syms[symi].name;
            if (syms[symi].shndx != 0) { *slot = reinterpret_cast<std::uint64_t>(base) + syms[symi].value + r->addend; continue; }
            auto it = kExports.find(name);
            if (it == kExports.end()) { missing.push_back(name); continue; }
            *slot = reinterpret_cast<std::uint64_t>(it->second) + r->addend;
            ++bound;
        }
    };
    if (rela) apply(rela, relasz);
    if (jmprel) apply(jmprel, pltrelsz);
    std::println("runner: {} relative, {} bound to kal_*", relative, bound);
    if (!missing.empty()) {
        for (auto& m : missing) std::println("runner: refused: unresolved {}", m);
        return 4;
    }
    for (auto& p : ph) {
        if (p.type != 1) continue;
        const auto lo = p.vaddr & ~(kAlign - 1), hi = (p.vaddr + p.memsz + kAlign - 1) & ~(kAlign - 1);
        const bool x = p.flags & 1, w = p.flags & 2;
        auto r = protect(base + lo, hi - lo, x, w);
        std::println("runner: segment {:#x}+{:#x} {}{}{} -> {}", p.vaddr, p.memsz, "R", w ? "W" : "", x ? "X" : "", r);
        if (r != "ok") return 5;
        if (x) __builtin___clear_cache(reinterpret_cast<char*>(base + lo), reinterpret_cast<char*>(base + hi));
    }
    // Find ok_main through the dynamic symbols (DT_HASH's chain count).
    const auto nsyms = hash ? reinterpret_cast<std::uint32_t*>(base + hash)[1] : 0u;
    using Entry = int (OK_ABI *)();
    Entry entry = nullptr;
    for (std::uint32_t i = 0; i < nsyms; ++i)
        if (syms[i].shndx != 0 && std::string_view(strs + syms[i].name) == "ok_main")
            entry = reinterpret_cast<Entry>(base + syms[i].value);
    if (!entry) { std::println("runner: refused: no ok_main"); return 4; }
    std::cout.flush();
    const int rc = entry();
    std::println("runner: ok_main returned {}", rc);
    return rc;
}

// Probe for mcpp#970: what cl.exe and link.exe write, and in which encoding.
// Prints facts only; asserts nothing.
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

static HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

static void say(const std::string& s) {
    DWORD n = 0;
    WriteFile(out, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
}

static std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring wide(const std::string& s, UINT cp, bool strict, bool* ok = nullptr) {
    if (s.empty()) { if (ok) *ok = true; return {}; }
    int n = MultiByteToWideChar(cp, strict ? MB_ERR_INVALID_CHARS : 0, s.data(), (int)s.size(), nullptr, 0);
    if (ok) *ok = n > 0;
    if (n <= 0) return {};
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, strict ? MB_ERR_INVALID_CHARS : 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::string hex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string h;
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7f) { h += (char)c; continue; }
        h += "\\x"; h += d[c >> 4]; h += d[c & 15];
    }
    return h;
}

static void write_file(const std::wstring& path, const std::string& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD n = 0;
    WriteFile(h, content.data(), (DWORD)content.size(), &n, nullptr);
    CloseHandle(h);
}

static bool read_file(const std::wstring& path, std::string& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[65536];
    DWORD n = 0;
    content.clear();
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) content.append(buf, n);
    CloseHandle(h);
    return true;
}

struct Ran { DWORD exit = 0; std::string bytes; bool started = false; DWORD error = 0; };

static Ran run(const std::wstring& cmd, DWORD flags, const std::wstring& cwd) {
    Ran r;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring line = cmd;
    r.started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, flags | CREATE_UNICODE_ENVIRONMENT,
                               nullptr, cwd.c_str(), &si, &pi) != 0;
    if (!r.started) r.error = GetLastError();
    CloseHandle(wr);
    CloseHandle(nul);
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) r.bytes.append(buf, n);
    CloseHandle(rd);
    if (r.started) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &r.exit);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return r;
}

static const char* const kExpected[] = {
    "\xe4\xb8\xad\xe6\x96\x87\xc3\xa9",                      // 中文é (the directory)
    "\xe6\x9c\xaa\xe5\xae\x9a\xe4\xb9\x89_\xc3\xa9",         // 未定义_é (the identifier)
    "\xe7\xbc\xba\xe5\xa4\xb1",                              // 缺失 (a missing name)
    "H\xc3\xa9" "ader",                                       // Héader
};

static void report(const char* what, const Ran& r) {
    say(std::string("---- ") + what + ": started=" + (r.started ? "yes" : "no") + " error=" + std::to_string(r.error)
        + " exit=" + std::to_string(r.exit) + " bytes=" + std::to_string(r.bytes.size()) + "\n");
    size_t from = 0;
    while (from < r.bytes.size()) {
        size_t nl = r.bytes.find('\n', from);
        std::string ln = r.bytes.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
        from = nl == std::string::npos ? r.bytes.size() : nl + 1;
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        bool high = false;
        for (unsigned char c : ln) high |= c >= 0x80;
        say("  | " + hex(ln) + "\n");
        if (!high) continue;
        const UINT cps[] = {437, 850, 1252, 936, 65001, GetACP(), GetOEMCP()};
        for (UINT cp : cps) {
            bool ok = false;
            std::string text = utf8(wide(ln, cp, true, &ok));
            std::string hits;
            for (auto* e : kExpected) if (text.find(e) != std::string::npos) hits += std::string(" has[") + e + "]";
            say("    as " + std::to_string(cp) + (ok ? "" : " (invalid)") + ": " + text + hits + "\n");
        }
    }
}

int wmain(int argc, wchar_t** argv) {
    const std::wstring label = argc > 1 ? argv[1] : L"default";
    say("==== probe970 " + utf8(label) + "\n");
    say("GetACP=" + std::to_string(GetACP()) + " GetOEMCP=" + std::to_string(GetOEMCP())
        + " console window=" + (GetConsoleWindow() ? "yes" : "no")
        + " GetConsoleOutputCP=" + std::to_string(GetConsoleOutputCP())
        + " GetConsoleCP=" + std::to_string(GetConsoleCP()) + "\n");
    wchar_t vslang[64] = L"";
    GetEnvironmentVariableW(L"VSLANG", vslang, 64);
    say("VSLANG=" + utf8(vslang) + "\n");
    if (GetConsoleOutputCP() == 0) {
        BOOL made = AllocConsole();
        say(std::string("no console: AllocConsole=") + (made ? "yes" : "no") + " now GetConsoleOutputCP="
            + std::to_string(GetConsoleOutputCP()) + "\n");
    }
    const UINT original = GetConsoleOutputCP();

    const std::wstring root = L"C:\\p970\\\u4e2d\u6587\u00e9";
    CreateDirectoryW(L"C:\\p970", nullptr);
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"\\Inc Dir").c_str(), nullptr);
    write_file(root + L"\\Inc Dir\\H\u00e9ader.h", "#pragma once\ninline int header_value() { return 1; }\n");
    write_file(root + L"\\a.cpp",
               "#include \"H\xc3\xa9" "ader.h\"\n#include <vector>\n"
               "int main() { std::vector<int> v; return header_value() + (int)v.size(); }\n");
    write_file(root + L"\\b.cpp",
               "#include \"H\xc3\xa9" "ader.h\"\nint f() { return \xe6\x9c\xaa\xe5\xae\x9a\xe4\xb9\x89_\xc3\xa9; }\n");
    write_file(root + L"\\c.cpp", "#include \"\xe7\xbc\xba\xe5\xa4\xb1.h\"\n");
    write_file(root + L"\\k.cpp",
               "void h(int); void h(long);\nvoid k() { int unused_\xc3\xa9 = 0; h(1u); }\n");
    write_file(root + L"\\m.ixx",
               "module;\n#include \"H\xc3\xa9" "ader.h\"\nexport module m;\nexport int g() { return header_value(); }\n");

    const std::wstring common = L" /nologo /c /EHsc /std:c++latest /utf-8 /W4 /I \"Inc Dir\" ";
    // The record, and the dependency text, for a compile that succeeds, one that fails, and a module.
    for (const wchar_t* unit : {L"a.cpp", L"b.cpp", L"m.ixx"}) {
        std::wstring base(unit);
        base = base.substr(0, base.find(L'.'));
        DeleteFileW((root + L"\\" + base + L".json").c_str());
        Ran r = run(L"cl.exe" + common + L"/sourceDependencies " + base + L".json /showIncludes " + unit, 0, root);
        report(("showIncludes+sourceDependencies " + utf8(unit)).c_str(), r);
        std::string json;
        if (read_file(root + L"\\" + base + L".json", json)) {
            say("  record " + utf8(base) + ".json (" + std::to_string(json.size()) + " bytes):\n");
            size_t from = 0;
            int shown = 0;
            while (from < json.size() && shown < 40) {
                size_t nl = json.find('\n', from);
                std::string ln = json.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
                from = nl == std::string::npos ? json.size() : nl + 1;
                say("    " + hex(ln) + "\n");
                ++shown;
            }
            bool ok = false;
            wide(json, 65001, true, &ok);
            say(std::string("  record is valid UTF-8: ") + (ok ? "yes" : "no") + "\n");
        } else {
            say("  no record " + utf8(base) + ".json\n");
        }
    }
    // The same compile with the record only: is the output then free of include lines?
    {
        Ran r = run(L"cl.exe" + common + L"/sourceDependencies a2.json a.cpp", 0, root);
        report("sourceDependencies only, a.cpp", r);
    }

    // The encoding of the text: the same failing compile, link and notes under each console code page.
    const UINT pages[] = {437, 1252, 936, 65001};
    for (UINT cp : pages) {
        BOOL set = SetConsoleOutputCP(cp);
        say("\n#### console output code page " + std::to_string(cp) + " set=" + (set ? "yes" : "no")
            + " now=" + std::to_string(GetConsoleOutputCP()) + "\n");
        report("cl b.cpp (inherited console)", run(L"cl.exe" + common + L"b.cpp", 0, root));
        report("cl c.cpp (inherited console)", run(L"cl.exe" + common + L"c.cpp", 0, root));
        report("cl k.cpp (inherited console)", run(L"cl.exe" + common + L"k.cpp", 0, root));
        report("link (inherited console)", run(L"link.exe /nologo \u7f3a\u5931.obj", 0, root));
        report("cl b.cpp DETACHED_PROCESS", run(L"cl.exe" + common + L"b.cpp", DETACHED_PROCESS, root));
        report("cl b.cpp CREATE_NO_WINDOW", run(L"cl.exe" + common + L"b.cpp", CREATE_NO_WINDOW, root));
        report("link DETACHED_PROCESS", run(L"link.exe /nologo \u7f3a\u5931.obj", DETACHED_PROCESS, root));
    }
    SetConsoleOutputCP(original);
    say("\nrestored console output code page " + std::to_string(GetConsoleOutputCP()) + "\n");
    return 0;
}

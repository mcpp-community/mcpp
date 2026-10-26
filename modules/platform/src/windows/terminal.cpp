// mcpp.platform.terminal on Windows: consoles, the console's output code
// page, keys read as console input events, and standard output diverted at
// the C runtime's descriptor.
module;
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <cstdio>
#include <io.h>        // _dup, _dup2, _close, _get_osfhandle, _fileno, _write
#include <windows.h>   // SetHandleInformation, GetConsoleMode, ReadConsoleInputW

module mcpp.platform.terminal;

import std;

namespace mcpp::platform::terminal::native {

namespace {

HANDLE handle_of(Stream s) {
    const auto h = reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(s == Stream::Out ? stdout : stderr)));
    return h == nullptr ? INVALID_HANDLE_VALUE : h;
}

bool console_of(Stream s) {
    const auto h = handle_of(s);
    DWORD mode = 0;
    return h != INVALID_HANDLE_VALUE && ::GetConsoleMode(h, &mode);
}

} // namespace

bool same_terminal() {
    // A process has at most one console.
    return console_of(Stream::Out) && console_of(Stream::Err);
}

unsigned console_output_code_page(Stream s) {
    return console_of(s) ? static_cast<unsigned>(::GetConsoleOutputCP()) : 0;
}

bool write_descriptor(int fd, std::string_view text) {
    std::size_t done = 0;
    while (done < text.size()) {
        const int n = ::_write(fd, text.data() + done, static_cast<unsigned>(text.size() - done));
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

int divert_stdout() {
    const int saved = ::_dup(1);
    if (saved >= 0) {
        // `_dup` duplicates the handle as inheritable, and CreateProcess with
        // handle inheritance (every `_popen` and `system`) passes it on.
        const auto h = reinterpret_cast<HANDLE>(::_get_osfhandle(saved));
        if (h != INVALID_HANDLE_VALUE)
            ::SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
        ::_dup2(2, 1);
    }
    return saved;
}

void restore_stdout(int saved) {
    ::_dup2(saved, 1);
    ::_close(saved);
}

} // namespace mcpp::platform::terminal::native

namespace mcpp::platform::terminal {

namespace {

HANDLE g_inputHandle = INVALID_HANDLE_VALUE;
DWORD  g_inputMode   = 0;
BOOL WINAPI restore_input_mode(DWORD) {
    if (g_inputHandle != INVALID_HANDLE_VALUE) ::SetConsoleMode(g_inputHandle, g_inputMode);
    return FALSE;   // the next handler, then the default action, still run
}

} // namespace

KeyInput::KeyInput(Stream display) {
    if (!is_terminal(display)) return;
    const HANDLE h = ::GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || !::GetConsoleMode(h, &mode)) return;
    g_inputHandle = h;
    g_inputMode   = mode;
    savedMode_    = mode;
    ::SetConsoleCtrlHandler(restore_input_mode, TRUE);
    // ENABLE_PROCESSED_INPUT stays: Ctrl-C still interrupts.
    if (!::SetConsoleMode(h, mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT))) return;
    active_ = true;
}

KeyInput::~KeyInput() {
    if (!active_) return;
    ::SetConsoleMode(g_inputHandle, static_cast<DWORD>(savedMode_));
    ::SetConsoleCtrlHandler(restore_input_mode, FALSE);
    g_inputHandle = INVALID_HANDLE_VALUE;
}

std::vector<Key> KeyInput::read() {
    std::vector<Key> keys;
    if (!active_) return keys;
    DWORD n = 0;
    while (::GetNumberOfConsoleInputEvents(g_inputHandle, &n) && n > 0) {
        INPUT_RECORD rec{};
        DWORD got = 0;
        if (!::ReadConsoleInputW(g_inputHandle, &rec, 1, &got) || got == 0) break;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
        switch (rec.Event.KeyEvent.wVirtualKeyCode) {
        case VK_UP:    case 'W': keys.push_back(Key::Up);    break;
        case VK_DOWN:  case 'S': keys.push_back(Key::Down);  break;
        case VK_LEFT:  case 'A': keys.push_back(Key::Left);  break;
        case VK_RIGHT: case 'D': keys.push_back(Key::Right); break;
        case VK_SPACE:           keys.push_back(Key::Space); break;
        default: break;
        }
    }
    return keys;
}

} // namespace mcpp::platform::terminal

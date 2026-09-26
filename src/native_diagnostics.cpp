#include "native_diagnostics.hpp"
#include "bumble_version.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#ifdef _WIN32
#include <Windows.h>
#include <DbgHelp.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace bumble::diagnostics {
namespace {
constexpr size_t log_limit = 8 * 1024 * 1024;
std::filesystem::path session;
std::atomic<const char*> current_phase{"startup"};
std::atomic_flag crashing = ATOMIC_FLAG_INIT;
static_assert(std::atomic<const char*>::is_always_lock_free);
std::thread log_thread;
int pipe_read = -1;
int console_fd = -1;
int log_fd = -1;
bool active = false;
thread_local std::array<char, 1024> exception_message{};
thread_local std::atomic_bool exception_ready{false};

bool is_link(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path));
#endif
}

#ifdef _WIN32
int close_fd(int fd) { return _close(fd); }
int write_fd(int fd, const void* bytes, unsigned size) { return _write(fd, bytes, size); }
int read_fd(int fd, void* bytes, unsigned size) { return _read(fd, bytes, size); }
int open_file(const std::filesystem::path& path) {
    int fd = -1;
    if (_wsopen_s(&fd, path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY | _O_NOINHERIT,
            _SH_DENYNO, _S_IREAD | _S_IWRITE)) return -1;
    return fd;
}
#else
int close_fd(int fd) { return close(fd); }
ssize_t write_fd(int fd, const void* bytes, unsigned size) { return write(fd, bytes, size); }
ssize_t read_fd(int fd, void* bytes, unsigned size) { return read(fd, bytes, size); }
int open_file(const std::filesystem::path& path) {
    return open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
}
#endif

void write_all(int fd, const void* bytes, size_t length) noexcept {
    auto* data = static_cast<const char*>(bytes);
    while (fd >= 0 && length) {
        const auto count = write_fd(fd, data, static_cast<unsigned>(std::min<size_t>(length, 65536)));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        data += count;
        length -= static_cast<size_t>(count);
    }
}

void collect_log() noexcept {
    std::array<char, 16384> buffer{};
    size_t size = 0;
    for (;;) {
        const auto count = read_fd(pipe_read, buffer.data(), static_cast<unsigned>(buffer.size()));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        if (size + static_cast<size_t>(count) > log_limit) {
            close_fd(log_fd);
            log_fd = -1;
            try {
                std::error_code error;
                std::filesystem::remove(session / "runtime.previous.log", error);
                std::filesystem::rename(session / "runtime.log", session / "runtime.previous.log", error);
                log_fd = open_file(session / "runtime.log");
            } catch (...) {}
            size = 0;
        }
        write_all(log_fd, buffer.data(), static_cast<size_t>(count));
        write_all(console_fd, buffer.data(), static_cast<size_t>(count));
        size += static_cast<size_t>(count);
    }
    if (log_fd >= 0) close_fd(log_fd);
    close_fd(pipe_read);
    if (console_fd >= 0) close_fd(console_fd);
}

struct Text {
    std::array<char, 4096> bytes{};
    size_t size = 0;
    void add(const char* value) noexcept {
        while (value && *value && size < bytes.size()) bytes[size++] = *value++;
    }
    void number(uint64_t value, unsigned base = 16) noexcept {
        char digits[32];
        size_t count = 0;
        do { digits[count++] = "0123456789abcdef"[value % base]; value /= base; } while (value);
        while (count && size < bytes.size()) bytes[size++] = digits[--count];
    }
    void header() noexcept {
        add("Bumble "); add(build::kDisplayVersion);
        add("\nphase="); add(current_phase.load(std::memory_order_relaxed));
        if (exception_ready.load(std::memory_order_acquire)) {
            add("\nexception_message="); add(exception_message.data());
        }
    }
};

#ifdef _WIN32
std::wstring report_path, dump_path;
HANDLE dump_request = nullptr, dump_done = nullptr, dump_thread = nullptr;
std::atomic_bool dump_stop{false};
EXCEPTION_RECORD saved_record{};
CONTEXT saved_context{};
EXCEPTION_POINTERS saved_exception{&saved_record, &saved_context};
DWORD fault_thread = 0;
LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = nullptr;

DWORD WINAPI dump_worker(void*) {
    WaitForSingleObject(dump_request, INFINITE);
    if (dump_stop.load()) return 0;
    const HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{fault_thread, &saved_exception, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
            static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo), &info, nullptr, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
    }
    SetEvent(dump_done);
    return 0;
}

LONG WINAPI exception_handler(EXCEPTION_POINTERS* error) noexcept {
    if (crashing.test_and_set()) TerminateProcess(GetCurrentProcess(), 127);
    Text text;
    text.header(); text.add("\nplatform=windows\nexception=0x");
    text.number(error->ExceptionRecord->ExceptionCode);
    text.add("\naddress=0x"); text.number(reinterpret_cast<uintptr_t>(error->ExceptionRecord->ExceptionAddress));
    text.add("\nthread="); text.number(GetCurrentThreadId(), 10); text.add("\n");
    const HANDLE file = CreateFileW(report_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD count;
        WriteFile(file, text.bytes.data(), static_cast<DWORD>(text.size), &count, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
    }
    saved_record = *error->ExceptionRecord;
    saved_record.ExceptionRecord = nullptr;
    saved_context = *error->ContextRecord;
    fault_thread = GetCurrentThreadId();
    SetEvent(dump_request);
    WaitForSingleObject(dump_done, 5000);
    return EXCEPTION_EXECUTE_HANDLER;
}

void abort_handler(int) {
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0xE0004242;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
    EXCEPTION_POINTERS exception{&record, &context};
    exception_handler(&exception);
    TerminateProcess(GetCurrentProcess(), 134);
}

void install_handlers() {
    report_path = (session / "crash.txt").wstring();
    dump_path = (session / "crash.dmp").wstring();
    dump_request = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    dump_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (dump_request && dump_done) dump_thread = CreateThread(nullptr, 0, dump_worker, nullptr, 0, nullptr);
    if (!dump_request || !dump_done || !dump_thread) throw std::runtime_error("Cannot start crash reporter");
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    previous_filter = SetUnhandledExceptionFilter(exception_handler);
    std::signal(SIGABRT, abort_handler);
}
#else
std::array<char, 4096> report_path{}, maps_path{}, stack_path{};
alignas(16) std::array<char, 65536> alternate_stack{};
std::array<char, 65536> fault_stack{};

void signal_handler(int signal_number, siginfo_t* info, void* context) noexcept {
    if (crashing.test_and_set()) _exit(128 + signal_number);
    Text text;
    text.header(); text.add("\nplatform=linux\nsignal="); text.number(signal_number, 10);
    text.add("\nthread="); text.number(static_cast<uint64_t>(syscall(SYS_gettid)), 10);
    text.add("\naddress=0x"); text.number(reinterpret_cast<uintptr_t>(info->si_addr));
    const auto* registers = &static_cast<ucontext_t*>(context)->uc_mcontext;
    for (int index = 0; index < NGREG; ++index) {
        text.add("\nregister["); text.number(index, 10); text.add("]=0x");
        text.number(static_cast<uint64_t>(registers->gregs[index]));
    }
    text.add("\nrip=0x"); text.number(registers->gregs[REG_RIP]);
    text.add("\nrsp=0x"); text.number(registers->gregs[REG_RSP]);
    text.add("\nrbp=0x"); text.number(registers->gregs[REG_RBP]); text.add("\n");
    const int report = open(report_path.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    write_all(report, text.bytes.data(), text.size);
    if (report >= 0) { fsync(report); close(report); }

    const int maps = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    const int output = open(maps_path.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (maps >= 0 && output >= 0) {
        for (;;) {
            const auto count = read(maps, fault_stack.data(), fault_stack.size());
            if (count <= 0) break;
            write_all(output, fault_stack.data(), static_cast<size_t>(count));
        }
        fsync(output);
    }
    if (maps >= 0) close(maps);
    if (output >= 0) close(output);
    const int memory = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (memory >= 0) {
        const auto count = pread(memory, fault_stack.data(), fault_stack.size(), registers->gregs[REG_RSP]);
        if (count > 0) {
            const int stack = open(stack_path.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
            write_all(stack, fault_stack.data(), static_cast<size_t>(count));
            if (stack >= 0) { fsync(stack); close(stack); }
        }
        close(memory);
    }
    _exit(128 + signal_number);
}

void install_handlers() {
    const auto set_path = [](auto& target, const char* name) {
        const auto path = (session / name).string();
        if (path.size() >= target.size()) throw std::runtime_error("Crash report path is too long");
        std::memcpy(target.data(), path.c_str(), path.size() + 1);
    };
    set_path(report_path, "crash.txt");
    set_path(maps_path, "crash.maps");
    set_path(stack_path, "crash.stack");
    stack_t stack{};
    stack.ss_sp = alternate_stack.data(); stack.ss_size = alternate_stack.size();
    if (sigaltstack(&stack, nullptr)) throw std::runtime_error("Cannot install crash signal stack");
    for (int number : {SIGSEGV, SIGABRT, SIGILL, SIGFPE, SIGBUS}) {
        struct sigaction action{};
        action.sa_sigaction = signal_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        if (sigaction(number, &action, nullptr)) throw std::runtime_error("Cannot install crash signal handler");
    }
}
#endif

void terminate_handler() noexcept {
    try {
        if (auto error = std::current_exception()) std::rethrow_exception(error);
    } catch (const std::exception& error) {
        std::strncpy(exception_message.data(), error.what(), exception_message.size() - 1);
        exception_ready.store(true, std::memory_order_release);
        std::fprintf(stderr, "Unhandled exception: %s\n", error.what());
    } catch (...) {
        std::strcpy(exception_message.data(), "Non-standard C++ exception");
        exception_ready.store(true, std::memory_order_release);
        std::fprintf(stderr, "Unhandled exception\n");
    }
    std::fflush(stderr);
    std::abort();
}

void prune(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> runs;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (std::filesystem::absolute(entry.path()).lexically_normal() == session.lexically_normal()) continue;
        const auto name = entry.path().filename().string();
        if (!is_link(entry.path()) && entry.is_directory() && name.starts_with("run-") &&
            name.find_first_not_of("0123456789-", 4) == std::string::npos &&
            !is_link(entry.path() / "session.txt") &&
            std::filesystem::is_regular_file(entry.path() / "session.txt")) {
            std::ifstream marker(entry.path() / "session.txt");
            std::string first_line;
            if (std::getline(marker, first_line) && first_line.starts_with("Bumble V")) runs.push_back(entry.path());
        }
    }
    std::sort(runs.begin(), runs.end());
    for (size_t i = 0; i + 4 < runs.size(); ++i) {
        bool known = true;
        for (const auto& entry : std::filesystem::directory_iterator(runs[i])) {
            const auto name = entry.path().filename().string();
            if (is_link(entry.path()) || !entry.is_regular_file() ||
                (name != "session.txt" && name != "runtime.log" && name != "runtime.previous.log" &&
                 name != "crash.txt" && name != "crash.dmp" && name != "crash.maps" && name != "crash.stack")) known = false;
        }
        if (known) std::filesystem::remove_all(runs[i]);
    }
}
}

void initialize(const std::filesystem::path& root, bool console) {
    for (auto path = std::filesystem::absolute(root); !path.empty();) {
        if (is_link(path))
            throw std::runtime_error("Log folder must not contain links");
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    std::filesystem::create_directories(root);
    if (std::filesystem::is_symlink(root)) throw std::runtime_error("Log folder must not be a link");
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    session = std::filesystem::absolute(root / ("run-" + std::to_string(stamp) + "-" + std::to_string(pid)));
    if (!std::filesystem::create_directory(session)) throw std::runtime_error("Cannot create launch report folder");
    const int header = open_file(session / "session.txt");
    if (header < 0) throw std::runtime_error("Cannot write launch report");
    Text text; text.header(); text.add("\npid="); text.number(pid, 10); text.add("\n");
    write_all(header, text.bytes.data(), text.size);
#ifndef _WIN32
    struct utsname system{};
    if (!uname(&system)) {
        const auto os = std::string(system.sysname) + " " + system.release + " " + system.machine + "\n";
        write_all(header, os.data(), os.size());
    }
#endif
    close_fd(header);
    log_fd = open_file(session / "runtime.log");
    if (log_fd < 0) throw std::runtime_error("Cannot write runtime log");
    int descriptors[2] = {-1, -1};
#ifdef _WIN32
    FILE* stream = nullptr;
    if (_fileno(stdout) < 0) (void)freopen_s(&stream, "NUL", "w", stdout);
    if (_fileno(stderr) < 0) (void)freopen_s(&stream, "NUL", "w", stderr);
    if (_pipe(descriptors, 65536, _O_BINARY | _O_NOINHERIT)) throw std::runtime_error("Cannot open log pipe");
    if (console) console_fd = _dup(_fileno(stderr));
#else
    if (pipe2(descriptors, O_CLOEXEC)) throw std::runtime_error("Cannot open log pipe");
    if (console) console_fd = dup(STDERR_FILENO);
#endif
    pipe_read = descriptors[0];
    try { log_thread = std::thread(collect_log); }
    catch (...) {
        close_fd(descriptors[0]); close_fd(descriptors[1]); close_fd(log_fd);
        if (console_fd >= 0) close_fd(console_fd);
        throw;
    }
    active = true;
#ifdef _WIN32
    const bool redirected = _dup2(descriptors[1], _fileno(stdout)) == 0 &&
        _dup2(descriptors[1], _fileno(stderr)) == 0;
#else
    const bool redirected = dup2(descriptors[1], STDOUT_FILENO) >= 0 &&
        dup2(descriptors[1], STDERR_FILENO) >= 0;
#endif
    close_fd(descriptors[1]);
    if (!redirected) throw std::runtime_error("Cannot redirect launch log");
#ifdef _WIN32
    SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
    SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))));
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    install_handlers();
    std::set_terminate(terminate_handler);
    if (std::atexit([] { finish(-1); })) throw std::runtime_error("Cannot register log shutdown");
    try { prune(root); } catch (...) {}
    const auto path_text = session.u8string();
    std::fprintf(stderr, "Bumble %s\nLaunch report: %s\n", build::kDisplayVersion,
        reinterpret_cast<const char*>(path_text.c_str()));
}

void phase(const char* name) noexcept {
    current_phase.store(name, std::memory_order_relaxed);
    std::fprintf(stderr, "Startup: %s\n", name);
}

void end_startup() noexcept {
    if (!log_thread.joinable()) return;
    std::fflush(stdout); std::fflush(stderr);
#ifdef _WIN32
    int null_fd = -1;
    _sopen_s(&null_fd, "NUL", _O_WRONLY, _SH_DENYNO, 0);
    if (null_fd >= 0) {
        _dup2(null_fd, _fileno(stdout)); _dup2(null_fd, _fileno(stderr)); close_fd(null_fd);
        SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
        SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))));
    } else { _close(_fileno(stdout)); _close(_fileno(stderr)); }
#else
    const int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd >= 0) { dup2(null_fd, STDOUT_FILENO); dup2(null_fd, STDERR_FILENO); close(null_fd); }
    else { close(STDOUT_FILENO); close(STDERR_FILENO); }
#endif
    log_thread.join();
}

void finish(int result) noexcept {
    if (!active) return;
    if (result < 0) std::fprintf(stderr, "Process exited before normal shutdown\n");
    else std::fprintf(stderr, "Exit status: %d\n", result);
    end_startup();
#ifdef _WIN32
    SetUnhandledExceptionFilter(previous_filter);
    std::signal(SIGABRT, SIG_DFL);
    dump_stop.store(true);
    if (dump_thread) {
        SetEvent(dump_request);
        WaitForSingleObject(dump_thread, INFINITE);
        CloseHandle(dump_thread);
    }
    if (dump_request) CloseHandle(dump_request);
    if (dump_done) CloseHandle(dump_done);
#endif
    active = false;
}

void test_crash(const char* kind) {
    if (!kind) return;
    phase("crash reporter test");
    if (!std::strcmp(kind, "quiet") || !std::strcmp(kind, "quiet-crash")) {
        std::fprintf(stderr, "Startup capture complete\n");
        end_startup();
        std::fprintf(stdout, "Gameplay output must not be captured\n");
        std::fprintf(stderr, "Gameplay output must not be captured\n");
        if (!std::strcmp(kind, "quiet-crash")) throw std::runtime_error("Crash after startup");
        return;
    }
    if (!std::strcmp(kind, "normal")) {
#ifdef _WIN32
        constexpr char message[] = "Native stdout test\n";
        DWORD written = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), message, sizeof(message) - 1, &written, nullptr))
            throw std::runtime_error("Native stdout capture failed");
#endif
        return;
    }
    if (!std::strcmp(kind, "exit")) std::exit(7);
    if (!std::strcmp(kind, "exception")) throw std::runtime_error("Crash reporter test exception");
    const auto fault = [] {
        volatile uintptr_t address = 1;
        *reinterpret_cast<volatile unsigned char*>(address) = 0;
    };
    if (!std::strcmp(kind, "signal")) fault();
    else if (!std::strcmp(kind, "worker")) { std::thread worker(fault); worker.join(); }
    else if (!std::strcmp(kind, "abort")) std::abort();
    else if (!std::strcmp(kind, "logs")) {
        std::array<char, 4096> bytes{}; bytes.fill('x');
        for (int i = 0; i < 5000; ++i) std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    } else throw std::runtime_error("Unknown crash reporter test");
}
}

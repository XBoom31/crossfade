#include "win_process.h"

#ifdef P100_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <mutex>
#include <unordered_map>

namespace winproc
{
static FILE *spawn_pipe(const std::string &cmdline, const char *mode);

namespace
{

std::mutex g_mutex;
std::unordered_map<FILE *, HANDLE> g_processes;

std::string shell_command(const std::string &command)
{
    return "cmd.exe /c " + command;
}

std::string quote_arg(const std::string &arg)
{
    std::string out = "\"";
    for (char c : arg)
    {
        if (c == '"')
            out += "\\\"";
        else
            out += c;
    }
    out += '"';
    return out;
}

std::string join_argv(const std::vector<std::string> &args)
{
    std::string line;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i)
            line += ' ';
        line += quote_arg(args[i]);
    }
    return line;
}

HANDLE open_nul(DWORD access, SECURITY_ATTRIBUTES &sa)
{
    return CreateFileA("NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       OPEN_EXISTING, 0, nullptr);
}

} // namespace

int system_hidden(const std::string &command)
{
    std::string cmdline = shell_command(command);

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE nul_in = open_nul(GENERIC_READ, sa);
    HANDLE nul_out = open_nul(GENERIC_WRITE, sa);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul_in;
    si.hStdOutput = nul_out;
    si.hStdError = nul_out;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (nul_in)
        CloseHandle(nul_in);
    if (nul_out)
        CloseHandle(nul_out);
    if (!ok)
        return -1;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

int run_argv_hidden(const std::vector<std::string> &args)
{
    if (args.empty())
        return -1;
    return system_hidden(join_argv(args));
}

int system_hidden_capture(const std::string &command, std::string &output)
{
    output.clear();
    FILE *pipe = popen_hidden(command, "r");
    if (!pipe)
        return -1;

    char buffer[1024];
    size_t n = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
    {
        if (output.size() < 65536)
            output.append(buffer, n);
    }
    return pclose_hidden(pipe);
}

bool spawn_argv_hidden(const std::vector<std::string> &args)
{
    if (args.empty())
        return false;

    std::string cmdline = join_argv(args);

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE nul_in = open_nul(GENERIC_READ, sa);
    HANDLE nul_out = open_nul(GENERIC_WRITE, sa);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul_in;
    si.hStdOutput = nul_out;
    si.hStdError = nul_out;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (nul_in)
        CloseHandle(nul_in);
    if (nul_out)
        CloseHandle(nul_out);
    if (!ok)
        return false;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

FILE *popen_hidden(const std::string &command, const char *mode)
{
    return spawn_pipe(shell_command(command), mode);
}

FILE *popen_direct_hidden(const std::string &command_line, const char *mode)
{
    return spawn_pipe(command_line, mode);
}

int direct_capture(const std::string &command_line, std::string &output)
{
    output.clear();
    FILE *pipe = popen_direct_hidden(command_line, "r");
    if (!pipe)
        return -1;

    char buffer[1024];
    size_t n = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
    {
        if (output.size() < 65536)
            output.append(buffer, n);
    }
    return pclose_hidden(pipe);
}

static FILE *spawn_pipe(const std::string &cmdline, const char *mode)
{
    if (!mode || mode[0] != 'r')
        return nullptr;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0))
        return nullptr;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = open_nul(GENERIC_READ, sa);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = write_end;
    si.hStdError = write_end;

    PROCESS_INFORMATION pi{};
    std::string command_line = cmdline; // CreateProcess may modify the buffer
    BOOL ok = CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (nul)
        CloseHandle(nul);
    CloseHandle(write_end);
    if (!ok)
    {
        CloseHandle(read_end);
        return nullptr;
    }
    CloseHandle(pi.hThread);

    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(read_end),
                                   _O_RDONLY | _O_BINARY);
    if (fd < 0)
    {
        CloseHandle(read_end);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return nullptr;
    }

    FILE *file = _fdopen(fd, "rb");
    if (!file)
    {
        _close(fd);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_processes[file] = pi.hProcess;
    }
    return file;
}

int pclose_hidden(FILE *pipe)
{
    if (!pipe)
        return -1;

    // fclose() closes the read handle; wait for the child afterwards so a
    // caller reading to EOF cannot return before the command finished.
    fclose(pipe);

    HANDLE process = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_processes.find(pipe);
        if (it != g_processes.end())
        {
            process = it->second;
            g_processes.erase(it);
        }
    }
    if (!process)
        return -1;

    WaitForSingleObject(process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    return (int)code;
}

} // namespace winproc

#endif // P100_WINDOWS

// Проверка изоляции: пытается открыть хостовые пути разными способами. Ожидается "blocked" для всего, кроме /usr.
#include <windows.h>
#include <winsock2.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#ifndef XSX_PROBE_HOST_USER
#error "Define XSX_PROBE_HOST_USER as the host login to check sandbox isolation"
#endif
#define XSX_WIDE_IMPL(x) L##x
#define XSX_WIDE(x) XSX_WIDE_IMPL(x)
static int leaks = 0;
static void Try(const wchar_t* path, bool expectVisible)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    bool ok = h != INVALID_HANDLE_VALUE; if (ok) CloseHandle(h);
    bool bad = ok && !expectVisible; leaks += bad;
    wprintf(L"[%ls] %-70ls %ls\n", bad ? L"LEAK" : L"ok  ", path, ok ? L"VISIBLE" : L"blocked");
}
int main()
{
    DWORD d = GetLogicalDrives(); printf("drives:"); for (int i = 0; i < 26; i++) if (d & (1u << i)) printf(" %c:", 'A' + i); printf("\n");
    const wchar_t* host[] = {
        L"Z:\\", L"Z:\\etc\\passwd", L"E:\\", L"D:\\",
        L"\\\\?\\unix\\home\\" XSX_WIDE(XSX_PROBE_HOST_USER) L"",
        L"\\\\?\\unix\\home\\" XSX_WIDE(XSX_PROBE_HOST_USER) L"\\xsx-research\\AGENTS.md", L"\\\\?\\unix\\root", L"\\\\?\\unix\\run\\media",
        L"\\??\\unix\\home\\" XSX_WIDE(XSX_PROBE_HOST_USER) L"", L"\\??\\Z:\\home\\" XSX_WIDE(XSX_PROBE_HOST_USER) L"", L"\\\\.\\unix\\home\\" XSX_WIDE(XSX_PROBE_HOST_USER) L"",
        L"C:\\..\\..\\..\\home\\" XSX_WIDE(XSX_PROBE_HOST_USER) L"", L"G:\\..\\..\\home\\" XSX_WIDE(XSX_PROBE_HOST_USER) L"", L"G:\\..\\..\\..\\..\\etc\\passwd" };
    for (auto p : host) Try(p, false);
    // Синтетические файлы самой песочницы: должны быть видны, но не содержать хостовых данных.
    Try(L"\\\\?\\unix\\etc\\passwd", true);
    { HANDLE h = CreateFileW(L"\\\\?\\unix\\etc\\passwd", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
      char buf[256] = {}; DWORD n = 0; if (h != INVALID_HANDLE_VALUE) { ReadFile(h, buf, 255, &n, nullptr); CloseHandle(h); }
      bool hostUser = strstr(buf, XSX_PROBE_HOST_USER) != nullptr; leaks += hostUser;
      printf("[%s] /etc/passwd content: \"%.*s\"\n", hostUser ? "LEAK" : "ok  ", (int)(n ? n - 1 : 0), buf); }
    Try(L"\\\\?\\unix\\gamedata", true);            // единственное согласованное расширение: каталог загрузки, read-only
    { HANDLE w = CreateFileW(L"\\\\?\\unix\\gamedata\\_probe_write.tmp", GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
      bool wrote = w != INVALID_HANDLE_VALUE; if (wrote) CloseHandle(w); leaks += wrote;
      printf("[%s] write into /gamedata -> %s\n", wrote ? "LEAK" : "ok  ", wrote ? "ALLOWED" : "denied (read-only)"); }
    Try(L"\\\\?\\unix\\run\\media\\" XSX_WIDE(XSX_PROBE_HOST_USER) L"", false);
    Try(L"C:\\windows", true); Try(L"G:\\", true);
    Try(L"\\\\?\\unix\\usr\\bin\\wine", true);   // /usr намеренно виден (read-only)
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons(53); a.sin_addr.s_addr = inet_addr("1.1.1.1");
    DWORD to = 2000; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&to, sizeof to);
    int r = connect(s, (sockaddr*)&a, sizeof a); bool net = r == 0; leaks += net;
    printf("[%s] network connect 1.1.1.1:53 -> %s (wsa=%d)\n", net ? "LEAK" : "ok  ", net ? "CONNECTED" : "failed", WSAGetLastError());
    char b[512]; for (const char* v : { "HOME", "USERPROFILE", "USER", "PATH", "DISPLAY", "SSH_AUTH_SOCK", "XDG_RUNTIME_DIR" }) { DWORD n = GetEnvironmentVariableA(v, b, sizeof b); printf("env %-16s = %s\n", v, n ? b : "(unset)"); }
    printf("\nSUMMARY leaks=%d\n", leaks); return leaks ? 1 : 0;
}

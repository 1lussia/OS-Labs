#include <windows.h>

#define BUF_SIZE 4096

typedef struct {
    DWORD len;
    DWORD done;
    char data[BUF_SIZE];
} Shared;

typedef struct {
    HANDLE file;
    HANDLE map;
    HANDLE evData;
    HANDLE evFree;
    HANDLE process;
    Shared *view;
} Channel;

typedef struct {
    char *data;
    DWORD len;
    DWORD cap;
} Line;

DWORD StrLen(const char *s) {
    DWORD n = 0;
    while (s[n] != 0) n++;
    return n;
}

void Print(const char *s) {
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, StrLen(s), &written, NULL);
}

int FormatUInt(unsigned long long v, char *buf) {
    char tmp[24];
    int len = 0;
    int i = 0;
    do {
        tmp[len++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    while (len > 0) buf[i++] = tmp[--len];
    buf[i] = 0;
    return i;
}

void Fail(const char *what) {
    char num[24];
    FormatUInt(GetLastError(), num);
    Print("Error: ");
    Print(what);
    Print(" (code ");
    Print(num);
    Print(")\n");
    ExitProcess(1);
}

int ReadByte(HANDLE h, char *c) {
    DWORD got = 0;
    if (!ReadFile(h, c, 1, &got, NULL)) {
        DWORD e = GetLastError();
        if (e != ERROR_BROKEN_PIPE && e != ERROR_HANDLE_EOF) Fail("ReadFile");
        return 0;
    }
    return got == 1;
}

void LineAdd(Line *l, char c) {
    if (l->len == l->cap) {
        DWORD newCap = 128;
        char *p;
        if (l->cap > 0) newCap = l->cap * 2;
        if (l->data == NULL) p = HeapAlloc(GetProcessHeap(), 0, newCap);
        else p = HeapReAlloc(GetProcessHeap(), 0, l->data, newCap);
        if (p == NULL) Fail("HeapAlloc");
        l->data = p;
        l->cap = newCap;
    }
    l->data[l->len] = c;
    l->len++;
}

int ReadLine(HANDLE h, Line *l) {
    char c;
    l->len = 0;
    while (ReadByte(h, &c)) {
        if (c == '\r') continue;
        if (c == '\n') return 1;
        LineAdd(l, c);
    }
    return l->len > 0;
}

unsigned int seed;

int RandomPercent(void) {
    seed = seed * 1103515245 + 12345;
    return (seed >> 16) % 100;
}

void ReadFileName(HANDLE in, const char *question, char *name) {
    Line line = {0};
    Print(question);
    if (!ReadLine(in, &line)) Fail("no input");
    if (line.len == 0 || line.len >= MAX_PATH) Fail("bad file name");
    for (DWORD i = 0; i < line.len; i++) name[i] = line.data[i];
    name[line.len] = 0;
}

void BuildName(char *out, const char *base, const char *suffix) {
    DWORD n = StrLen(base);
    for (DWORD i = 0; i < n; i++) out[i] = base[i];
    for (DWORD i = 0; i <= StrLen(suffix); i++) out[n + i] = suffix[i];
}

void MakeBase(char *out, DWORD pid, int index) {
    const char *prefix = "Lab3_";
    int n = 0;
    for (int i = 0; prefix[i] != 0; i++) out[n++] = prefix[i];
    n += FormatUInt(pid, out + n);
    out[n++] = '_';
    n += FormatUInt((unsigned long long)index, out + n);
}

void CreateChannel(const char *base, Channel *ch) {
    char name[96];

    BuildName(name, base, ".tmp");
    ch->file = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (ch->file == INVALID_HANDLE_VALUE) Fail("CreateFile (backing file)");

    BuildName(name, base, "_map");
    ch->map = CreateFileMappingA(ch->file, NULL, PAGE_READWRITE, 0, sizeof(Shared), name);
    if (ch->map == NULL) Fail("CreateFileMapping");

    ch->view = (Shared *)MapViewOfFile(ch->map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (ch->view == NULL) Fail("MapViewOfFile");
    ch->view->len = 0;
    ch->view->done = 0;

    BuildName(name, base, "_data");
    ch->evData = CreateEventA(NULL, FALSE, FALSE, name);
    if (ch->evData == NULL) Fail("CreateEvent (data)");

    BuildName(name, base, "_free");
    ch->evFree = CreateEventA(NULL, FALSE, TRUE, name);
    if (ch->evFree == NULL) Fail("CreateEvent (free)");
}

void StartChild(const char *exePath, const char *outName, const char *base, Channel *ch) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    char cmd[MAX_PATH + 64];
    DWORD n = 0;
    HANDLE out;

    out = CreateFileA(outName, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) Fail("CreateFile (output)");

    cmd[n++] = '"';
    for (DWORD i = 0; exePath[i] != 0; i++) cmd[n++] = exePath[i];
    cmd[n++] = '"';
    cmd[n++] = ' ';
    for (DWORD i = 0; base[i] != 0; i++) cmd[n++] = base[i];
    cmd[n] = 0;

    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    if (!CreateProcessA(exePath, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) Fail("CreateProcess");

    CloseHandle(out);
    CloseHandle(pi.hThread);
    ch->process = pi.hProcess;
}

void SendChunk(Channel *ch, const char *p, DWORD n, DWORD done) {
    HANDLE h[2];
    DWORD r;
    h[0] = ch->evFree;
    h[1] = ch->process;
    r = WaitForMultipleObjects(2, h, FALSE, INFINITE);
    if (r == WAIT_FAILED) Fail("WaitForMultipleObjects");
    if (r != WAIT_OBJECT_0) {
        Print("Error: child process stopped unexpectedly\n");
        ExitProcess(1);
    }
    for (DWORD i = 0; i < n; i++) ch->view->data[i] = p[i];
    ch->view->len = n;
    ch->view->done = done;
    if (!SetEvent(ch->evData)) Fail("SetEvent");
}

void SendLine(Channel *ch, const char *p, DWORD n) {
    while (n > 0) {
        DWORD part = n < BUF_SIZE ? n : BUF_SIZE;
        SendChunk(ch, p, part, 0);
        p += part;
        n -= part;
    }
}

void CloseChannel(Channel *ch) {
    UnmapViewOfFile(ch->view);
    CloseHandle(ch->map);
    CloseHandle(ch->evData);
    CloseHandle(ch->evFree);
    CloseHandle(ch->file);
}

int main(void) {
    char exePath[MAX_PATH];
    char name1[MAX_PATH], name2[MAX_PATH];
    char base1[48], base2[48];
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE children[2];
    Channel ch1, ch2;
    Line line = {0};
    DWORD n, i, pid;

    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    seed = GetTickCount();

    n = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 10) Fail("GetModuleFileName");
    i = n;
    while (i > 0 && exePath[i - 1] != '\\') i--;
    exePath[i] = 0;
    {
        const char *childName = "child.exe";
        for (DWORD k = 0; k <= StrLen(childName); k++) exePath[i + k] = childName[k];
    }

    ReadFileName(in, "File name for child1: ", name1);
    ReadFileName(in, "File name for child2: ", name2);

    pid = GetCurrentProcessId();
    MakeBase(base1, pid, 1);
    MakeBase(base2, pid, 2);

    CreateChannel(base1, &ch1);
    CreateChannel(base2, &ch2);
    StartChild(exePath, name1, base1, &ch1);
    StartChild(exePath, name2, base2, &ch2);

    Print("Enter lines:\n");
    while (ReadLine(in, &line)) {
        LineAdd(&line, '\n');
        if (RandomPercent() < 80) SendLine(&ch1, line.data, line.len);
        else SendLine(&ch2, line.data, line.len);
    }

    SendChunk(&ch1, NULL, 0, 1);
    SendChunk(&ch2, NULL, 0, 1);

    children[0] = ch1.process;
    children[1] = ch2.process;
    WaitForMultipleObjects(2, children, TRUE, INFINITE);
    CloseHandle(ch1.process);
    CloseHandle(ch2.process);
    CloseChannel(&ch1);
    CloseChannel(&ch2);
    return 0;
}
#include <windows.h>

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

void Fail(const char *what) {
    char num[12];
    int i = 11;
    DWORD code = GetLastError();
    num[i] = 0;
    do {
        i--;
        num[i] = '0' + code % 10;
        code = code / 10;
    } while (code > 0);
    Print("Error: ");
    Print(what);
    Print(" (code ");
    Print(num + i);
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

void WriteBytes(HANDLE h, const char *p, DWORD n) {
    DWORD written;
    if (!WriteFile(h, p, n, &written, NULL)) Fail("WriteFile");
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

void StartChild(const char *exePath, const char *fileName, HANDLE *pipeWrite, HANDLE *process) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE pipeRead, file;
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};

    if (!CreatePipe(&pipeRead, pipeWrite, &sa, 0)) Fail("CreatePipe");
    if (!SetHandleInformation(*pipeWrite, HANDLE_FLAG_INHERIT, 0)) Fail("SetHandleInformation");

    file = CreateFileA(fileName, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) Fail("CreateFile");

    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = pipeRead;
    si.hStdOutput = file;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    if (!CreateProcessA(exePath, NULL, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) Fail("CreateProcess");

    CloseHandle(pipeRead);
    CloseHandle(file);
    CloseHandle(pi.hThread);
    *process = pi.hProcess;
}

int main(void) {
    char exePath[MAX_PATH];
    char name1[MAX_PATH], name2[MAX_PATH];
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE pipe1, pipe2, child1, child2;
    HANDLE children[2];
    Line line = {0};
    DWORD n, i;

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

    StartChild(exePath, name1, &pipe1, &child1);
    StartChild(exePath, name2, &pipe2, &child2);

    Print("Enter lines:\n");
    while (ReadLine(in, &line)) {
        LineAdd(&line, '\n');
        if (RandomPercent() < 80) WriteBytes(pipe1, line.data, line.len);
        else WriteBytes(pipe2, line.data, line.len);
    }

    CloseHandle(pipe1);
    CloseHandle(pipe2);

    children[0] = child1;
    children[1] = child2;
    WaitForMultipleObjects(2, children, TRUE, INFINITE);
    CloseHandle(child1);
    CloseHandle(child2);
    return 0;
}

#include <windows.h>

#define BUF_SIZE 4096

typedef struct {
    DWORD len;
    DWORD done;
    char data[BUF_SIZE];
} Shared;

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

void BuildName(char *out, const char *base, const char *suffix) {
    DWORD n = StrLen(base);
    for (DWORD i = 0; i < n; i++) out[i] = base[i];
    for (DWORD i = 0; i <= StrLen(suffix); i++) out[n + i] = suffix[i];
}

void WriteBytes(HANDLE h, const char *p, DWORD n) {
    DWORD written;
    while (n > 0) {
        if (!WriteFile(h, p, n, &written, NULL)) Fail("WriteFile");
        p += written;
        n -= written;
    }
}

int IsLatinVowel(char c) {
    const char *vowels = "aeiouyAEIOUY";
    for (int i = 0; vowels[i] != 0; i++) {
        if (vowels[i] == c) return 1;
    }
    return 0;
}

int IsRussianVowel(unsigned char first, unsigned char second) {
    const unsigned char d0[] = {0xB0, 0xB5, 0xB8, 0xBE, 0x90, 0x95, 0x98, 0x9E, 0x81, 0xA3, 0xAB, 0xAD, 0xAE, 0xAF};
    const unsigned char d1[] = {0x91, 0x83, 0x8B, 0x8D, 0x8E, 0x8F};
    if (first == 0xD0) {
        for (int i = 0; i < 14; i++) {
            if (d0[i] == second) return 1;
        }
    }
    if (first == 0xD1) {
        for (int i = 0; i < 6; i++) {
            if (d1[i] == second) return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    char name[96];
    HANDLE map, evData, evFree;
    Shared *view;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    char result[BUF_SIZE + 2];
    unsigned char pending = 0;

    if (argc < 2) {
        Print("Usage: child.exe <name>\n");
        return 1;
    }

    BuildName(name, argv[1], "_map");
    map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (map == NULL) Fail("OpenFileMapping");
    view = (Shared *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (view == NULL) Fail("MapViewOfFile");

    BuildName(name, argv[1], "_data");
    evData = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    if (evData == NULL) Fail("OpenEvent (data)");
    BuildName(name, argv[1], "_free");
    evFree = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    if (evFree == NULL) Fail("OpenEvent (free)");

    for (;;) {
        DWORD used = 0;
        if (WaitForSingleObject(evData, INFINITE) != WAIT_OBJECT_0) Fail("WaitForSingleObject");
        if (view->done) break;
        for (DWORD i = 0; i < view->len; i++) {
            unsigned char b = (unsigned char)view->data[i];
            if (pending != 0) {
                if (!IsRussianVowel(pending, b)) {
                    result[used++] = (char)pending;
                    result[used++] = (char)b;
                }
                pending = 0;
            } else if (b == 0xD0 || b == 0xD1) {
                pending = b;
            } else if (!IsLatinVowel((char)b)) {
                result[used++] = (char)b;
            }
        }
        WriteBytes(out, result, used);
        if (!SetEvent(evFree)) Fail("SetEvent");
    }

    UnmapViewOfFile(view);
    CloseHandle(map);
    CloseHandle(evData);
    CloseHandle(evFree);
    return 0;
}
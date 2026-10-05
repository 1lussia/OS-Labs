#include <windows.h>

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

int main(void) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    char c;

    while (ReadByte(in, &c)) {
        if ((unsigned char)c == 0xD0 || (unsigned char)c == 0xD1) {
            char second;
            if (!ReadByte(in, &second)) break;
            if (!IsRussianVowel((unsigned char)c, (unsigned char)second)) {
                WriteBytes(out, &c, 1);
                WriteBytes(out, &second, 1);
            }
        } else if (!IsLatinVowel(c)) {
            WriteBytes(out, &c, 1);
        }
    }
    return 0;
}

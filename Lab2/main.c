#include <windows.h>

#define CHUNK 16384
#define MAX_ITER 300

typedef struct {
    double x;
    double y;
} Point;

typedef struct {
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    int count;
    int total;
    unsigned long long generation;
} Barrier;

struct Context;

typedef struct {
    int id;
    struct Context *ctx;
    double *sumX;
    double *sumY;
    long long *cnt;
    long long changed;
    char padding[64];
} ThreadData;

typedef struct Context {
    volatile LONG64 next;
    char padding[56];
    Point *points;
    long long n;
    int k;
    Point *centroids;
    int *labels;
    int threads;
    int maxIter;
    int iterations;
    int stop;
    ThreadData *data;
    Barrier barrier;
} Context;

HANDLE hOut;
HANDLE hErr;
unsigned long long rngState;

DWORD StrLen(const char *s) {
    DWORD n = 0;
    while (s[n] != 0) n++;
    return n;
}

void WriteAll(HANDLE h, const char *p, DWORD n) {
    DWORD written;
    while (n > 0) {
        if (!WriteFile(h, p, n, &written, NULL)) ExitProcess(2);
        p += written;
        n -= written;
    }
}

void Print(HANDLE h, const char *s) {
    WriteAll(h, s, StrLen(s));
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

void PrintUInt(HANDLE h, unsigned long long v) {
    char buf[24];
    FormatUInt(v, buf);
    Print(h, buf);
}

int FormatDouble(double v, int decimals, char *buf) {
    unsigned long long scale = 1;
    unsigned long long whole, frac, value;
    int i = 0;
    for (int d = 0; d < decimals; d++) scale *= 10;
    if (v < 0) {
        buf[i++] = '-';
        v = -v;
    }
    value = (unsigned long long)(v * (double)scale + 0.5);
    whole = value / scale;
    frac = value % scale;
    i += FormatUInt(whole, buf + i);
    if (decimals > 0) {
        buf[i++] = '.';
        for (int d = decimals - 1; d >= 0; d--) {
            buf[i + d] = (char)('0' + frac % 10);
            frac /= 10;
        }
        i += decimals;
    }
    buf[i] = 0;
    if (buf[0] == '-' && value == 0) {
        for (int j = 0; j < i; j++) buf[j] = buf[j + 1];
        i--;
    }
    return i;
}

void PrintDouble(HANDLE h, double v, int decimals) {
    char buf[48];
    FormatDouble(v, decimals, buf);
    Print(h, buf);
}

void Fail(const char *what) {
    DWORD code = GetLastError();
    Print(hErr, "Error: ");
    Print(hErr, what);
    Print(hErr, " (code ");
    PrintUInt(hErr, code);
    Print(hErr, ")\n");
    ExitProcess(1);
}

void Usage(const char *problem) {
    Print(hErr, "Error: ");
    Print(hErr, problem);
    Print(hErr, "\nUsage: main.exe -t <max threads>\n");
    ExitProcess(1);
}

void *Alloc(unsigned long long bytes) {
    void *p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)bytes);
    if (p == NULL) Fail("HeapAlloc");
    return p;
}

void *AllocPadded(unsigned long long bytes) {
    char *p = Alloc(bytes + 128);
    return p + 64;
}

void FreePadded(void *p) {
    HeapFree(GetProcessHeap(), 0, (char *)p - 64);
}

int ParseLong(const char *s, long long *result) {
    long long v = 0;
    int i = 0;
    if (s[0] == 0) return 0;
    while (s[i] != 0) {
        if (s[i] < '0' || s[i] > '9') return 0;
        if (v > 100000000000LL) return 0;
        v = v * 10 + (s[i] - '0');
        i++;
    }
    *result = v;
    return 1;
}

double Random01(void) {
    rngState ^= rngState >> 12;
    rngState ^= rngState << 25;
    rngState ^= rngState >> 27;
    return (double)((rngState * 2685821657736338717ULL) >> 11) / 9007199254740992.0;
}

Point *GeneratePoints(long long n, int groups) {
    Point *points = Alloc((unsigned long long)n * sizeof(Point));
    Point *centers = Alloc((unsigned long long)groups * sizeof(Point));
    for (int j = 0; j < groups; j++) {
        centers[j].x = Random01() * 2000 - 1000;
        centers[j].y = Random01() * 2000 - 1000;
    }
    for (long long i = 0; i < n; i++) {
        int g = (int)(Random01() * groups);
        double dx = Random01() + Random01() + Random01() + Random01() - 2;
        double dy = Random01() + Random01() + Random01() + Random01() - 2;
        if (g >= groups) g = groups - 1;
        points[i].x = centers[g].x + dx * 80;
        points[i].y = centers[g].y + dy * 80;
    }
    HeapFree(GetProcessHeap(), 0, centers);
    return points;
}

void InitCentroids(const Point *points, long long n, int k, Point *centroids) {
    for (int j = 0; j < k; j++) centroids[j] = points[(long long)j * n / k];
}

int Nearest(Point p, const Point *centroids, int k) {
    int best = 0;
    double bestDist = 0;
    for (int j = 0; j < k; j++) {
        double dx = p.x - centroids[j].x;
        double dy = p.y - centroids[j].y;
        double d = dx * dx + dy * dy;
        if (j == 0 || d < bestDist) {
            bestDist = d;
            best = j;
        }
    }
    return best;
}

int KMeansSequential(const Point *points, long long n, int k, Point *centroids, int *labels, int maxIter) {
    double *sumX = Alloc((unsigned long long)k * sizeof(double));
    double *sumY = Alloc((unsigned long long)k * sizeof(double));
    long long *cnt = Alloc((unsigned long long)k * sizeof(long long));
    int iterations = 0;
    for (long long i = 0; i < n; i++) labels[i] = -1;
    while (iterations < maxIter) {
        long long changed = 0;
        for (int j = 0; j < k; j++) {
            sumX[j] = 0;
            sumY[j] = 0;
            cnt[j] = 0;
        }
        for (long long i = 0; i < n; i++) {
            int c = Nearest(points[i], centroids, k);
            if (labels[i] != c) {
                labels[i] = c;
                changed++;
            }
            sumX[c] += points[i].x;
            sumY[c] += points[i].y;
            cnt[c]++;
        }
        for (int j = 0; j < k; j++) {
            if (cnt[j] > 0) {
                centroids[j].x = sumX[j] / cnt[j];
                centroids[j].y = sumY[j] / cnt[j];
            }
        }
        iterations++;
        if (changed == 0) break;
    }
    HeapFree(GetProcessHeap(), 0, sumX);
    HeapFree(GetProcessHeap(), 0, sumY);
    HeapFree(GetProcessHeap(), 0, cnt);
    return iterations;
}

void BarrierInit(Barrier *b, int total) {
    InitializeCriticalSection(&b->cs);
    InitializeConditionVariable(&b->cv);
    b->count = 0;
    b->total = total;
    b->generation = 0;
}

void BarrierWait(Barrier *b) {
    unsigned long long gen;
    EnterCriticalSection(&b->cs);
    gen = b->generation;
    b->count++;
    if (b->count == b->total) {
        b->count = 0;
        b->generation++;
        WakeAllConditionVariable(&b->cv);
    } else {
        while (gen == b->generation) SleepConditionVariableCS(&b->cv, &b->cs, INFINITE);
    }
    LeaveCriticalSection(&b->cs);
}

void Reduce(Context *ctx) {
    long long changed = 0;
    for (int j = 0; j < ctx->k; j++) {
        double sx = 0, sy = 0;
        long long c = 0;
        for (int t = 0; t < ctx->threads; t++) {
            sx += ctx->data[t].sumX[j];
            sy += ctx->data[t].sumY[j];
            c += ctx->data[t].cnt[j];
        }
        if (c > 0) {
            ctx->centroids[j].x = sx / c;
            ctx->centroids[j].y = sy / c;
        }
    }
    for (int t = 0; t < ctx->threads; t++) changed += ctx->data[t].changed;
    ctx->iterations++;
    if (changed == 0 || ctx->iterations >= ctx->maxIter) ctx->stop = 1;
}

DWORD WINAPI Worker(LPVOID arg) {
    ThreadData *td = (ThreadData *)arg;
    Context *ctx = td->ctx;
    const Point *points = ctx->points;
    const Point *centroids = ctx->centroids;
    int *labels = ctx->labels;
    int k = ctx->k;
    long long n = ctx->n;
    double *sumX = td->sumX;
    double *sumY = td->sumY;
    long long *cnt = td->cnt;
    for (;;) {
        long long changed = 0;
        for (int j = 0; j < k; j++) {
            sumX[j] = 0;
            sumY[j] = 0;
            cnt[j] = 0;
        }
        for (;;) {
            long long begin = InterlockedExchangeAdd64(&ctx->next, CHUNK);
            long long end = begin + CHUNK;
            if (begin >= n) break;
            if (end > n) end = n;
            for (long long i = begin; i < end; i++) {
                int c = Nearest(points[i], centroids, k);
                if (labels[i] != c) {
                    labels[i] = c;
                    changed++;
                }
                sumX[c] += points[i].x;
                sumY[c] += points[i].y;
                cnt[c]++;
            }
        }
        td->changed = changed;
        BarrierWait(&ctx->barrier);
        if (td->id == 0) {
            Reduce(ctx);
            ctx->next = 0;
        }
        BarrierWait(&ctx->barrier);
        if (ctx->stop) break;
    }
    return 0;
}

int KMeansParallel(const Point *points, long long n, int k, Point *centroids, int *labels, int maxIter, int threads) {
    Context ctx;
    HANDLE *handles = Alloc((unsigned long long)threads * sizeof(HANDLE));
    ctx.points = (Point *)points;
    ctx.n = n;
    ctx.k = k;
    ctx.centroids = centroids;
    ctx.labels = labels;
    ctx.threads = threads;
    ctx.maxIter = maxIter;
    ctx.next = 0;
    ctx.iterations = 0;
    ctx.stop = 0;
    ctx.data = Alloc((unsigned long long)threads * sizeof(ThreadData));
    BarrierInit(&ctx.barrier, threads);
    for (long long i = 0; i < n; i++) labels[i] = -1;
    for (int t = 0; t < threads; t++) {
        ctx.data[t].id = t;
        ctx.data[t].ctx = &ctx;
        ctx.data[t].sumX = AllocPadded((unsigned long long)k * sizeof(double));
        ctx.data[t].sumY = AllocPadded((unsigned long long)k * sizeof(double));
        ctx.data[t].cnt = AllocPadded((unsigned long long)k * sizeof(long long));
    }
    for (int t = 0; t < threads; t++) {
        handles[t] = CreateThread(NULL, 64 * 1024, Worker, &ctx.data[t], STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (handles[t] == NULL) Fail("CreateThread");
    }
    for (int t = 0; t < threads; t++) {
        if (WaitForSingleObject(handles[t], INFINITE) == WAIT_FAILED) Fail("WaitForSingleObject");
        CloseHandle(handles[t]);
    }
    for (int t = 0; t < threads; t++) {
        FreePadded(ctx.data[t].sumX);
        FreePadded(ctx.data[t].sumY);
        FreePadded(ctx.data[t].cnt);
    }
    DeleteCriticalSection(&ctx.barrier.cs);
    HeapFree(GetProcessHeap(), 0, ctx.data);
    HeapFree(GetProcessHeap(), 0, handles);
    return ctx.iterations;
}

double NowMs(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

long long ReadNumber(const char *prompt, const char *error) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    char line[32];
    int len = 0;
    char c;
    DWORD got;
    long long v;
    Print(hErr, prompt);
    while (ReadFile(in, &c, 1, &got, NULL) && got == 1) {
        if (c == '\r') continue;
        if (c == '\n') break;
        if (len < 31) line[len++] = c;
    }
    line[len] = 0;
    if (!ParseLong(line, &v) || v < 1) Usage(error);
    return v;
}

void PrintPoint(HANDLE h, Point p) {
    Print(h, "(");
    PrintDouble(h, p.x, 3);
    Print(h, ", ");
    PrintDouble(h, p.y, 3);
    Print(h, ")");
}

int main(int argc, char **argv) {
    long long threads = 0, n = 0, k;
    Point *points, *seqCentroids, *parCentroids;
    int *seqLabels, *parLabels;
    int seqIter, parIter;
    double t0, seqMs, parMs, maxDiff = 0;
    long long mismatched = 0;

    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    hErr = GetStdHandle(STD_ERROR_HANDLE);

    for (int a = 1; a < argc; a++) {
        const char *key = argv[a];
        if (a + 1 >= argc) Usage("missing value after a key");
        if (key[0] == '-' && key[1] == 't' && key[2] == 0) {
            if (!ParseLong(argv[++a], &threads) || threads < 1 || threads > 4096) Usage("-t must be from 1 to 4096");
        } else {
            Usage("unknown key");
        }
    }
    if (threads == 0) Usage("key -t (max threads) is required");
    n = ReadNumber("Number of points: ", "number of points must be a positive integer");
    k = ReadNumber("Number of clusters: ", "number of clusters must be a positive integer");
    if (k > n) Usage("number of clusters is greater than number of points");

    rngState = 6364136223846793005ULL + 1442695040888963407ULL;
    points = GeneratePoints(n, (int)(k < 64 ? k : 64));

    seqCentroids = Alloc((unsigned long long)k * sizeof(Point));
    parCentroids = Alloc((unsigned long long)k * sizeof(Point));
    seqLabels = Alloc((unsigned long long)n * sizeof(int));
    parLabels = Alloc((unsigned long long)n * sizeof(int));
    InitCentroids(points, n, (int)k, seqCentroids);
    InitCentroids(points, n, (int)k, parCentroids);

    t0 = NowMs();
    seqIter = KMeansSequential(points, n, (int)k, seqCentroids, seqLabels, MAX_ITER);
    seqMs = NowMs() - t0;

    t0 = NowMs();
    parIter = KMeansParallel(points, n, (int)k, parCentroids, parLabels, MAX_ITER, (int)threads);
    parMs = NowMs() - t0;

    for (int j = 0; j < k; j++) {
        double dx = seqCentroids[j].x - parCentroids[j].x;
        double dy = seqCentroids[j].y - parCentroids[j].y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > maxDiff) maxDiff = dx;
        if (dy > maxDiff) maxDiff = dy;
    }
    for (long long i = 0; i < n; i++) {
        if (seqLabels[i] != parLabels[i]) mismatched++;
    }

    Print(hOut, "Points: ");
    PrintUInt(hOut, (unsigned long long)n);
    Print(hOut, ", clusters: ");
    PrintUInt(hOut, (unsigned long long)k);
    Print(hOut, ", max threads: ");
    PrintUInt(hOut, (unsigned long long)threads);
    Print(hOut, "\nSequential: ");
    PrintDouble(hOut, seqMs, 3);
    Print(hOut, " ms, iterations: ");
    PrintUInt(hOut, (unsigned long long)seqIter);
    Print(hOut, "\nParallel:   ");
    PrintDouble(hOut, parMs, 3);
    Print(hOut, " ms, iterations: ");
    PrintUInt(hOut, (unsigned long long)parIter);
    Print(hOut, "\nSpeedup:    ");
    PrintDouble(hOut, seqMs / parMs, 3);
    Print(hOut, "\nEfficiency: ");
    PrintDouble(hOut, seqMs / parMs / (double)threads, 3);
    Print(hOut, "\nResults match: ");
    Print(hOut, (mismatched == 0 && maxDiff < 1e-6) ? "yes" : "no");
    Print(hOut, " (different labels: ");
    PrintUInt(hOut, (unsigned long long)mismatched);
    Print(hOut, ")\n");

    for (int j = 0; j < k; j++) {
        long long size = 0;
        for (long long i = 0; i < n; i++) {
            if (parLabels[i] == j) size++;
        }
        Print(hOut, "Cluster ");
        PrintUInt(hOut, (unsigned long long)j + 1);
        Print(hOut, ": center ");
        PrintPoint(hOut, parCentroids[j]);
        Print(hOut, ", points: ");
        PrintUInt(hOut, (unsigned long long)size);
        Print(hOut, "\n");
    }

    if (n <= 100) {
        for (long long i = 0; i < n; i++) {
            PrintPoint(hOut, points[i]);
            Print(hOut, " -> cluster ");
            PrintUInt(hOut, (unsigned long long)parLabels[i] + 1);
            Print(hOut, "\n");
        }
    }
    return 0;
}

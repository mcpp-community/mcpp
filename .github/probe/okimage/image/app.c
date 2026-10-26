/* An ok-format image for the probe: position-independent ELF, compiled for no
 * operating system, importing kal_* and nothing else. The same bytes are run on
 * every system of one architecture by the prototype runner (../runner).
 *
 * What it exercises:
 *   - calls into the runner's openkal implementation with this architecture's
 *     fixed convention (SysV on x86_64, AAPCS64 on aarch64);
 *   - a callback the implementation makes into the image (kal_task_start);
 *   - thread_local through -femulated-tls, served by __emutls_get_address below
 *     on nothing but kal_* (kal_task_current, kal_alloc);
 *   - arguments, a by-value structure (kal_stream), a 64-bit return. */
typedef unsigned long kal_uintptr;
typedef long kal_intptr;
typedef unsigned int kal_u32;
struct kal_stream { kal_uintptr h; };
struct kal_task { kal_uintptr h; };

struct kal_stream kal_stdout(void);
kal_intptr kal_stream_write(struct kal_stream, const void*, kal_uintptr);
kal_uintptr kal_env_arg_count(void);
kal_intptr kal_env_arg(kal_uintptr, char*, kal_uintptr);
int kal_task_start(void (*entry)(void*), void* arg, struct kal_task* out);
int kal_task_join(struct kal_task);
kal_uintptr kal_task_current(void);
void* kal_alloc(kal_uintptr size, kal_uintptr align);

/* The compiler may emit these for struct copies and zeroing. */
void* memcpy(void* d, const void* s, unsigned long n) {
    unsigned char* a = d; const unsigned char* b = s;
    while (n--) *a++ = *b++;
    return d;
}
void* memset(void* d, int c, unsigned long n) {
    unsigned char* a = d;
    while (n--) *a++ = (unsigned char)c;
    return d;
}

static unsigned long len(const char* s) { unsigned long n = 0; while (s[n]) ++n; return n; }
static void put(const char* s) { kal_stream_write(kal_stdout(), s, len(s)); }
static void put_num(long v) {
    char b[24]; int i = 23; b[i] = 0;
    int neg = v < 0; unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    put(b + i);
}

/* ── emulated TLS on kal_* alone ───────────────────────────────────────────
 * Each context is found by kal_task_current() in a small table; each variable
 * gets an index on first use. A real C runtime would do this once, behind
 * whatever per-context word the specification settles on (design §8.3). */
struct emutls_control { unsigned long size, align; union { unsigned long index; void* address; } object; void* value; };
#define MAX_CTX 64
#define MAX_VARS 32
static struct { kal_uintptr id; void* vars[MAX_VARS]; } g_ctx[MAX_CTX];
static unsigned long g_next_index;
static volatile int g_lock;
static void lock(void) { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

void* __emutls_get_address(struct emutls_control* c) {
    unsigned long index = __atomic_load_n(&c->object.index, __ATOMIC_ACQUIRE);
    if (index == 0) {
        lock();
        if (c->object.index == 0) c->object.index = ++g_next_index;
        index = c->object.index;
        unlock();
    }
    const kal_uintptr self = kal_task_current();
    lock();
    int slot = -1;
    for (int i = 0; i < MAX_CTX; ++i) if (g_ctx[i].id == self) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < MAX_CTX; ++i) if (g_ctx[i].id == 0) { g_ctx[i].id = self; slot = i; break; }
    void** var = &g_ctx[slot].vars[index - 1];
    if (!*var) {
        unsigned char* p = kal_alloc(c->size, c->align ? c->align : 8);
        if (c->value) memcpy(p, c->value, c->size); else memset(p, 0, c->size);
        *var = p;
    }
    void* out = *var;
    unlock();
    return out;
}

static __thread long tl_value = 7;

struct job { long set; long seen; kal_u32 done; };
static void worker(void* arg) {
    struct job* j = arg;
    long before = tl_value;          /* each context starts from the initial value */
    tl_value = j->set;
    j->seen = (before == 7 && tl_value == j->set) ? tl_value : -1;
}

/* Many arguments, mixed widths: the convention is fixed per architecture. */
__attribute__((noinline)) static long mix(long a, int b, long c, int d, long e, int f, long g, int h) {
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8;
}

__attribute__((visibility("default"))) int ok_main(void) {
    put("image: hello from the same bytes\n");
    put("image: argc as kal_env_arg_count = "); put_num((long)kal_env_arg_count()); put("\n");
    char a0[256]; kal_intptr n = kal_env_arg(0, a0, sizeof a0 - 1);
    if (n > 0 && n < 255) { a0[n] = 0; put("image: arg0 = "); put(a0); put("\n"); }
    put("image: mix = "); put_num(mix(1, 2, 3, 4, 5, 6, 7, 8)); put(" (expect 204)\n");

    tl_value = 1;
    struct job j1 = {10, 0, 0}, j2 = {20, 0, 0};
    struct kal_task t1, t2;
    int r1 = kal_task_start(worker, &j1, &t1);
    int r2 = kal_task_start(worker, &j2, &t2);
    if (r1 == 0) kal_task_join(t1);
    if (r2 == 0) kal_task_join(t2);
    put("image: tls main="); put_num(tl_value);
    put(" ctx1="); put_num(j1.seen); put(" ctx2="); put_num(j2.seen);
    put(" (expect 1 10 20) start="); put_num(r1); put("/"); put_num(r2); put("\n");
    return (tl_value == 1 && j1.seen == 10 && j2.seen == 20 && mix(1, 2, 3, 4, 5, 6, 7, 8) == 204) ? 0 : 3;
}

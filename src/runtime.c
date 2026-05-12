/*===========================================================================
 *  runtime.c  --  ExtendedC runtime + conservative mark-and-sweep GC
 *
 *  GC design:
 *    Conservative collector -- scans the C stack and registered global slots
 *    for anything that looks like a heap pointer.  No LLVM metadata needed.
 *
 *    Every allocation is tracked in an intrusive singly-linked list via a
 *    hidden ObjHeader prepended to each object.  GC pauses are triggered
 *    automatically when live heap bytes exceed gc_threshold, or manually
 *    via ec_gc_collect().
 *
 *    Stack scan:
 *      The TRUE stack top (highest address, where the stack segment ends)
 *      is read from /proc/self/maps at startup -- this is the only reliable
 *      source on Linux at all optimization levels.  A pthread fallback is
 *      used if /proc is unavailable.
 *
 *      gc_scan_stack() flushes CPU registers to the stack via setjmp, then
 *      scans every aligned word from the current SP up to gc_stack_top.
 *      Because the IR stores every object reference in an alloca slot
 *      (not just in registers), this is precise for all IR-managed
 *      references.
 *
 *    Global object variables are registered with ec_gc_register_global()
 *    so the GC can scan them as additional roots.
 *
 *  Exception design (caller-owns-jmpbuf):
 *    The jmp_buf is allocated by the IR caller as a local alloca.
 *    IR calls ec_try_push(buf*), then @setjmp(buf) directly with the
 *    returns_twice attribute.  ec_throw longjmps into the buf.
 *    ec_catch_msg pops the frame and returns the message.
 *===========================================================================*/

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <setjmp.h>
#include <ctype.h>

/* Forward declaration */
void ec_gc_collect(void);

/* =========================================================================
 *  GC -- object layout
 * =========================================================================*/

#define GC_INITIAL_THRESHOLD   (512UL  * 1024)          /* 512 KB  */
#define GC_GROWTH_FACTOR       2
#define GC_MAX_THRESHOLD       (256UL  * 1024 * 1024)   /* 256 MB  */
#define GC_MAX_GLOBALS         1024
#define GC_MAX_ROOTS           4096

typedef struct ObjHeader {
    struct ObjHeader* next;
    size_t            size;     /* usable bytes after header */
    uint32_t          type_tag; /* 0 = opaque blob */
    uint8_t           marked;
    uint8_t           pinned;
    uint8_t           _pad[2];
} ObjHeader;

#define HDR(p)   (((ObjHeader*)(p)) - 1)
#define DATA(h)  ((void*)((h) + 1))

/* =========================================================================
 *  GC -- state
 * =========================================================================*/

static ObjHeader*  gc_list            = NULL;
static size_t      gc_bytes           = 0;
static size_t      gc_threshold       = GC_INITIAL_THRESHOLD;
static size_t      gc_num_objs        = 0;
static size_t      gc_total_allocs    = 0;
static size_t      gc_total_frees     = 0;
static size_t      gc_num_collections = 0;

/*
 * gc_stack_top: the HIGHEST address of the thread stack segment.
 * On x86-64 Linux the stack grows downward, so the "top" is the largest
 * address -- the far end of the stack mapping.
 *
 * We read this from /proc/self/maps at startup because
 * __builtin_frame_address(0) inside ec_gc_init only gives us main()'s
 * frame address, which is LOWER than the actual stack segment top (saved
 * registers, the dynamic linker's frames, env vars, etc. all live above
 * main on the stack).  Scanning only to main's frame would miss pointers
 * that live in the frames above it.
 */
static uintptr_t   gc_stack_top = 0;

static void**  gc_globals[GC_MAX_GLOBALS];
static int     gc_nglobals = 0;

static void*   gc_roots[GC_MAX_ROOTS];
static int     gc_nroots = 0;

/* =========================================================================
 *  GC -- stack top detection
 * =========================================================================*/

/*
 * Read the true top of the stack segment from /proc/self/maps.
 * The [stack] entry looks like:
 *   7fff12340000-7fff12360000 rwxp ... [stack]
 * The second address (hi) is the exclusive upper bound of the mapping --
 * the true top of the stack on a downward-growing x86-64 stack.
 */
static uintptr_t get_stack_top_from_maps(void) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;

    char line[256];
    uintptr_t result = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "[stack]")) {
            uintptr_t lo, hi;
            if (sscanf(line, "%lx-%lx", &lo, &hi) == 2)
                result = hi;
            break;
        }
    }
    fclose(f);
    return result;
}

/*
 * Fallback: use pthread to get stack base + size.
 * Returns 0 if not available.
 */
static uintptr_t get_stack_top_pthread(void) {
#ifdef _GNU_SOURCE
    pthread_attr_t attr;
    void* stack_addr = NULL;
    size_t stack_size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return 0;
    pthread_attr_getstack(&attr, &stack_addr, &stack_size);
    pthread_attr_destroy(&attr);
    if (stack_addr && stack_size)
        return (uintptr_t)stack_addr + stack_size;
#endif
    return 0;
}

static uintptr_t detect_stack_top(void) {
    uintptr_t top = get_stack_top_from_maps();
    if (top) return top;

    top = get_stack_top_pthread();
    if (top) return top;

    /*
     * Last resort: use the frame address of the caller (main) and add a
     * generous margin.  This is imprecise but better than scanning nothing.
     * 8 MB is the default Linux stack size.
     */
    return (uintptr_t)__builtin_frame_address(0) + (8UL * 1024 * 1024);
}

/* =========================================================================
 *  GC -- mark phase
 * =========================================================================*/

static void gc_mark_val(uintptr_t v);

static void gc_mark_obj(ObjHeader* h) {
    if (!h || h->marked) return;
    h->marked = 1;
    uintptr_t* body = (uintptr_t*)DATA(h);
    size_t nwords = h->size / sizeof(uintptr_t);
    for (size_t i = 0; i < nwords; i++)
        gc_mark_val(body[i]);
}

static void gc_mark_val(uintptr_t v) {
    if (v < 4096) return;                    /* null / small integer guard */
    if (v % sizeof(void*) != 0) return;      /* must be pointer-aligned */
    for (ObjHeader* h = gc_list; h; h = h->next) {
        uintptr_t lo = (uintptr_t)DATA(h);
        uintptr_t hi = lo + h->size;
        if (v >= lo && v < hi) {
            gc_mark_obj(h);
            return;
        }
    }
}

/*
 * Flush CPU registers to the stack via setjmp, then scan every
 * pointer-aligned word from the current stack pointer up to gc_stack_top.
 *
 * Why setjmp?  The C ABI allows callee-saved registers to hold live
 * pointers across calls.  setjmp spills all callee-saved registers onto
 * the current stack frame, making them visible to the linear stack scan.
 *
 * Why __attribute__((noinline))?  The compiler must not inline this into
 * ec_gc_collect; if it did, __builtin_frame_address(0) would return the
 * collector's caller's frame, potentially missing words at the bottom of
 * the scan range.
 */
__attribute__((noinline))
static void gc_scan_stack(void) {
    jmp_buf regs;
    setjmp(regs);  /* spills callee-saved registers */

    /*
     * __builtin_frame_address(0) gives the frame base of THIS function.
     * The actual stack pointer is at or below this address.  We scan from
     * here (the lowest live address in this frame) upward to gc_stack_top.
     *
     * Stack layout on x86-64 (growing downward):
     *
     *   gc_stack_top  (highest address -- top of stack segment)
     *       ...
     *   main's frame
     *       ...
     *   ec_gc_collect's frame
     *   gc_scan_stack's frame   <-- __builtin_frame_address(0)
     *       ...                 <-- actual SP (slightly below frame_address)
     *
     * We scan from frame_address(0) to gc_stack_top, which conservatively
     * covers all live frames above us.
     */
    uintptr_t sp  = (uintptr_t)__builtin_frame_address(0);
    uintptr_t top = gc_stack_top;

    if (sp >= top) {
        /* Defensive: if detection failed, scan a generous 8 MB window */
        top = sp + (8UL * 1024 * 1024);
    }

    for (uintptr_t a = sp; a + sizeof(uintptr_t) <= top; a += sizeof(uintptr_t))
        gc_mark_val(*(uintptr_t*)a);
}

static void gc_mark_globals(void) {
    for (int i = 0; i < gc_nglobals; i++) {
        void** slot = gc_globals[i];
        if (slot && *slot)
            gc_mark_val((uintptr_t)*slot);
    }
}

static void gc_mark_roots(void) {
    for (int i = 0; i < gc_nroots; i++)
        if (gc_roots[i])
            gc_mark_val((uintptr_t)gc_roots[i]);
}

/* =========================================================================
 *  GC -- sweep phase
 * =========================================================================*/

static size_t gc_sweep(void) {
    ObjHeader** pp = &gc_list;
    size_t freed = 0;
    while (*pp) {
        ObjHeader* h = *pp;
        if (h->pinned) {
            h->marked = 0;
            pp = &h->next;
        } else if (!h->marked) {
            *pp = h->next;
            freed         += h->size;
            gc_bytes      -= h->size;
            gc_num_objs--;
            gc_total_frees++;
            free(h);
        } else {
            h->marked = 0;
            pp = &h->next;
        }
    }
    return freed;
}

/* =========================================================================
 *  GC -- public API
 * =========================================================================*/

void ec_gc_init(void) {
    /*
     * Called once from the generated @main before any allocations.
     * Detect the true stack top here so we have the most complete view.
     */
    gc_stack_top  = detect_stack_top();
    gc_threshold  = GC_INITIAL_THRESHOLD;
}

void* ec_new(int64_t size) {
    if (size <= 0) size = 1;

    if (gc_bytes + (size_t)size > gc_threshold)
        ec_gc_collect();

    ObjHeader* h = (ObjHeader*)calloc(1, sizeof(ObjHeader) + (size_t)size);
    if (!h) {
        ec_gc_collect();
        h = (ObjHeader*)calloc(1, sizeof(ObjHeader) + (size_t)size);
        if (!h) { fprintf(stderr, "ec_new: out of memory\n"); exit(1); }
    }

    h->size   = (size_t)size;
    h->marked = 0;
    h->pinned = 0;
    h->next   = gc_list;
    gc_list   = h;

    gc_bytes += (size_t)size;
    gc_num_objs++;
    gc_total_allocs++;

    return DATA(h);
}

void ec_delete(void* ptr) {
    if (!ptr) return;
    ObjHeader* target = HDR(ptr);
    ObjHeader** pp = &gc_list;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            gc_bytes -= target->size;
            gc_num_objs--;
            gc_total_frees++;
            free(target);
            return;
        }
        pp = &(*pp)->next;
    }
    free(ptr);
}

void ec_gc_collect(void) {
    gc_scan_stack();
    gc_mark_globals();
    gc_mark_roots();

    size_t freed = gc_sweep();
    gc_num_collections++;

    if (freed == 0 && gc_bytes > gc_threshold / 2) {
        gc_threshold = (gc_threshold * GC_GROWTH_FACTOR < GC_MAX_THRESHOLD)
                     ?  gc_threshold * GC_GROWTH_FACTOR
                     :  GC_MAX_THRESHOLD;
    }
}

void ec_gc_register_global(void** slot) {
    if (gc_nglobals < GC_MAX_GLOBALS)
        gc_globals[gc_nglobals++] = slot;
}

void ec_gc_push_root(void* ptr) {
    if (gc_nroots < GC_MAX_ROOTS)
        gc_roots[gc_nroots++] = ptr;
}

void ec_gc_pop_root(void)           { if (gc_nroots > 0) gc_nroots--; }
void ec_gc_pin(void* ptr)           { if (ptr) HDR(ptr)->pinned = 1; }
void ec_gc_unpin(void* ptr)         { if (ptr) HDR(ptr)->pinned = 0; }
int64_t ec_gc_live_bytes(void)      { return (int64_t)gc_bytes; }
int64_t ec_gc_live_objects(void)    { return (int64_t)gc_num_objs; }
int64_t ec_gc_num_collections(void) { return (int64_t)gc_num_collections; }

char* ec_gc_stats(void) {
    static char buf[256];
    snprintf(buf, sizeof(buf),
        "GC: live=%zu bytes / %zu objects | "
        "collections=%zu | allocs=%zu | frees=%zu | threshold=%zu KB",
        gc_bytes, gc_num_objs,
        gc_num_collections, gc_total_allocs, gc_total_frees,
        gc_threshold / 1024);
    return buf;
}

/* =========================================================================
 *  Array support
 *
 *  Fat-pointer layout (GC-managed):
 *    bytes [0..7]  = int64 length (element count)
 *    bytes [8..]   = element data (elem_size * length bytes, zero-filled)
 * =========================================================================*/

void* ec_array_new(int64_t elem_size, int64_t length) {
    if (elem_size <= 0) elem_size = 1;
    if (length    <  0) length    = 0;
    int64_t total = 8 + elem_size * length;
    int8_t* arr   = (int8_t*)ec_new(total);
    memcpy(arr, &length, 8);
    return arr;
}

int64_t ec_array_len(void* arr) {
    if (!arr) return 0;
    int64_t len;
    memcpy(&len, arr, 8);
    return len;
}

/* =========================================================================
 *  String operations
 * =========================================================================*/

char* ec_strconcat(char* a, char* b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a), lb = strlen(b);
    char*  out = (char*)ec_new((int64_t)(la + lb + 1));
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    out[la + lb] = '\0';
    return out;
}

char* ec_int_to_str(int32_t n) {
    char* buf = (char*)ec_new(32);
    snprintf(buf, 32, "%d", n);
    return buf;
}

char* ec_float_to_str(float f) {
    char* buf = (char*)ec_new(64);
    snprintf(buf, 64, "%g", (double)f);
    return buf;
}

char* ec_char_to_str(int8_t c) {
    char* buf = (char*)ec_new(2);
    buf[0] = (char)c;
    buf[1] = '\0';
    return buf;
}

int32_t ec_str_len(char* s)           { return s ? (int32_t)strlen(s) : 0; }

char ec_char_at(char* s, int32_t i) {
    if (!s || i < 0 || i >= (int32_t)strlen(s)) return '\0';
    return s[i];
}

char* ec_substring(char* s, int32_t start, int32_t end) {
    if (!s) return (char*)ec_new(1);
    int32_t len = (int32_t)strlen(s);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return (char*)ec_new(1);
    int32_t sz  = end - start;
    char*   out = (char*)ec_new((int64_t)(sz + 1));
    memcpy(out, s + start, (size_t)sz);
    out[sz] = '\0';
    return out;
}

/* =========================================================================
 *  I/O
 * =========================================================================*/

void  ec_print(char* s)   { if (s) fputs(s, stdout); }
void  ec_println(char* s) { if (s) fputs(s, stdout); fputc('\n', stdout); }

char* ec_readline(void) {
    char* buf = (char*)ec_new(4096);
    int c, i = 0;
    while ((c = getchar()) != EOF && c != '\n' && i < 4094)
        buf[i++] = (char)c;
    buf[i] = '\0';
    return buf;
}

/* =========================================================================
 *  Reflection
 * =========================================================================*/

char* ec_typeof(char* typeName) {
    if (!typeName) return (char*)ec_new(1);
    size_t n = strlen(typeName) + 1;
    char*  out = (char*)ec_new((int64_t)n);
    memcpy(out, typeName, n);
    return out;
}

/* =========================================================================
 *  Exception support  (caller-owns-jmpbuf)
 *
 *  Protocol:
 *    IR allocates jmp_buf as a local alloca, calls ec_try_push(buf_ptr),
 *    then calls @setjmp(buf) directly (declared returns_twice in IR).
 *    On throw: ec_throw stores the message and longjmps into the buf.
 *    On catch: ec_catch_msg pops the frame and returns the message.
 *    On success: ec_try_pop / ec_try_end pops the frame.
 * =========================================================================*/

#define MAX_TRY_DEPTH 64

static jmp_buf* tryStack[MAX_TRY_DEPTH];
static char*    tryMsg  [MAX_TRY_DEPTH];
static int      tryTop = -1;
static char     lastMsg[4096];

void ec_try_push(void* buf) {
    if (tryTop + 1 >= MAX_TRY_DEPTH) {
        fprintf(stderr, "ec_try_push: try nesting too deep\n");
        exit(1);
    }
    ++tryTop;
    tryStack[tryTop] = (jmp_buf*)buf;
    tryMsg  [tryTop] = NULL;
}

void ec_try_pop(void) {
    if (tryTop >= 0) { tryMsg[tryTop] = NULL; --tryTop; }
}

void ec_throw(char* msg) {
    if (tryTop >= 0) {
        tryMsg[tryTop] = msg ? msg : "<exception>";
        longjmp(*tryStack[tryTop], 1);
    }
    fprintf(stderr, "Unhandled exception: %s\n", msg ? msg : "<exception>");
    exit(1);
}

char* ec_catch_msg(void) {
    if (tryTop >= 0) {
        const char* src = tryMsg[tryTop] ? tryMsg[tryTop] : "<unknown exception>";
        strncpy(lastMsg, src, sizeof(lastMsg) - 1);
        lastMsg[sizeof(lastMsg) - 1] = '\0';
        tryMsg[tryTop] = NULL;
        --tryTop;
        return lastMsg;
    }
    return "<no exception>";
}

void ec_try_end(void) { ec_try_pop(); }

/* =========================================================================
 *  File I/O
 * =========================================================================*/

/* Read entire file into a GC-managed string. Returns null on error. */
char* ec_read_file(char* path) {
    if (!path) return NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char* buf = (char*)ec_new((int64_t)(sz + 1));
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* Write string to file. Returns 1 on success, 0 on failure. */
int32_t ec_write_file(char* path, char* content) {
    if (!path || !content) return 0;
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t len = strlen(content);
    int ok = (fwrite(content, 1, len, f) == len);
    fclose(f);
    return ok ? 1 : 0;
}

/* Check if file exists. Returns 1 if yes, 0 if no. */
int32_t ec_file_exists(char* path) {
    if (!path) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* =========================================================================
 *  String operations -- extended
 * =========================================================================*/

/* Find first occurrence of 'needle' in 'haystack'. Returns index or -1. */
int32_t ec_str_find(char* haystack, char* needle) {
    if (!haystack || !needle) return -1;
    char* p = strstr(haystack, needle);
    if (!p) return -1;
    return (int32_t)(p - haystack);
}

/* Find last occurrence of 'needle' in 'haystack'. Returns index or -1. */
int32_t ec_str_find_last(char* haystack, char* needle) {
    if (!haystack || !needle || !*needle) return -1;
    int32_t last = -1, nlen = (int32_t)strlen(needle);
    char* p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        last = (int32_t)(p - haystack);
        p += nlen;
    }
    return last;
}

int32_t ec_str_starts_with(char* s, char* prefix) {
    if (!s || !prefix) return 0;
    size_t plen = strlen(prefix);
    return strncmp(s, prefix, plen) == 0 ? 1 : 0;
}

int32_t ec_str_ends_with(char* s, char* suffix) {
    if (!s || !suffix) return 0;
    size_t slen = strlen(s), suflen = strlen(suffix);
    if (suflen > slen) return 0;
    return strcmp(s + slen - suflen, suffix) == 0 ? 1 : 0;
}

int32_t ec_str_contains(char* s, char* sub) {
    if (!s || !sub) return 0;
    return strstr(s, sub) ? 1 : 0;
}

int32_t ec_str_equals(char* a, char* b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    return strcmp(a, b) == 0 ? 1 : 0;
}

int32_t ec_str_compare(char* a, char* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return  1;
    return strcmp(a, b);
}

char* ec_str_to_lower(char* s) {
    if (!s) return (char*)ec_new(1);
    size_t n = strlen(s);
    char* out = (char*)ec_new((int64_t)(n + 1));
    for (size_t i = 0; i < n; i++)
        out[i] = (char)tolower((unsigned char)s[i]);
    out[n] = '\0';
    return out;
}

char* ec_str_to_upper(char* s) {
    if (!s) return (char*)ec_new(1);
    size_t n = strlen(s);
    char* out = (char*)ec_new((int64_t)(n + 1));
    for (size_t i = 0; i < n; i++)
        out[i] = (char)toupper((unsigned char)s[i]);
    out[n] = '\0';
    return out;
}

char* ec_str_trim(char* s) {
    if (!s) return (char*)ec_new(1);
    const char* start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    const char* end = s + strlen(s);
    while (end > start && isspace((unsigned char)*(end-1))) end--;
    size_t n = (size_t)(end - start);
    char* out = (char*)ec_new((int64_t)(n + 1));
    memcpy(out, start, n);
    out[n] = '\0';
    return out;
}

char* ec_str_trim_left(char* s) {
    if (!s) return (char*)ec_new(1);
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    char* out = (char*)ec_new((int64_t)(n + 1));
    memcpy(out, s, n + 1);
    return out;
}

char* ec_str_trim_right(char* s) {
    if (!s) return (char*)ec_new(1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n-1])) n--;
    char* out = (char*)ec_new((int64_t)(n + 1));
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

char* ec_str_repeat(char* s, int32_t times) {
    if (!s || times <= 0) return (char*)ec_new(1);
    size_t slen = strlen(s);
    size_t total = slen * (size_t)times;
    char* out = (char*)ec_new((int64_t)(total + 1));
    for (int32_t i = 0; i < times; i++)
        memcpy(out + i * slen, s, slen);
    out[total] = '\0';
    return out;
}

char* ec_str_replace(char* s, char* from, char* to) {
    if (!s || !from || !to || !*from) {
        if (!s) return (char*)ec_new(1);
        size_t n = strlen(s) + 1;
        char* out = (char*)ec_new((int64_t)n);
        memcpy(out, s, n);
        return out;
    }
    size_t fromlen = strlen(from), tolen = strlen(to);
    /* Count occurrences */
    int32_t count = 0;
    char* p = s;
    while ((p = strstr(p, from))) { count++; p += fromlen; }
    size_t newlen = strlen(s) + (size_t)count * (tolen - fromlen);
    char* out = (char*)ec_new((int64_t)(newlen + 1));
    char* dst = out;
    p = s;
    char* found;
    while ((found = strstr(p, from))) {
        size_t pre = (size_t)(found - p);
        memcpy(dst, p, pre); dst += pre;
        memcpy(dst, to, tolen); dst += tolen;
        p = found + fromlen;
    }
    strcpy(dst, p);
    return out;
}

char* ec_str_reverse(char* s) {
    if (!s) return (char*)ec_new(1);
    size_t n = strlen(s);
    char* out = (char*)ec_new((int64_t)(n + 1));
    for (size_t i = 0; i < n; i++) out[i] = s[n - 1 - i];
    out[n] = '\0';
    return out;
}

/* Split s by delim, returns GC fat-pointer array of strings */
void* ec_str_split(char* s, char* delim) {
    if (!s || !delim || !*delim) {
        /* Return single-element array with the whole string */
        void* arr = ec_array_new(8, 1);
        int8_t* data = (int8_t*)arr + 8;
        char* copy = (char*)ec_new((int64_t)(strlen(s) + 1));
        if (s) strcpy(copy, s);
        memcpy(data, &copy, sizeof(char*));
        return arr;
    }
    size_t dlen = strlen(delim);
    /* Count parts */
    int32_t count = 1;
    char* p = s;
    while ((p = strstr(p, delim))) { count++; p += dlen; }
    /* Build array */
    void* arr = ec_array_new(8, (int64_t)count);
    int8_t* data = (int8_t*)arr + 8;
    p = s; int32_t i = 0;
    char* found;
    while ((found = strstr(p, delim))) {
        size_t partlen = (size_t)(found - p);
        char* part = (char*)ec_new((int64_t)(partlen + 1));
        memcpy(part, p, partlen); part[partlen] = '\0';
        memcpy(data + i * sizeof(char*), &part, sizeof(char*));
        p = found + dlen; i++;
    }
    /* Last part */
    size_t lastlen = strlen(p);
    char* last = (char*)ec_new((int64_t)(lastlen + 1));
    memcpy(last, p, lastlen); last[lastlen] = '\0';
    memcpy(data + i * sizeof(char*), &last, sizeof(char*));
    return arr;
}

/* Join array of strings with separator */
char* ec_str_join(void* arr, char* sep) {
    if (!arr) return (char*)ec_new(1);
    int64_t n = ec_array_len(arr);
    if (n == 0) return (char*)ec_new(1);
    if (!sep) sep = "";
    size_t seplen = strlen(sep);
    /* Calculate total length */
    size_t total = 0;
    int8_t* data = (int8_t*)arr + 8;
    for (int64_t i = 0; i < n; i++) {
        char* elem = NULL;
        memcpy(&elem, data + i * sizeof(char*), sizeof(char*));
        if (elem) total += strlen(elem);
        if (i < n - 1) total += seplen;
    }
    char* out = (char*)ec_new((int64_t)(total + 1));
    char* dst = out;
    for (int64_t i = 0; i < n; i++) {
        char* elem = NULL;
        memcpy(&elem, data + i * sizeof(char*), sizeof(char*));
        if (elem) { size_t el = strlen(elem); memcpy(dst, elem, el); dst += el; }
        if (i < n - 1 && seplen > 0) { memcpy(dst, sep, seplen); dst += seplen; }
    }
    *dst = '\0';
    return out;
}

int32_t ec_str_count(char* s, char* sub) {
    if (!s || !sub || !*sub) return 0;
    int32_t count = 0; size_t slen = strlen(sub);
    char* p = s;
    while ((p = strstr(p, sub))) { count++; p += slen; }
    return count;
}

/* =========================================================================
 *  Char operations
 * =========================================================================*/

int32_t ec_char_is_digit(int8_t c)  { return isdigit((unsigned char)c)  ? 1 : 0; }
int32_t ec_char_is_alpha(int8_t c)  { return isalpha((unsigned char)c)  ? 1 : 0; }
int32_t ec_char_is_alnum(int8_t c)  { return isalnum((unsigned char)c)  ? 1 : 0; }
int32_t ec_char_is_space(int8_t c)  { return isspace((unsigned char)c)  ? 1 : 0; }
int32_t ec_char_is_upper(int8_t c)  { return isupper((unsigned char)c)  ? 1 : 0; }
int32_t ec_char_is_lower(int8_t c)  { return islower((unsigned char)c)  ? 1 : 0; }
int32_t ec_char_is_punct(int8_t c)  { return ispunct((unsigned char)c)  ? 1 : 0; }
int8_t  ec_char_to_lower_fn(int8_t c) { return (int8_t)tolower((unsigned char)c); }
int8_t  ec_char_to_upper_fn(int8_t c) { return (int8_t)toupper((unsigned char)c); }
int32_t ec_char_code(int8_t c)      { return (int32_t)(unsigned char)c; }
int8_t  ec_char_from_code(int32_t n){ return (int8_t)(n & 0xFF); }

/* =========================================================================
 *  Math -- extended
 * =========================================================================*/

int32_t ec_abs_int(int32_t n)  { return n < 0 ? -n : n; }
int64_t ec_abs_long(int64_t n) { return n < 0 ? -n : n; }
int32_t ec_max_int(int32_t a, int32_t b) { return a > b ? a : b; }
int32_t ec_min_int(int32_t a, int32_t b) { return a < b ? a : b; }
int64_t ec_max_long(int64_t a, int64_t b){ return a > b ? a : b; }
int64_t ec_min_long(int64_t a, int64_t b){ return a < b ? a : b; }
double  ec_max_double(double a, double b) { return a > b ? a : b; }
double  ec_min_double(double a, double b) { return a < b ? a : b; }
int32_t ec_clamp_int(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
double ec_clamp_double(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int32_t ec_gcd(int32_t a, int32_t b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int32_t t = b; b = a % b; a = t; }
    return a;
}
int32_t ec_lcm(int32_t a, int32_t b) {
    int32_t g = ec_gcd(a, b);
    return g ? (a / g) * b : 0;
}
int32_t ec_is_even(int32_t n) { return (n % 2 == 0) ? 1 : 0; }
int32_t ec_is_odd(int32_t n)  { return (n % 2 != 0) ? 1 : 0; }
int32_t ec_is_prime(int32_t n) {
    if (n < 2) return 0;
    if (n == 2) return 1;
    if (n % 2 == 0) return 0;
    for (int32_t i = 3; (int64_t)i * i <= n; i += 2)
        if (n % i == 0) return 0;
    return 1;
}
int32_t ec_pow_int(int32_t base, int32_t exp) {
    int32_t result = 1;
    for (int32_t i = 0; i < exp; i++) result *= base;
    return result;
}
double ec_infinity(void) { return INFINITY; }
double ec_nan(void)      { return NAN; }
double ec_cbrt_fn(double x) { return cbrt(x); }
double ec_hypot_fn(double a, double b) { return hypot(a, b); }
double ec_log2_fn(double x)  { return log2(x); }
double ec_log10_fn(double x) { return log10(x); }
double ec_atan2_fn(double y, double x) { return atan2(y, x); }

/* =========================================================================
 *  List  (type-erased growable array of i8* pointers, GC-managed)
 *
 *  Layout: ec_new allocation containing:
 *    [0..7]   = int64 length  (number of elements)
 *    [8..15]  = int64 capacity
 *    [16..]   = i8* data[]    (capacity * 8 bytes)
 *
 *  Elements are i8* -- callers cast to/from their actual types.
 *  The GC sees the list allocation and its interior pointers.
 * =========================================================================*/

#define LIST_HEADER_SIZE 16  /* length (8) + capacity (8) */
#define LIST_INIT_CAP    8

static void* list_raw_data(void* list) {
    return (int8_t*)list + LIST_HEADER_SIZE;
}
static int64_t list_get_len(void* list)  {
    int64_t v; memcpy(&v, list, 8); return v;
}
static int64_t list_get_cap(void* list)  {
    int64_t v; memcpy(&v, (int8_t*)list + 8, 8); return v;
}
static void list_set_len(void* list, int64_t v) { memcpy(list, &v, 8); }
static void list_set_cap(void* list, int64_t v) { memcpy((int8_t*)list + 8, &v, 8); }

void* ec_list_new(void) {
    int64_t cap  = LIST_INIT_CAP;
    int64_t size = LIST_HEADER_SIZE + cap * 8;
    void* list = ec_new(size);
    list_set_len(list, 0);
    list_set_cap(list, cap);
    return list;
}

int64_t ec_list_len(void* list) {
    if (!list) return 0;
    return list_get_len(list);
}

void* ec_list_get(void* list, int64_t idx) {
    if (!list) return NULL;
    int64_t len = list_get_len(list);
    if (idx < 0 || idx >= len) return NULL;
    void* elem = NULL;
    memcpy(&elem, (int8_t*)list_raw_data(list) + idx * 8, 8);
    return elem;
}

/* Returns a NEW list (GC may have moved things -- the old pointer is invalid).
   Caller must re-assign: myList = ec_list_add(myList, item) */
void* ec_list_add(void* list, void* item) {
    if (!list) list = ec_list_new();
    int64_t len = list_get_len(list);
    int64_t cap = list_get_cap(list);
    if (len >= cap) {
        /* Grow: double capacity, allocate new list */
        int64_t newcap  = cap * 2;
        int64_t newsize = LIST_HEADER_SIZE + newcap * 8;
        void* newlist   = ec_new(newsize);
        list_set_len(newlist, len);
        list_set_cap(newlist, newcap);
        memcpy(list_raw_data(newlist), list_raw_data(list), (size_t)(len * 8));
        list = newlist;
    }
    memcpy((int8_t*)list_raw_data(list) + len * 8, &item, 8);
    list_set_len(list, len + 1);
    return list;
}

void* ec_list_set(void* list, int64_t idx, void* item) {
    if (!list) return list;
    int64_t len = list_get_len(list);
    if (idx < 0 || idx >= len) return list;
    memcpy((int8_t*)list_raw_data(list) + idx * 8, &item, 8);
    return list;
}

void* ec_list_remove(void* list, int64_t idx) {
    if (!list) return list;
    int64_t len = list_get_len(list);
    if (idx < 0 || idx >= len) return list;
    int8_t* data = (int8_t*)list_raw_data(list);
    memmove(data + idx * 8, data + (idx + 1) * 8, (size_t)((len - idx - 1) * 8));
    list_set_len(list, len - 1);
    return list;
}

void* ec_list_insert(void* list, int64_t idx, void* item) {
    if (!list) list = ec_list_new();
    int64_t len = list_get_len(list);
    if (idx < 0) idx = 0;
    if (idx > len) idx = len;
    /* Add to grow if needed */
    list = ec_list_add(list, NULL);  /* extends len and possibly reallocates */
    len = list_get_len(list);
    int8_t* data = (int8_t*)list_raw_data(list);
    /* Shift elements right */
    memmove(data + (idx + 1) * 8, data + idx * 8, (size_t)((len - 1 - idx) * 8));
    memcpy(data + idx * 8, &item, 8);
    return list;
}

int64_t ec_list_index_of(void* list, void* item) {
    if (!list) return -1;
    int64_t len = list_get_len(list);
    int8_t* data = (int8_t*)list_raw_data(list);
    for (int64_t i = 0; i < len; i++) {
        void* elem = NULL;
        memcpy(&elem, data + i * 8, 8);
        if (elem == item) return i;
    }
    return -1;
}

int32_t ec_list_contains(void* list, void* item) {
    return ec_list_index_of(list, item) >= 0 ? 1 : 0;
}

void* ec_list_slice(void* list, int64_t start, int64_t end) {
    if (!list) return ec_list_new();
    int64_t len = list_get_len(list);
    if (start < 0) start = 0;
    if (end > len) end = len;
    void* result = ec_list_new();
    int8_t* data = (int8_t*)list_raw_data(list);
    for (int64_t i = start; i < end; i++) {
        void* elem = NULL;
        memcpy(&elem, data + i * 8, 8);
        result = ec_list_add(result, elem);
    }
    return result;
}

void* ec_list_concat(void* a, void* b) {
    if (!a) return b ? b : ec_list_new();
    if (!b) return a;
    int64_t alen = list_get_len(a), blen = list_get_len(b);
    void* result = ec_list_new();
    int8_t* adata = (int8_t*)list_raw_data(a);
    int8_t* bdata = (int8_t*)list_raw_data(b);
    for (int64_t i = 0; i < alen; i++) {
        void* elem = NULL; memcpy(&elem, adata + i * 8, 8);
        result = ec_list_add(result, elem);
    }
    for (int64_t i = 0; i < blen; i++) {
        void* elem = NULL; memcpy(&elem, bdata + i * 8, 8);
        result = ec_list_add(result, elem);
    }
    return result;
}

/* =========================================================================
 *  Map  (string → i8* open-addressing hash map, GC-managed)
 *
 *  Layout: ec_new allocation containing:
 *    [0..7]   = int64 count   (number of live entries)
 *    [8..15]  = int64 cap     (number of buckets, always power of 2)
 *    [16..]   = Bucket[cap]   where Bucket = { char* key (8B), i8* val (8B) }
 *
 *  Tombstones: key == (char*)-1 (deleted slot)
 *  Empty:      key == NULL
 * =========================================================================*/

#define MAP_HEADER_SIZE  16
#define MAP_BUCKET_SIZE  16   /* key(8) + val(8) */
#define MAP_INIT_CAP     16
#define MAP_LOAD_NUM     3
#define MAP_LOAD_DEN     4    /* resize when count > cap * 3/4 */
#define MAP_TOMBSTONE    ((char*)(intptr_t)-1)

typedef struct { char* key; void* val; } MapBucket;

static MapBucket* map_buckets(void* map) {
    return (MapBucket*)((int8_t*)map + MAP_HEADER_SIZE);
}
static int64_t map_count(void* map) { int64_t v; memcpy(&v, map, 8); return v; }
static int64_t map_cap(void* map)   { int64_t v; memcpy(&v, (int8_t*)map+8, 8); return v; }
static void map_set_count(void* map, int64_t v) { memcpy(map, &v, 8); }
static void map_set_cap(void* map, int64_t v)   { memcpy((int8_t*)map+8, &v, 8); }

static uint64_t map_hash(const char* s) {
    /* FNV-1a 64-bit */
    uint64_t h = 14695981039346656037ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

void* ec_map_new(void) {
    int64_t cap = MAP_INIT_CAP;
    void* map = ec_new((int64_t)(MAP_HEADER_SIZE + cap * MAP_BUCKET_SIZE));
    map_set_count(map, 0);
    map_set_cap(map, cap);
    /* Buckets are zero-filled by ec_new (calloc) */
    return map;
}

static void* map_grow(void* map);

void* ec_map_set(void* map, char* key, void* val) {
    if (!map) map = ec_map_new();
    if (!key) return map;

    int64_t count = map_count(map), cap = map_cap(map);
    /* Resize if over load factor */
    if (count + 1 > cap * MAP_LOAD_NUM / MAP_LOAD_DEN)
        map = map_grow(map);

    cap = map_cap(map);
    MapBucket* buckets = map_buckets(map);
    uint64_t idx = map_hash(key) & (uint64_t)(cap - 1);
    int64_t tombstone = -1;

    for (;;) {
        MapBucket* b = &buckets[idx];
        if (!b->key) {
            /* Empty slot */
            if (tombstone >= 0) b = &buckets[tombstone];
            else map_set_count(map, count + 1);
            size_t klen = strlen(key) + 1;
            char* k = (char*)ec_new((int64_t)klen);
            memcpy(k, key, klen);
            b->key = k;
            b->val = val;
            return map;
        }
        if (b->key == MAP_TOMBSTONE) {
            if (tombstone < 0) tombstone = (int64_t)idx;
        } else if (strcmp(b->key, key) == 0) {
            b->val = val;
            return map;
        }
        idx = (idx + 1) & (uint64_t)(cap - 1);
    }
}

void* ec_map_get(void* map, char* key) {
    if (!map || !key) return NULL;
    int64_t cap = map_cap(map);
    MapBucket* buckets = map_buckets(map);
    uint64_t idx = map_hash(key) & (uint64_t)(cap - 1);
    for (;;) {
        MapBucket* b = &buckets[idx];
        if (!b->key) return NULL;
        if (b->key != MAP_TOMBSTONE && strcmp(b->key, key) == 0)
            return b->val;
        idx = (idx + 1) & (uint64_t)(cap - 1);
    }
}

int32_t ec_map_has(void* map, char* key) {
    if (!map || !key) return 0;
    int64_t cap = map_cap(map);
    MapBucket* buckets = map_buckets(map);
    uint64_t idx = map_hash(key) & (uint64_t)(cap - 1);
    for (;;) {
        MapBucket* b = &buckets[idx];
        if (!b->key) return 0;
        if (b->key != MAP_TOMBSTONE && strcmp(b->key, key) == 0) return 1;
        idx = (idx + 1) & (uint64_t)(cap - 1);
    }
}

void* ec_map_delete(void* map, char* key) {
    if (!map || !key) return map;
    int64_t cap = map_cap(map);
    MapBucket* buckets = map_buckets(map);
    uint64_t idx = map_hash(key) & (uint64_t)(cap - 1);
    for (;;) {
        MapBucket* b = &buckets[idx];
        if (!b->key) return map;
        if (b->key != MAP_TOMBSTONE && strcmp(b->key, key) == 0) {
            b->key = MAP_TOMBSTONE;
            b->val = NULL;
            map_set_count(map, map_count(map) - 1);
            return map;
        }
        idx = (idx + 1) & (uint64_t)(cap - 1);
    }
}

int64_t ec_map_count(void* map) { return map ? map_count(map) : 0; }

/* Returns GC array of string keys */
void* ec_map_keys(void* map) {
    void* result = ec_list_new();
    if (!map) return result;
    int64_t cap = map_cap(map);
    MapBucket* buckets = map_buckets(map);
    for (int64_t i = 0; i < cap; i++) {
        if (buckets[i].key && buckets[i].key != MAP_TOMBSTONE)
            result = ec_list_add(result, buckets[i].key);
    }
    return result;
}

/* Returns GC array of values */
void* ec_map_values(void* map) {
    void* result = ec_list_new();
    if (!map) return result;
    int64_t cap = map_cap(map);
    MapBucket* buckets = map_buckets(map);
    for (int64_t i = 0; i < cap; i++) {
        if (buckets[i].key && buckets[i].key != MAP_TOMBSTONE)
            result = ec_list_add(result, buckets[i].val);
    }
    return result;
}

static void* map_grow(void* map) {
    int64_t oldcap = map_cap(map);
    int64_t newcap = oldcap * 2;
    void* newmap = ec_new((int64_t)(MAP_HEADER_SIZE + newcap * MAP_BUCKET_SIZE));
    map_set_count(newmap, 0);
    map_set_cap(newmap, newcap);
    MapBucket* old = map_buckets(map);
    for (int64_t i = 0; i < oldcap; i++) {
        if (old[i].key && old[i].key != MAP_TOMBSTONE)
            newmap = ec_map_set(newmap, old[i].key, old[i].val);
    }
    return newmap;
}

/* =========================================================================
 *  Set  (string set, backed by Map with NULL values)
 * =========================================================================*/

void* ec_set_new(void)                       { return ec_map_new(); }
void* ec_set_add(void* set, char* key)       { return ec_map_set(set, key, (void*)(intptr_t)1); }
int32_t ec_set_has(void* set, char* key)     { return ec_map_has(set, key); }
void* ec_set_remove(void* set, char* key)    { return ec_map_delete(set, key); }
int64_t ec_set_count(void* set)              { return ec_map_count(set); }
void* ec_set_keys(void* set)                 { return ec_map_keys(set); }

/* =========================================================================
 *  StringBuilder  (efficient mutable string buffer, GC-managed)
 *
 *  Layout:
 *    [0..7]   = int64 length   (bytes written, not including null)
 *    [8..15]  = int64 capacity (allocated bytes after header)
 *    [16..]   = char data[]
 * =========================================================================*/

#define SB_HEADER_SIZE   16
#define SB_INIT_CAP      64

static int64_t sb_len(void* sb) { int64_t v; memcpy(&v, sb, 8); return v; }
static int64_t sb_cap(void* sb) { int64_t v; memcpy(&v, (int8_t*)sb+8, 8); return v; }
static void sb_set_len(void* sb, int64_t v) { memcpy(sb, &v, 8); }
static void sb_set_cap(void* sb, int64_t v) { memcpy((int8_t*)sb+8, &v, 8); }
static char* sb_data(void* sb) { return (char*)((int8_t*)sb + SB_HEADER_SIZE); }

void* ec_sb_new(void) {
    int64_t cap = SB_INIT_CAP;
    void* sb = ec_new((int64_t)(SB_HEADER_SIZE + cap + 1));
    sb_set_len(sb, 0);
    sb_set_cap(sb, cap);
    return sb;
}

void* ec_sb_append_str(void* sb, char* s) {
    if (!sb) sb = ec_sb_new();
    if (!s) return sb;
    int64_t slen = (int64_t)strlen(s);
    int64_t len = sb_len(sb), cap = sb_cap(sb);
    if (len + slen + 1 > cap) {
        int64_t newcap = (cap + slen + 1) * 2;
        void* newsb = ec_new((int64_t)(SB_HEADER_SIZE + newcap + 1));
        sb_set_len(newsb, len);
        sb_set_cap(newsb, newcap);
        memcpy(sb_data(newsb), sb_data(sb), (size_t)len);
        sb = newsb;
    }
    memcpy(sb_data(sb) + len, s, (size_t)slen);
    sb_set_len(sb, len + slen);
    sb_data(sb)[len + slen] = '\0';
    return sb;
}

void* ec_sb_append_char(void* sb, int8_t c) {
    char buf[2] = { (char)c, '\0' };
    return ec_sb_append_str(sb, buf);
}

void* ec_sb_append_int(void* sb, int32_t n) {
    char buf[32]; snprintf(buf, sizeof(buf), "%d", n);
    return ec_sb_append_str(sb, buf);
}

void* ec_sb_append_long(void* sb, int64_t n) {
    char buf[32]; snprintf(buf, sizeof(buf), "%ld", n);
    return ec_sb_append_str(sb, buf);
}

char* ec_sb_to_string(void* sb) {
    if (!sb) return (char*)ec_new(1);
    int64_t len = sb_len(sb);
    char* out = (char*)ec_new(len + 1);
    memcpy(out, sb_data(sb), (size_t)len);
    out[len] = '\0';
    return out;
}

int64_t ec_sb_length(void* sb) { return sb ? sb_len(sb) : 0; }

void* ec_sb_clear(void* sb) {
    if (!sb) return ec_sb_new();
    sb_set_len(sb, 0);
    sb_data(sb)[0] = '\0';
    return sb;
}

/* =========================================================================
 *  System
 * =========================================================================*/

void ec_exit(int32_t code)  { exit(code); }
int32_t lang_argc(void)       { return 0; }  /* populated by driver */
char* lang_argv(int32_t i)    { (void)i; return (char*)ec_new(1); }

/* Monotonic time in milliseconds (for benchmarking) */
int64_t ec_time_ms(void) {
#if defined(_POSIX_TIMERS)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
    return 0;
#endif
}

/* =========================================================================
 *  Threading primitives
 *  
 *  Future: GC allocation layout:
 *    [0..7]   = i8*  result  (NULL until done)
 *    [8..15]  = i32  done    (0=running, 1=done)
 *    [16..23] = i64  pthread_t handle
 *    [24..31] = i32  detached (1=detached/fire-and-forget)
 *
 *  Mutex: GC allocation wrapping a pthread_mutex_t
 *    [0..39]  = pthread_mutex_t (40 bytes on Linux x86-64)
 *
 *  Thread entry trampoline passes {fn_ptr, env_ptr, future_ptr} struct
 *  so the spawned thread can write its result to the Future before exit.
 * =========================================================================*/

#define FUTURE_HEADER  32   /* result(8) + done(8) + pthread_t(8) + pad(8) */
#define MUTEX_SIZE     40   /* sizeof(pthread_mutex_t) on Linux x86-64      */

/* ── Thread trampoline ───────────────────────────────────────────────────── */

typedef struct {
    void* (*fn)(void*);   /* the lambda function: fn(env) → i8*  */
    void*  env;           /* closure env pointer                  */
    void*  future;        /* the Future GC object                 */
} ThreadArg;

static void* thread_trampoline(void* raw) {
    ThreadArg* arg = (ThreadArg*)raw;
    void* result = arg->fn(arg->env);

    /* Write result and mark done */
    if (arg->future) {
        memcpy(arg->future, &result, 8);
        int32_t done = 1;
        memcpy((int8_t*)arg->future + 8, &done, 4);
    }
    free(arg);  /* arg was malloc'd below, not GC-managed */
    return result;
}

/* ── Future operations ───────────────────────────────────────────────────── */

/*
 * ec_future_new(fn_ptr, env_ptr)
 * Spawn a new thread running fn_ptr(env_ptr) and return a Future GC object.
 * fn_ptr signature: i8* fn(i8* env)
 */
void* ec_future_new(void* fn_ptr, void* env_ptr) {
    /* Allocate future in GC heap */
    void* future = ec_new((int64_t)FUTURE_HEADER);
    memset(future, 0, FUTURE_HEADER);

    /* Build trampoline arg (heap-allocated, freed by trampoline) */
    ThreadArg* arg = (ThreadArg*)malloc(sizeof(ThreadArg));
    if (!arg) return future;

    arg->fn     = (void*(*)(void*))fn_ptr;
    arg->env    = env_ptr;
    arg->future = future;

    pthread_t tid;
    if (pthread_create(&tid, NULL, thread_trampoline, arg) != 0) {
        free(arg);
        return future;
    }
    /* Detach: thread cleans itself up; we read result via future */
    pthread_detach(tid);
    /* Store tid in future[16..23] for informational purposes */
    memcpy((int8_t*)future + 16, &tid, sizeof(pthread_t));
    return future;
}

/*
 * ec_future_await(future) → i8*
 * Busy-wait (with yield) until the Future is done, then return the result.
 */
void* ec_future_await(void* future) {
    if (!future) return NULL;
    /* Spin-yield until done flag is set */
    while (1) {
        int32_t done = 0;
        memcpy(&done, (int8_t*)future + 8, 4);
        if (done) break;
        sched_yield();
    }
    void* result = NULL;
    memcpy(&result, future, 8);
    return result;
}

/*
 * ec_future_done(future) → i32
 * Returns 1 if the async task has completed, 0 if still running.
 */
int32_t ec_future_done(void* future) {
    if (!future) return 1;
    int32_t done = 0;
    memcpy(&done, (int8_t*)future + 8, 4);
    return done;
}

/* ── Mutex operations ────────────────────────────────────────────────────── */

void* ec_mutex_new(void) {
    void* m = ec_new((int64_t)MUTEX_SIZE);
    pthread_mutex_t init = PTHREAD_MUTEX_INITIALIZER;
    memcpy(m, &init, sizeof(pthread_mutex_t));
    return m;
}

void ec_mutex_lock(void* m) {
    if (!m) return;
    pthread_mutex_lock((pthread_mutex_t*)m);
}

void ec_mutex_unlock(void* m) {
    if (!m) return;
    pthread_mutex_unlock((pthread_mutex_t*)m);
}

int32_t ec_mutex_trylock(void* m) {
    if (!m) return 0;
    return pthread_mutex_trylock((pthread_mutex_t*)m) == 0 ? 1 : 0;
}

void ec_mutex_destroy(void* m) {
    if (!m) return;
    pthread_mutex_destroy((pthread_mutex_t*)m);
}

/* ── Thread sleep / yield ────────────────────────────────────────────────── */

void ec_thread_sleep_ms(int32_t ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

void ec_thread_yield(void) {
    sched_yield();
}

/* =========================================================================
 *  Terminal / editor primitives
 *  Used by ecvim.ec to access raw terminal I/O
 * =========================================================================*/

#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>

static struct termios ec_orig_termios;
static int            ec_term_is_raw = 0;

void ec_term_raw(void) {
    if (ec_term_is_raw) return;
    tcgetattr(STDIN_FILENO, &ec_orig_termios);
    struct termios raw = ec_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |=  (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    ec_term_is_raw = 1;
}

void ec_term_restore(void) {
    if (!ec_term_is_raw) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &ec_orig_termios);
    ec_term_is_raw = 0;
}

int32_t ec_term_rows(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_row == 0)
        return 24;
    return (int32_t)ws.ws_row;
}

int32_t ec_term_cols(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
        return 80;
    return (int32_t)ws.ws_col;
}

/* Read one keypress and return a keycode.
 * Escape sequences are decoded into values >= 1000.
 * Regular characters are returned as-is (0-127).          */
#define ECKEY_ESC        27
#define ECKEY_UP         1000
#define ECKEY_DOWN       1001
#define ECKEY_LEFT       1002
#define ECKEY_RIGHT      1003
#define ECKEY_DEL        1004
#define ECKEY_HOME       1005
#define ECKEY_END        1006
#define ECKEY_PGUP       1007
#define ECKEY_PGDN       1008
#define ECKEY_F1         1009
#define ECKEY_F2         1010
#define ECKEY_F5         1013

int32_t ec_read_key(void) {
    int nread;
    unsigned char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) return -1;
    }
    if (c != 27) return (int32_t)c;

    unsigned char seq[6] = {0};
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return ECKEY_ESC;
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return ECKEY_ESC;

    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            if (read(STDIN_FILENO, &seq[2], 1) != 1) return ECKEY_ESC;
            if (seq[2] == '~') {
                switch (seq[1]) {
                case '1': return ECKEY_HOME;
                case '3': return ECKEY_DEL;
                case '4': return ECKEY_END;
                case '5': return ECKEY_PGUP;
                case '6': return ECKEY_PGDN;
                case '7': return ECKEY_HOME;
                case '8': return ECKEY_END;
                }
            }
            if (read(STDIN_FILENO, &seq[3], 1) != 1) return ECKEY_ESC;
            if (seq[1]=='1'&&seq[2]=='1'&&seq[3]=='~') return ECKEY_F1;
            if (seq[1]=='1'&&seq[2]=='2'&&seq[3]=='~') return ECKEY_F2;
            if (seq[1]=='1'&&seq[2]=='5'&&seq[3]=='~') return ECKEY_F5;
        } else {
            switch (seq[1]) {
            case 'A': return ECKEY_UP;
            case 'B': return ECKEY_DOWN;
            case 'C': return ECKEY_RIGHT;
            case 'D': return ECKEY_LEFT;
            case 'H': return ECKEY_HOME;
            case 'F': return ECKEY_END;
            }
        }
    } else if (seq[0] == 'O') {
        switch (seq[1]) {
        case 'H': return ECKEY_HOME;
        case 'F': return ECKEY_END;
        case 'P': return ECKEY_F1;
        case 'Q': return ECKEY_F2;
        case 't': return ECKEY_F5;
        }
    }
    return ECKEY_ESC;
}

/* Write raw bytes to stdout (no newline, flushed immediately).
 * Used by the render loop to output ANSI escape sequences.    */
void ec_write_raw(char* s) {
    if (!s) return;
    size_t len = strlen(s);
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(STDOUT_FILENO, s + written, len - written);
        if (n <= 0) break;
        written += n;
    }
}

void ec_flush(void) {
    fflush(stdout);
}

/* Run a shell command; returns exit code. */
int32_t ec_system(char* cmd) {
    if (!cmd) return -1;
    return (int32_t)system(cmd);
}

/* Register term_restore as an atexit handler so the terminal
 * is always restored even on abnormal exit.                   */
void ec_atexit_restore(void) {
    atexit(ec_term_restore);
}

/* SIGWINCH handler registration.
 * The handler just sets a global flag; the EC main loop polls it. */
static volatile int ec_winch_flag = 0;
static void ec_winch_handler_internal(int sig) { (void)sig; ec_winch_flag = 1; }

void ec_register_winch(void) {
    signal(SIGWINCH, ec_winch_handler_internal);
}

int32_t ec_winch_pending(void) {
    if (ec_winch_flag) { ec_winch_flag = 0; return 1; }
    return 0;
}

/* =========================================================================
 *  std.time runtime primitives
 * =========================================================================*/

#include <time.h>
#include <sys/time.h>

/* Wall-clock milliseconds since Unix epoch (UTC) */
int64_t ec_time_wallclock_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000LL + (int64_t)tv.tv_usec / 1000LL;
}

/* Decompose epoch-ms into UTC calendar fields */
static void ec_time_decompose(int64_t ms, struct tm *out) {
    time_t t = (time_t)(ms / 1000LL);
    gmtime_r(&t, out);
}

int32_t ec_time_year(int64_t ms)  { struct tm t; ec_time_decompose(ms,&t); return t.tm_year+1900; }
int32_t ec_time_month(int64_t ms) { struct tm t; ec_time_decompose(ms,&t); return t.tm_mon+1; }
int32_t ec_time_day(int64_t ms)   { struct tm t; ec_time_decompose(ms,&t); return t.tm_mday; }
int32_t ec_time_hour(int64_t ms)  { struct tm t; ec_time_decompose(ms,&t); return t.tm_hour; }
int32_t ec_time_minute(int64_t ms){ struct tm t; ec_time_decompose(ms,&t); return t.tm_min; }
int32_t ec_time_second(int64_t ms){ struct tm t; ec_time_decompose(ms,&t); return t.tm_sec; }
int32_t ec_time_weekday(int64_t ms){ struct tm t; ec_time_decompose(ms,&t); return t.tm_wday; } /* 0=Sun */
int32_t ec_time_yearday(int64_t ms){ struct tm t; ec_time_decompose(ms,&t); return t.tm_yday; }

/* Format a UTC epoch-ms using strftime pattern → GC string */
char* ec_time_format(int64_t ms, char* fmt) {
    if (!fmt) fmt = "%Y-%m-%d %H:%M:%S";
    struct tm t;
    ec_time_decompose(ms, &t);
    char buf[256];
    strftime(buf, sizeof(buf), fmt, &t);
    size_t len = strlen(buf);
    char* out = (char*)ec_new((int64_t)(len + 1));
    memcpy(out, buf, len + 1);
    return out;
}

/* Local-time versions */
static void ec_time_decompose_local(int64_t ms, struct tm *out) {
    time_t t = (time_t)(ms / 1000LL);
    localtime_r(&t, out);
}

int32_t ec_time_year_local(int64_t ms)   { struct tm t; ec_time_decompose_local(ms,&t); return t.tm_year+1900; }
int32_t ec_time_month_local(int64_t ms)  { struct tm t; ec_time_decompose_local(ms,&t); return t.tm_mon+1; }
int32_t ec_time_day_local(int64_t ms)    { struct tm t; ec_time_decompose_local(ms,&t); return t.tm_mday; }
int32_t ec_time_hour_local(int64_t ms)   { struct tm t; ec_time_decompose_local(ms,&t); return t.tm_hour; }
int32_t ec_time_minute_local(int64_t ms) { struct tm t; ec_time_decompose_local(ms,&t); return t.tm_min; }
int32_t ec_time_second_local(int64_t ms) { struct tm t; ec_time_decompose_local(ms,&t); return t.tm_sec; }
char*   ec_time_format_local(int64_t ms, char* fmt) {
    if (!fmt) fmt = "%Y-%m-%d %H:%M:%S";
    struct tm t;
    ec_time_decompose_local(ms, &t);
    char buf[256];
    strftime(buf, sizeof(buf), fmt, &t);
    size_t len = strlen(buf);
    char* out = (char*)ec_new((int64_t)(len + 1));
    memcpy(out, buf, len + 1);
    return out;
}

/* Helper: duplicate string into GC heap */
/* (ec_strdup is already defined above as ec_strconcat helper -- use ec_new) */
static char* ec_strdup_local(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* p = (char*)ec_new((int64_t)(len + 1));
    memcpy(p, s, len + 1);
    return p;
}

/* =========================================================================
 *  std.algorithm — sort with comparator closure
 *
 *  ec_list_sort_fn(list_data, fn_ptr, env_ptr)
 *    fn_ptr signature: i32 fn(i8* env, i8* a, i8* b)  — returns <0, 0, >0
 *    Uses an insertion sort (stable, GC-safe, no temporary heap).
 * =========================================================================*/

typedef struct {
    void* fn;
    void* env;
} SortCtx;

/* Insertion sort — stable, works well for the GC since no malloc */
void ec_list_sort_fn(void* list_data, void* fn_ptr, void* env_ptr) {
    if (!list_data || !fn_ptr) return;
    /* Extract the element array from the list fat block.
     * Layout: [i64 capacity][i64 length][i8** elements ...] */
    int64_t length = *(int64_t*)((char*)list_data + 8);
    void**  elems  = (void**)((char*)list_data + 16);

    typedef int32_t (*CmpFn)(void* env, void* a, void* b);
    CmpFn cmp = (CmpFn)fn_ptr;

    /* Insertion sort */
    for (int64_t i = 1; i < length; i++) {
        void* key = elems[i];
        int64_t j = i - 1;
        while (j >= 0 && cmp(env_ptr, elems[j], key) > 0) {
            elems[j + 1] = elems[j];
            j--;
        }
        elems[j + 1] = key;
    }
}

/* Reverse a list in-place */
void ec_list_reverse_fn(void* list_data) {
    if (!list_data) return;
    int64_t length = *(int64_t*)((char*)list_data + 8);
    void**  elems  = (void**)((char*)list_data + 16);
    int64_t lo = 0, hi = length - 1;
    while (lo < hi) {
        void* tmp = elems[lo];
        elems[lo] = elems[hi];
        elems[hi] = tmp;
        lo++; hi--;
    }
}

/* Binary search — list must be sorted by cmp.
 * Returns index of a match, or -1 if not found. */
int64_t ec_list_binary_search(void* list_data, void* target,
                               void* fn_ptr, void* env_ptr) {
    if (!list_data || !fn_ptr) return -1;
    int64_t length = *(int64_t*)((char*)list_data + 8);
    void**  elems  = (void**)((char*)list_data + 16);
    typedef int32_t (*CmpFn)(void* env, void* a, void* b);
    CmpFn cmp = (CmpFn)fn_ptr;
    int64_t lo = 0, hi = length - 1;
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        int32_t r = cmp(env_ptr, elems[mid], target);
        if (r == 0) return mid;
        if (r < 0)  lo = mid + 1;
        else        hi = mid - 1;
    }
    return -1;
}

/* Shuffle — Fisher-Yates using ec random via srand/rand */
void ec_list_shuffle(void* list_data) {
    if (!list_data) return;
    int64_t length = *(int64_t*)((char*)list_data + 8);
    void**  elems  = (void**)((char*)list_data + 16);
    for (int64_t i = length - 1; i > 0; i--) {
        int64_t j = (int64_t)rand() % (i + 1);
        void* tmp = elems[i];
        elems[i]  = elems[j];
        elems[j]  = tmp;
    }
}

/* Sum of integer-valued elements via accessor closure */
int64_t ec_list_sum_fn(void* list_data, void* fn_ptr, void* env_ptr) {
    if (!list_data) return 0;
    int64_t length = *(int64_t*)((char*)list_data + 8);
    void**  elems  = (void**)((char*)list_data + 16);
    typedef int64_t (*AccessFn)(void* env, void* item);
    AccessFn acc = (AccessFn)fn_ptr;
    int64_t sum = 0;
    for (int64_t i = 0; i < length; i++) sum += acc(env_ptr, elems[i]);
    return sum;
}

/* Seed the random number generator */
void ec_random_seed(int32_t seed) { srand((unsigned)seed); }
int32_t ec_random_int(int32_t lo, int32_t hi) {
    if (hi <= lo) return lo;
    return lo + (int32_t)(rand() % (hi - lo));
}
double ec_random_double(void) { return (double)rand() / (double)RAND_MAX; }

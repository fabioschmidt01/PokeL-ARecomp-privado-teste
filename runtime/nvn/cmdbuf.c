// NVN command buffers: commands are recorded into the guest-provided command memory as CmdRec
// records, so the game's own memory management and out-of-memory callbacks work as designed.
// A command handle is the guest address of a list's first record.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"

#define JUMP_RESERVE (sizeof(CmdRec) + 8)

static NvnCmdBuf *cb_of(uint64_t guest) { return HOST(NvnCmdBuf, guest); }

NVN(nvnCommandBufferInitialize) {
    NvnCmdBuf *cb = calloc(1, sizeof *cb);
    cb->guest = A(0);
    SET_HOST(A(0), cb);
    RET(1);
}
NVN(nvnCommandBufferFinalize) { free(cb_of(A(0))); SET_HOST(A(0), 0); }
NVN(nvnCommandBufferSetDebugLabel) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetDebugLabel\n"); }

static CmdRec *raw_rec(uint64_t at, uint32_t op, uint32_t size) {
    CmdRec *r = (CmdRec *)(uintptr_t)at;
    memset(r, 0, sizeof *r);
    r->op = op;
    r->size = size;
    return r;
}

// Records live in host memory, never in the game's command memory. Each chunk the game hands over
// (nvnCommandBufferAddCommandMemory) gets a host "shadow" of the same size, and the game's accounting stays exactly as
// before (sizes, Used / Free, the memory callback). Recorded into the guest chunk itself, a list could be rewritten
// before the render thread translated it: in Floaro Main Street (PLA, 2026-11-14) the game handed a command buffer
// memory that lists still queued were in - a jump record pointed back into its own page, the called lists turned into
// allocator garbage (a "corrupt command list", then a crash in exec_list / guest_copy; the device lost on slower
// CPUs, #7 / #12). Our records are several times the size of the hardware's commands, so the game's command memory
// turns over that much faster than it was built for.
// A shadow is freed once the game gave that guest memory out again (retired), everything queued for the render thread
// by then was translated, and 8 frames went by (lists submitted right after it are translated too: the render thread
// stays within a frame or two of the game) - so whatever the game does with its memory, a list's records stay as
// recorded until they are translated. Chunks the game never gives out again (lists it keeps and calls) stay.
// PLA hands out ~1 KB chunks, ~10000 a second: the guest pages index the live shadows (retiring looks at that page's
// few), the retired ones wait in a queue in retirement order (freeing looks at its head) - a first version walked
// every live shadow per chunk, 20000 of them in Jubilife: 7 fps. Records also no longer dirty the guest heap's pages
// (the write watch). LARECOMP_CMD_SHADOW=0: off (records in guest memory, as before).
typedef struct ShNode { uint64_t page; struct Shadow *s; struct ShNode *next; } ShNode;
typedef struct Shadow {
    uint64_t gbase;
    volatile uint64_t gend;  // the guest chunk it shadows, as far as records were accounted in it (rec; an overflow
                             // chunk: the guest chunk it continues)
    uint8_t *mem;
    uint64_t size;
    uint64_t retired_frame, retired_q;  // when the game gave the guest memory out again: frame, render-queue items queued
    bool retired;
    ShNode *nodes;  // one per guest page (in the page index while live)
    int nnodes;
    struct Shadow *next;  // (the retired queue)
} Shadow;
#define SH_BUCKETS 65536
static ShNode *sh_index[SH_BUCKETS];
static Shadow *retired_head, *retired_tail;
static uint64_t shadow_live, shadow_bytes, shadow_freed, shadow_retired_n;
static pthread_mutex_t shadow_lock = PTHREAD_MUTEX_INITIALIZER;
static int shadow_on = -1;
extern uint64_t renderer_present_count(void);
extern uint64_t rq_done_seq(void);   // (rqueue.c: items translated so far; UINT64_MAX with the render thread off)
extern uint64_t rq_fence_seq(void);  // (rqueue.c: items queued so far)

static bool shadows_enabled(void) {
    if (shadow_on < 0) {
        const char *e = getenv("LARECOMP_CMD_SHADOW");
        shadow_on = !e || atoi(e) != 0;
        LOG("[nvn] command lists recorded in %s (LARECOMP_CMD_SHADOW=%d: %s)\n",
            shadow_on ? "host memory (the game's command memory only accounted)" : "the game's command memory",
            !shadow_on, shadow_on ? "in the game's memory" : "in host memory");
    }
    return shadow_on;
}

static inline unsigned sh_hash(uint64_t page) { return (unsigned)((page * 0x9E3779B97F4A7C15ull) >> 48) & (SH_BUCKETS - 1); }

// [gbase, gend): its guest range now (grows as records are accounted); [gbase, iend): the pages it is indexed under
static Shadow *shadow_new(uint64_t gbase, uint64_t gend, uint64_t iend, uint64_t size) {
    Shadow *s = calloc(1, sizeof *s);
    s->gbase = gbase, s->gend = gend, s->size = size;
    s->mem = malloc(size);
    if (iend > gbase) {
        uint64_t p0 = gbase >> 12, p1 = (iend - 1) >> 12;
        s->nnodes = (int)(p1 - p0 + 1);
        s->nodes = malloc(sizeof *s->nodes * (size_t)s->nnodes);
        for (int i = 0; i < s->nnodes; i++) s->nodes[i] = (ShNode){.page = p0 + (uint64_t)i, .s = s};
    }
    pthread_mutex_lock(&shadow_lock);
    for (int i = 0; i < s->nnodes; i++) {
        ShNode **b = &sh_index[sh_hash(s->nodes[i].page)];
        s->nodes[i].next = *b;
        *b = &s->nodes[i];
    }
    shadow_live++;
    shadow_bytes += size;
    pthread_mutex_unlock(&shadow_lock);
    return s;
}

static void sh_unlink(Shadow *s) {  // (shadow_lock) out of the page index
    for (int i = 0; i < s->nnodes; i++)
        for (ShNode **p = &sh_index[sh_hash(s->nodes[i].page)]; *p; p = &(*p)->next)
            if (*p == &s->nodes[i]) { *p = s->nodes[i].next; break; }
}

// The game gives [gbase, gend) out again: the shadows of chunks there are retired, and retired ones whose time is up
// are freed.
static void shadows_retire(uint64_t gbase, uint64_t gend) {
    uint64_t now = renderer_present_count(), done = rq_done_seq(), queued = rq_fence_seq();
    pthread_mutex_lock(&shadow_lock);
    if (gend > gbase)
        for (uint64_t pg = gbase >> 12; pg <= (gend - 1) >> 12; pg++) {
            for (ShNode *n = sh_index[sh_hash(pg)]; n;) {
                Shadow *s = n->s;
                n = n->next;
                if (s->gbase >= gend || gbase >= s->gend) continue;  // (a bucket holds other pages' nodes too)
                s->retired = true, s->retired_frame = now, s->retired_q = queued;
                sh_unlink(s);
                n = sh_index[sh_hash(pg)];  // (unlinking may have taken the next node: the bucket again - it is short)
                if (retired_tail) retired_tail->next = s; else retired_head = s;
                retired_tail = s;
                shadow_retired_n++;
            }
        }
    while (retired_head && now >= retired_head->retired_frame + 8 && done >= retired_head->retired_q) {
        Shadow *s = retired_head;
        retired_head = s->next;
        if (!retired_head) retired_tail = NULL;
        shadow_live--, shadow_retired_n--, shadow_freed++;
        shadow_bytes -= s->size;
        free(s->nodes);
        free(s->mem);
        free(s);
    }
    static uint64_t calls;
    if (++calls % 50000 == 0)
        LOG("[nvn] command memory shadows: %llu live (%llu retired, waiting), %.1f MB, %llu freed\n",
            (unsigned long long)shadow_live, (unsigned long long)shadow_retired_n, shadow_bytes / 1048576.0,
            (unsigned long long)shadow_freed);
    pthread_mutex_unlock(&shadow_lock);
}

// The game is told hardware-like sizes: a record counts 16 bytes plus its copied data (ours: 80 plus the data, ~5x a
// hardware command). Told our own sizes, the game's command memory allocator went ~5x faster than it was built for
// and gave out memory still holding lists it calls later (PLA: ~12000 chunks of ~1 KB a second) - with the records
// there, they were rewritten (Floaro's crash); with them in shadows, those shadows were retired and freed while the
// game still called them: a list calling itself in the reused memory, 7 million draws a frame, a black screen after
// 22 minutes (2026-11-14). Only the part accounted counts as a shadow's guest range (Shadow.gend: an allocator that
// takes back a chunk's unused tail must not retire it).
// A shadow starts at 2x its guest chunk (records are up to 5x the accounting, ~2x on average); when it is full and the
// game's chunk is not, the list goes on in another shadow for the rest of that chunk (host_continue: no call to the
// game) - each shadow covers the guest bytes accounted while it was current. 5x up front was ~375 MB after 20 minutes.
#define ACCT(extra) ((16u + (extra) + 7u) & ~7u)
#define JUMP_ACCT 16u
#define SHADOW_SCALE 5u   // (records per accounted byte at most: the overflow chunk's view)
#define SHADOW_START 2u   // (a new shadow: this many times its guest chunk)

// The shadow is full, the game's chunk is not: the rest of the chunk goes on in a new shadow.
static void host_continue(NvnCmdBuf *cb, uint32_t need) {
    uint64_t at = cb->gmem + cb->gused, left = cb->gsize > cb->gused ? cb->gsize - cb->gused : 0;
    uint64_t size = left * SHADOW_START + 2 * JUMP_RESERVE;
    if (size < need + 2 * JUMP_RESERVE) size = need + 2 * JUMP_RESERVE;
    Shadow *s = shadow_new(at, at, cb->gmem + cb->gsize, size);
    CmdRec *j = raw_rec(cb->mem + cb->used, CMD_JUMP, sizeof(CmdRec));
    j->x[0] = (uint64_t)(uintptr_t)s->mem;
    cb->mem = (uint64_t)(uintptr_t)s->mem;
    cb->size = size;
    cb->used = 0;
    cb->shadow = s;
    static uint64_t n;
    if (++n == 1 || n == 100000) LOG("[nvn] command memory: a shadow continued in another (%llu times)\n", (unsigned long long)n);
}

// (objects.c nvnMemoryPoolFinalize) the game is done with a whole pool: shadows of chunks in it are retired too (its
// memory may go back to the heap and never come out as command memory again)
void nvn_cmdmem_pool_gone(uint64_t storage, uint64_t size) { if (size) shadows_retire(storage, storage + size); }

static void overflow_chunk(NvnCmdBuf *cb, uint32_t need) {
    uint64_t size = need + JUMP_RESERVE > 0x10000 ? need + JUMP_RESERVE : 0x10000;
    bool sh = shadows_enabled();
    // (retired with the guest chunk it continues; without shadows that is all there is of it)
    uint64_t gend = sh ? cb->gmem + cb->gused : cb->gmem + cb->gsize;
    Shadow *s = shadow_new(cb->gmem, gend, gend, size);
    static int logged;
    if (logged++ < 5) LOG("[nvn] command memory overflow: %u bytes, list goes on in host memory\n", need);
    if (cb->mem) {
        CmdRec *j = raw_rec(cb->mem + cb->used, CMD_JUMP, sizeof(CmdRec));
        j->x[0] = (uint64_t)(uintptr_t)s->mem;
    }
    cb->mem = (uint64_t)(uintptr_t)s->mem;
    cb->size = size;
    cb->used = 0;
    cb->shadow = NULL;  // (its guest range stays as it is now)
    if (sh) cb->gsize = cb->gused + size / SHADOW_SCALE;  // (the game's view: room again, as much as the host chunk holds)
    else cb->gmem = cb->mem, cb->gsize = size, cb->gused = 0;
}

NVN(nvnCommandBufferAddCommandMemory) {
    NvnCmdBuf *cb = cb_of(A(0));
    uint64_t gmem = HOST(NvnPool, A(1))->storage + A(2), size = A(3);
    shadows_retire(gmem, gmem + size);  // (records from earlier lists there: freed once nothing reads them)
    uint64_t mem = gmem, hsize = size;
    Shadow *s = NULL;
    if (shadows_enabled()) {
        hsize = size * SHADOW_START + 2 * JUMP_RESERVE;
        s = shadow_new(gmem, gmem, gmem + size, hsize);  // (its guest range grows as records are accounted)
        mem = (uint64_t)(uintptr_t)s->mem;
    }
    if (cb->recording && cb->mem) {
        // chain the list being recorded into the new chunk
        CmdRec *j = raw_rec(cb->mem + cb->used, CMD_JUMP, sizeof(CmdRec));
        j->x[0] = mem;
    }
    cb->gmem = gmem;
    cb->gsize = size;
    cb->gused = 0;
    cb->shadow = s;
    cb->mem = mem;
    cb->size = hsize;
    cb->used = 0;
}
NVN(nvnCommandBufferAddControlMemory) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferAddControlMemory\n"); }
NVN(nvnCommandBufferGetCommandMemorySize) { RET(cb_of(A(0))->gsize); }
NVN(nvnCommandBufferGetCommandMemoryUsed) { RET(cb_of(A(0))->gused); }
NVN(nvnCommandBufferGetCommandMemoryFree) {
    NvnCmdBuf *cb = cb_of(A(0));
    uint64_t jr = shadow_on > 0 ? JUMP_ACCT : JUMP_RESERVE;
    RET(cb->gsize > cb->gused + jr ? cb->gsize - cb->gused - jr : 0);
}
NVN(nvnCommandBufferGetControlMemorySize) { RET(0x10000); }
NVN(nvnCommandBufferGetControlMemoryUsed) { RET(0); }
NVN(nvnCommandBufferGetControlMemoryFree) { RET(0x10000); }
NVN(nvnCommandBufferSetMemoryCallback) { cb_of(A(0))->callback = A(1); }
NVN(nvnCommandBufferGetMemoryCallback) { RET(cb_of(A(0))->callback); }
NVN(nvnCommandBufferSetMemoryCallbackData) { cb_of(A(0))->callback_data = A(1); }
NVN(nvnCommandBufferGetMemoryCallbackData) { RET(cb_of(A(0))->callback_data); }
NVN(nvnCommandBufferSetCommandMemoryCallbackEnabled) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetCommandMemoryCallbackEnabled\n"); }

// Reserve a record. May call back into the guest for more memory, so callers snapshot their
// arguments first.
static CmdRec *rec(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra) {
    uint32_t size = (uint32_t)((sizeof(CmdRec) + extra + 7) & ~7u);
    bool sh = shadow_on > 0;
    uint32_t acct = sh ? ACCT(extra) : size, jr = sh ? JUMP_ACCT : (uint32_t)JUMP_RESERVE;  // (the game's view)
    for (int tries = 0;; tries++) {
        bool groom = cb->mem && cb->gused + acct + jr <= cb->gsize, hroom = cb->mem && cb->used + size + JUMP_RESERVE <= cb->size;
        if (sh && groom && !hroom && cb->shadow) host_continue(cb, size), hroom = true;  // (the game's chunk has room)
        if (groom && hroom) break;
        if (!cb->callback || tries == 2) {
            if (!cb->mem) FATAL("nvn: out of command memory (need %u)\n", size);
            overflow_chunk(cb, size);
            break;
        }
        uint64_t saved[8];
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = cb->guest;
        c->x[1] = 0;  // NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY
        c->x[2] = acct + jr;
        c->x[3] = cb->callback_data;
        guest_call(c, cb->callback);
        memcpy(c->x, saved, sizeof saved);
    }
    CmdRec *r = raw_rec(cb->mem + cb->used, op, size);
    cb->used += size;
    cb->gused += acct;
    if (cb->shadow) ((Shadow *)cb->shadow)->gend = cb->gmem + cb->gused + jr;  // (what the game knows it gave us)
    return r;
}

NVN(nvnCommandBufferBeginRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    cb->recording = true;
    rec(c, cb, CMD_BARRIER, 0);  // makes sure memory exists; the list starts here
    cb->start = cb->mem + cb->used - sizeof(CmdRec);
}
NVN(nvnCommandBufferEndRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    rec(c, cb, CMD_END, 0);
    cb->recording = false;
    RET(cb->start);
}
NVN(nvnCommandBufferIsRecording) { RET(cb_of(A(0))->recording); }

// ---- generic recorders
typedef struct { uint64_t x[8]; float f[4]; } Snap;
static Snap snap(Cpu *c) {
    Snap s;
    memcpy(s.x, c->x, sizeof s.x);
    for (int i = 0; i < 4; i++) s.f[i] = *(float *)&c->v[i];
    return s;
}
static CmdRec *record(Cpu *c, uint32_t op, uint32_t extra, Snap *s) {
    CmdRec *r = rec(c, cb_of(s->x[0]), op, extra);
    memcpy(r->x, s->x + 1, sizeof r->x);
    memcpy(r->f, s->f, sizeof r->f);
    return r;
}
// record with a copy of `len` bytes from the guest pointer in argument `arg`
static CmdRec *record_copy(Cpu *c, uint32_t op, int arg, uint32_t len) {
    Snap s = snap(c);
    CmdRec *r = record(c, op, len, &s);
    if (s.x[arg] && len) memcpy(r->extra, (void *)(uintptr_t)s.x[arg], len);
    return r;
}
#define PLAIN(name, op) NVN(name) { Snap s = snap(c); record(c, op, 0, &s); }

// ---- state binding
NVN(nvnCommandBufferBindProgram) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_PROGRAM, 0, &s);
    r->x[0] = s.x[1] ? (uint64_t)(uintptr_t)HOST(NvnProgram, s.x[1]) : 0;  // host program (NULL unbinds: draws skip)
}
NVN(nvnCommandBufferBindBlendState) { record_copy(c, CMD_BIND_BLEND, 1, 8); }
NVN(nvnCommandBufferBindChannelMaskState) { record_copy(c, CMD_BIND_CHANNEL_MASK, 1, 4); }
NVN(nvnCommandBufferBindColorState) { record_copy(c, CMD_BIND_COLOR, 1, 4); }
NVN(nvnCommandBufferBindDepthStencilState) { record_copy(c, CMD_BIND_DEPTH_STENCIL, 1, 8); }
NVN(nvnCommandBufferBindMultisampleState) { record_copy(c, CMD_BIND_MULTISAMPLE, 1, 0x18); }
NVN(nvnCommandBufferBindPolygonState) { record_copy(c, CMD_BIND_POLYGON, 1, 4); }
NVN(nvnCommandBufferBindVertexAttribState) { record_copy(c, CMD_BIND_VERTEX_ATTRIB, 2, (uint32_t)A(1) * 4); }
NVN(nvnCommandBufferBindVertexStreamState) { record_copy(c, CMD_BIND_VERTEX_STREAM, 2, (uint32_t)A(1) * 8); }

// (cmd, first, count, const NVNbufferRange*) and the single-binding forms
NVN(nvnCommandBufferBindVertexBuffers) { record_copy(c, CMD_BIND_VERTEX_BUFFERS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferBindVertexBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_VERTEX_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = 1;
    memcpy(r->extra, &s.x[2], 16);
}
// (cmd, stage, first, count, const NVNbufferRange*)
NVN(nvnCommandBufferBindUniformBuffers) { record_copy(c, CMD_BIND_UNIFORM_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindUniformBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_UNIFORM_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
NVN(nvnCommandBufferBindStorageBuffers) { record_copy(c, CMD_BIND_STORAGE_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindStorageBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_STORAGE_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
// (cmd, stage, first, count, const handle*)
NVN(nvnCommandBufferBindTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindImages) { record_copy(c, CMD_BIND_IMAGES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindImage) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_IMAGES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindSeparateTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateSamplers) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferBindSeparateSamplers\n"); }
NVN(nvnCommandBufferBindSeparateSampler) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferBindSeparateSampler\n"); }
// (buffers: first, count, NVNbufferRange* {address, size}) copied with the command
NVN(nvnCommandBufferBindTransformFeedbackBuffers) {
    Snap s = snap(c);
    uint32_t n = (uint32_t)s.x[2] < 4 ? (uint32_t)s.x[2] : 4;
    CmdRec *r = record(c, CMD_BIND_XFB_BUFFERS, n * 16, &s);
    if (n && s.x[3]) memcpy(r->extra, (const void *)(uintptr_t)s.x[3], n * 16);
}
PLAIN(nvnCommandBufferBindTransformFeedbackBuffer, CMD_BIND_XFB_BUFFER)  // (index, address, size)
NVN(nvnCommandBufferBindCoverageModulationTable) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferBindCoverageModulationTable\n"); }

NVN(nvnCommandBufferSetTexturePool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_TEXTURE_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}
NVN(nvnCommandBufferSetSamplerPool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SAMPLER_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}

// SetRenderTargets(cmd, n, colors[], colorViews[], depth, depthView)
// extra: { u64 tex[8]; NvnTextureView views[8]; u8 has_view[8]; u64 depth; NvnTextureView dview; u8 has_dview }
typedef struct {
    uint64_t tex[8];
    NvnTextureView views[8];
    uint8_t has_view[8];
    uint64_t depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtRec;
NVN(nvnCommandBufferSetRenderTargets) {
    Snap s = snap(c);
    int n = (int)s.x[1];
    if (n > 8) n = 8;
    RtRec rt = {0};
    const uint64_t *colors = (const uint64_t *)(uintptr_t)s.x[2], *views = (const uint64_t *)(uintptr_t)s.x[3];
    for (int i = 0; i < n; i++) {
        rt.tex[i] = colors && colors[i] ? (uint64_t)(uintptr_t)HOST(NvnTexture, colors[i]) : 0;
        if (views && views[i]) {
            rt.has_view[i] = 1;
            memcpy(&rt.views[i], (void *)(uintptr_t)views[i], sizeof(NvnTextureView));
            nvn_texture_note_view((NvnTexture *)(uintptr_t)rt.tex[i], &rt.views[i]);
        }
    }
    rt.depth = s.x[4] ? (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[4]) : 0;
    if (s.x[5]) {
        rt.has_dview = 1;
        memcpy(&rt.dview, (void *)(uintptr_t)s.x[5], sizeof(NvnTextureView));
        nvn_texture_note_view((NvnTexture *)(uintptr_t)rt.depth, &rt.dview);
    }
    CmdRec *r = record(c, CMD_SET_RENDER_TARGETS, sizeof rt, &s);
    r->x[0] = (uint64_t)n;
    memcpy(r->extra, &rt, sizeof rt);
}

// viewports/scissors/depth ranges
NVN(nvnCommandBufferSetViewports) { record_copy(c, CMD_SET_VIEWPORTS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetViewport) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_VIEWPORTS, 16, &s);
    float v[4] = {(float)(int32_t)s.x[1], (float)(int32_t)s.x[2], (float)(int32_t)s.x[3], (float)(int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetScissors) { record_copy(c, CMD_SET_SCISSORS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetScissor) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SCISSORS, 16, &s);
    int32_t v[4] = {(int32_t)s.x[1], (int32_t)s.x[2], (int32_t)s.x[3], (int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetDepthRanges) { record_copy(c, CMD_SET_DEPTH_RANGES, 3, (uint32_t)A(2) * 8); }
NVN(nvnCommandBufferSetDepthRange) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_DEPTH_RANGES, 8, &s);
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, s.f, 8);
}
NVN(nvnCommandBufferSetBlendColor) { record_copy(c, CMD_SET_BLEND_COLOR, 1, 16); }
NVN(nvnCommandBufferSetStencilRef) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 0; }
NVN(nvnCommandBufferSetStencilMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 1; }
NVN(nvnCommandBufferSetStencilValueMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 2; }
PLAIN(nvnCommandBufferSetPrimitiveRestart, CMD_SET_PRIMITIVE_RESTART)
PLAIN(nvnCommandBufferSetAlphaRef, CMD_SET_ALPHA_REF)
PLAIN(nvnCommandBufferSetDepthBounds, CMD_SET_DEPTH_BOUNDS)
PLAIN(nvnCommandBufferSetPolygonOffsetClamp, CMD_SET_POLYGON_OFFSET)
PLAIN(nvnCommandBufferSetLineWidth, CMD_SET_LINE_WIDTH)
PLAIN(nvnCommandBufferSetPointSize, CMD_SET_POINT_SIZE)
PLAIN(nvnCommandBufferSetSampleMask, CMD_SET_SAMPLE_MASK)
PLAIN(nvnCommandBufferSetRenderEnable, CMD_SET_RENDER_ENABLE)
NVN(nvnCommandBufferSetRenderEnableConditional) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetRenderEnableConditional\n"); }
PLAIN(nvnCommandBufferSetDepthClamp, CMD_SET_DEPTH_CLAMP)  // (the game's clamp: geometry past the clip planes is clamped, not clipped)
PLAIN(nvnCommandBufferSetRasterizerDiscard, CMD_SET_RASTERIZER_DISCARD)
NVN(nvnCommandBufferSetConservativeRasterEnable) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetConservativeRasterEnable\n"); }
NVN(nvnCommandBufferSetConservativeRasterDilate) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetConservativeRasterDilate\n"); }
NVN(nvnCommandBufferSetSubpixelPrecisionBias) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetSubpixelPrecisionBias\n"); }
NVN(nvnCommandBufferSetViewportSwizzles) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetViewportSwizzles\n"); }
NVN(nvnCommandBufferSetStencilCullCriteria) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetStencilCullCriteria\n"); }
NVN(nvnCommandBufferSetColorReductionEnable) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetColorReductionEnable\n"); }
NVN(nvnCommandBufferSetColorReductionThresholds) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetColorReductionThresholds\n"); }
NVN(nvnCommandBufferSetTiledCacheAction) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetTiledCacheAction\n"); }
NVN(nvnCommandBufferSetTiledCacheTileSize) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetTiledCacheTileSize\n"); }
NVN(nvnCommandBufferSetShaderScratchMemory) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetShaderScratchMemory\n"); }
NVN(nvnCommandBufferSetPatchSize) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetPatchSize\n"); }
NVN(nvnCommandBufferSetInnerTessellationLevels) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetInnerTessellationLevels\n"); }
NVN(nvnCommandBufferSetOuterTessellationLevels) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetOuterTessellationLevels\n"); }
NVN(nvnCommandBufferSetProgramSubroutines) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetProgramSubroutines\n"); }
NVN(nvnCommandBufferSaveZCullData) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSaveZCullData\n"); }
NVN(nvnCommandBufferRestoreZCullData) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferRestoreZCullData\n"); }

static uint64_t copy_row_stride, copy_image_stride;
RT_ON_RESET(cmdbuf_reset) { copy_row_stride = copy_image_stride = 0; }
NVN(nvnCommandBufferSetCopyRowStride) {
    Snap s = snap(c);
    copy_row_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferSetCopyImageStride) {
    Snap s = snap(c);
    copy_image_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferGetCopyRowStride) { RET(copy_row_stride); }
NVN(nvnCommandBufferGetCopyImageStride) { RET(copy_image_stride); }

// ---- clears
NVN(nvnCommandBufferClearColor) { record_copy(c, CMD_CLEAR_COLOR, 2, 16); }
NVN(nvnCommandBufferClearColori) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 1; }
NVN(nvnCommandBufferClearColorui) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 2; }
PLAIN(nvnCommandBufferClearDepthStencil, CMD_CLEAR_DEPTH_STENCIL)
// ClearTexture(cmd, texture, view, region, color, mask)
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } ClearTexRec;
static void clear_texture(Cpu *c, int kind) {
    Snap s = snap(c);
    ClearTexRec ct = {0};
    ct.tex = (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[1]);
    if (s.x[2]) { ct.has_view = 1; memcpy(&ct.view, (void *)(uintptr_t)s.x[2], sizeof ct.view); nvn_texture_note_view((NvnTexture *)(uintptr_t)ct.tex, &ct.view); }
    if (s.x[3]) memcpy(ct.region, (void *)(uintptr_t)s.x[3], sizeof ct.region);
    if (s.x[4]) memcpy(ct.color, (void *)(uintptr_t)s.x[4], 16);
    CmdRec *r = record(c, CMD_CLEAR_TEXTURE, sizeof ct, &s);
    r->x[6] = (uint64_t)kind;
    memcpy(r->extra, &ct, sizeof ct);
}
NVN(nvnCommandBufferClearTexture) { clear_texture(c, 0); }
NVN(nvnCommandBufferClearTexturei) { clear_texture(c, 1); }
NVN(nvnCommandBufferClearTextureui) { clear_texture(c, 2); }
PLAIN(nvnCommandBufferClearBuffer, CMD_CLEAR_BUFFER)
// DiscardColor(cmdbuf, index): the target's contents become undefined. On the Switch a discarded (compressed)
// target reads back cleared; areas the game then leaves undrawn (e.g. a pit after a floor tile falls away) stay
// black there. Treated as a clear to (0,0,0,0) (LARECOMP_KEEP_DISCARD=1: old no-op behaviour).
NVN(nvnCommandBufferDiscardColor) {
    static int keep = -1, logged;
    if (keep < 0) keep = ENV("LARECOMP_KEEP_DISCARD") != NULL;
    if (logged < 5) { logged++; LOG("[nvn] DiscardColor index %llu%s\n", (unsigned long long)c->x[1], keep ? " (kept)" : " -> clear"); }
    if (keep) return;
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_CLEAR_COLOR, 16, &s);
    memset(r->extra, 0, 16);
    r->x[2] = 0xF;  // all channels
    r->x[5] = 0;    // float clear
}
NVN(nvnCommandBufferDiscardDepthStencil) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferDiscardDepthStencil\n"); }

// ---- draws (all integer arguments fit in x1..x7)
// Their counts, firsts, bases and the mode are 32-bit ints in NVN's API: the AAPCS64 leaves a w register's upper half
// undefined, and Pokémon Legends: Arceus passes some with garbage there - a 0-instance draw (one that draws nothing)
// came with a "vertex count" of 0x5000000000 / 0x7080001a70: the runtime reserved 8 GB for its vertices (#7) or read
// an address made of it (#12, a crash after the scene went black / flickered). Only the index buffer is an address.
static void draw_args32(CmdRec *r, unsigned keep64) {
    for (int i = 0; i < 7; i++) if (!(keep64 >> i & 1)) r->x[i] = (uint32_t)r->x[i];
}
NVN(nvnCommandBufferDrawArrays) { Snap s = snap(c); CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0, &s); draw_args32(r, 0); r->x[3] = 0; r->x[4] = 1; }
NVN(nvnCommandBufferDrawArraysInstanced) { Snap s = snap(c); draw_args32(record(c, CMD_DRAW_ARRAYS, 0, &s), 0); }
// Debug: LARECOMP_TRACE_COUNT=<n> logs the guest call chain of indexed draws with n indices.
static void trace_draw(Cpu *c, uint64_t count) {
    static long want = -2;
    if (want == -2) want = ENV("LARECOMP_TRACE_COUNT") ? atol(ENV("LARECOMP_TRACE_COUNT")) : -1;
    if (want < 0 || (long)count != want) return;
    static int n;
    if (n++ > 40) return;
    LOG("[trace] draw count %llu lr %llx chain:", (unsigned long long)count, (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 14 && fp >= 0x7000000000ull && fp < 0x8000000000ull && (fp & 7) == 0; i++) {
        uint64_t next = *(uint64_t *)(uintptr_t)fp, lr = *(uint64_t *)(uintptr_t)(fp + 8);
        LOG(" %llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    LOG("\n");
}

NVN(nvnCommandBufferDrawElements) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    draw_args32(r, 1u << 3);  // (x[3]: the index buffer's address)
    r->x[4] = 0; r->x[5] = 0; r->x[6] = 1;  // base vertex, base instance, instances
}
NVN(nvnCommandBufferDrawElementsBaseVertex) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    draw_args32(r, 1u << 3);
    r->x[5] = 0; r->x[6] = 1;
}
NVN(nvnCommandBufferDrawElementsInstanced) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    draw_args32(record(c, CMD_DRAW_ELEMENTS, 0, &s), 1u << 3);
}
PLAIN(nvnCommandBufferDrawArraysIndirect, CMD_DRAW_ARRAYS_INDIRECT)
PLAIN(nvnCommandBufferDrawElementsIndirect, CMD_DRAW_ELEMENTS_INDIRECT)
PLAIN(nvnCommandBufferMultiDrawArraysIndirectCount, CMD_MULTI_DRAW_ARRAYS_INDIRECT_COUNT)
PLAIN(nvnCommandBufferMultiDrawElementsIndirectCount, CMD_MULTI_DRAW_ELEMENTS_INDIRECT_COUNT)
PLAIN(nvnCommandBufferDrawTransformFeedback, CMD_DRAW_XFB)  // (mode, control buffer)
NVN(nvnCommandBufferDrawTexture) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferDrawTexture\n"); }
PLAIN(nvnCommandBufferDispatchCompute, CMD_DISPATCH)
PLAIN(nvnCommandBufferDispatchComputeIndirect, CMD_DISPATCH_INDIRECT)
PLAIN(nvnCommandBufferBeginTransformFeedback, CMD_BEGIN_XFB)  // (control buffer)
PLAIN(nvnCommandBufferEndTransformFeedback, CMD_END_XFB)
NVN(nvnCommandBufferPauseTransformFeedback) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferPauseTransformFeedback\n"); }
NVN(nvnCommandBufferResumeTransformFeedback) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferResumeTransformFeedback\n"); }

// ---- copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRef;
static void tex_ref(TexRef *t, uint64_t tex, uint64_t view, uint64_t region) {
    t->tex = tex ? (uint64_t)(uintptr_t)HOST(NvnTexture, tex) : 0;
    if (view) { t->has_view = 1; memcpy(&t->view, (void *)(uintptr_t)view, sizeof t->view); nvn_texture_note_view((NvnTexture *)(uintptr_t)t->tex, &t->view); }
    if (region) memcpy(t->region, (void *)(uintptr_t)region, sizeof t->region);
}
NVN(nvnCommandBufferCopyBufferToTexture) {  // (cmd, src, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[2], s.x[3], s.x[4]);
    CmdRec *r = record(c, CMD_COPY_BUFFER_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToBuffer) {  // (cmd, src, srcView, srcRegion, dst, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[1], s.x[2], s.x[3]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_BUFFER, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToTexture) {  // (cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], s.x[3]);
    tex_ref(&t[1], s.x[4], s.x[5], s.x[6]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
PLAIN(nvnCommandBufferCopyBufferToBuffer, CMD_COPY_BUFFER_TO_BUFFER)
NVN(nvnCommandBufferDownsample) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], 0, 0);
    tex_ref(&t[1], s.x[2], 0, 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferDownsampleTextureView) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], 0);
    tex_ref(&t[1], s.x[3], s.x[4], 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferTiledDownsample) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferTiledDownsample\n"); }
NVN(nvnCommandBufferTiledDownsampleTextureView) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferTiledDownsampleTextureView\n"); }
NVN(nvnCommandBufferResolveDepthBuffer) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferResolveDepthBuffer\n"); }
// UpdateUniformBuffer(cmd, buffer, size, offset, dataSize, data)
NVN(nvnCommandBufferUpdateUniformBuffer) { record_copy(c, CMD_UPDATE_UNIFORM_BUFFER, 5, (uint32_t)A(4)); }

// ---- synchronization and misc
NVN(nvnCommandBufferFenceSync) {
    // work executes at submit time, so the sync is satisfied once this list has run
    Snap s = snap(c);
    record(c, CMD_FENCE_SYNC, 0, &s);
}
PLAIN(nvnCommandBufferWaitSync, CMD_WAIT_SYNC)
PLAIN(nvnCommandBufferBarrier, CMD_BARRIER)
PLAIN(nvnCommandBufferSignalEvent, CMD_SIGNAL_EVENT)
PLAIN(nvnCommandBufferWaitEvent, CMD_WAIT_EVENT)
PLAIN(nvnCommandBufferReportCounter, CMD_REPORT_COUNTER)
NVN(nvnCommandBufferReportValue) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferReportValue\n"); }
NVN(nvnCommandBufferResetCounter) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferResetCounter\n"); }
NVN(nvnCommandBufferCallCommands) { record_copy(c, CMD_CALL, 2, (uint32_t)A(1) * 8); }
// nvnCommandBufferCopyCommands COPIES the lists' commands into this buffer (CallCommands only references them): the
// game may reset and re-record the source memory as soon as the call returns. Recorded as a CALL, the render thread
// read the sources a frame or more later - by then rewritten: garbage records (an "impossible draw" with an address
// as its instance count, a jump to 0x70 - a crash in exec_list after a 2 s shader-compile stall in Floaro, PLA
// 2026-11-14), the more often the further translation lags behind the game. Now the records are copied here, at
// once (they are self-contained: arguments and copied data, no pointers into themselves).
static void copy_list(Cpu *c, NvnCmdBuf *cb, uint64_t handle) {
    uint64_t p = handle;
    for (int guard = 0; p && guard < 1000000; guard++) {
        const CmdRec *s = (const CmdRec *)(uintptr_t)p;
        if (s->op == CMD_END) return;
        if (s->op == CMD_JUMP) { p = s->x[0]; continue; }
        uint32_t size = s->size;
        if (size < sizeof(CmdRec) || (size & 7) || s->op >= CMD_COUNT) {
            static int bad;
            if (bad++ < 5) LOG("[nvn] CopyCommands: a bad record in list %llx (op %u size %u): copy stops there\n",
                               (unsigned long long)handle, s->op, size);
            return;
        }
        CmdRec *d = rec(c, cb, s->op, size - (uint32_t)sizeof(CmdRec));  // (may call the game's memory callback)
        memcpy(d, s, size);
        p += size;
    }
}
NVN(nvnCommandBufferCopyCommands) {
    NvnCmdBuf *cb = cb_of(A(0));
    uint64_t n = A(1), h[64], *hs = n <= 64 ? h : malloc(sizeof *hs * n);
    if (n && A(2)) memcpy(hs, (const void *)(uintptr_t)A(2), sizeof *hs * n);
    else n = 0;
    static uint64_t calls, lists;
    calls++, lists += n;
    if (calls == 1 || calls == 1000 || calls == 100000)
        LOG("[nvn] CopyCommands: %llu calls, %llu lists copied (records copied at the call)\n",
            (unsigned long long)calls, (unsigned long long)lists);
    for (uint64_t i = 0; i < n; i++) copy_list(c, cb, hs[i]);
    if (hs != h) free(hs);
}
NVN(nvnCommandBufferInsertDebugMarker) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferInsertDebugMarker\n"); }
NVN(nvnCommandBufferInsertDebugMarkerDynamic) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferInsertDebugMarkerDynamic\n"); }
NVN(nvnCommandBufferInsertDebugMarkerStatic) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferInsertDebugMarkerStatic\n"); }
NVN(nvnCommandBufferPushDebugGroup) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferPushDebugGroup\n"); }
NVN(nvnCommandBufferPushDebugGroupDynamic) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferPushDebugGroupDynamic\n"); }
NVN(nvnCommandBufferPushDebugGroupStatic) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferPushDebugGroupStatic\n"); }
NVN(nvnCommandBufferPopDebugGroup) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferPopDebugGroup\n"); }
NVN(nvnCommandBufferPopDebugGroupId) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferPopDebugGroupId\n"); }

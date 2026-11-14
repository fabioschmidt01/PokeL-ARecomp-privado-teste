// Small services: parental control, add-on content, and other mostly-stubbed interfaces.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../platform.h"

static void ok_logger(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[svc] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ pctl
static void pctl_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1031: case 1035: case 1018: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // restrictions off
    case 1032: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;
    default: LOG("[pctl] IParentalControlService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void pctl_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0 || req->cmd == 1)
        ipc_return_object(resp, service_new("IParentalControlService", pctl_service, NULL));
}
SERVICE("pctl", pctl) { pctl_root(s, req, resp); }
SERVICE("pctl:s", pctl_s) { pctl_root(s, req, resp); }
SERVICE("pctl:a", pctl_a) { pctl_root(s, req, resp); }
SERVICE("pctl:r", pctl_r) { pctl_root(s, req, resp); }

// ------------------------------------------------------------------ friend:u / friend:a
// Pokémon Legends Arceus opens the friend service at boot (Z-A doesn't): an offline Switch with no friends. The
// logging stub returned no IFriendService, and the SDK read through the missing object (a crash at boot).
static void friend_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;  // GetCompletionEvent
    case 10100: OUT(int32_t, 0) = 0; resp_size(resp, 4); break;            // GetFriendCount: none
    case 10101: case 10102: case 10110: case 20101: case 20102:            // friend lists, counts: empty
        OUT(int32_t, 0) = 0; resp_size(resp, 4); break;
    default: LOG("[friend] IFriendService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}
static void friend_notification(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0) resp->copy_handles[resp->ncopy++] = new_event_handle();  // GetEvent (never signalled)
    else LOG("[friend] INotificationService cmd %u (ok)\n", req->cmd);
}
static void friend_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("IFriendService", friend_service, NULL));
    else if (req->cmd == 1) ipc_return_object(resp, service_new("INotificationService", friend_notification, NULL));
    else LOG("[friend] cmd %u (ok)\n", req->cmd);
}
SERVICE("friend:u", friend_u) { friend_root(s, req, resp); }
SERVICE("friend:a", friend_a) { friend_root(s, req, resp); }
SERVICE("friend:v", friend_v) { friend_root(s, req, resp); }

// ------------------------------------------------------------------ nifm:u / nifm:s / nifm:a
// The network interface service: a Switch with no internet connection. Pokémon Legends Arceus asks for it from a
// menu after 15 minutes of play (2026-11-14: nifm:u cmd 5); the logging stub returned no IGeneralService and the SDK
// read through the missing object (a crash, f_700832fd50). A connection request is never accepted: its state is
// "error / not submitted" and its result "pending connection" (NIFM 111), as on a console in airplane mode - the game
// then tells the player it is offline.
#define NIFM_PENDING_CONNECTION MAKE_RESULT(110, 111)
static void nifm_request(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;  // GetRequestState: 1 (not submitted / error)
    case 1: resp->result = NIFM_PENDING_CONNECTION; break;      // GetResult
    case 2:                                                      // GetSystemEventReadableHandles (never signalled)
        resp->copy_handles[resp->ncopy++] = new_event_handle();
        resp->copy_handles[resp->ncopy++] = new_event_handle();
        break;
    default: LOG("[nifm] IRequest cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);  // Cancel, Submit, Set*...
    }
}
static void nifm_general(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1: OUT(uint64_t, 0) = 1; resp_size(resp, 8); break;  // GetClientId
    case 4: ipc_return_object(resp, service_new("IRequest", nifm_request, NULL)); break;  // CreateRequest
    case 12: resp->result = NIFM_PENDING_CONNECTION; break;    // GetCurrentIpAddress: none
    case 18: resp->result = NIFM_PENDING_CONNECTION; break;    // GetInternetConnectionStatus: not connected
    case 21: case 22: case 32: case 33:  // Is{Wireless,Ethernet}CommunicationEnabled, IsAny{Internet,Foreground}RequestAccepted
        OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    default: LOG("[nifm] IGeneralService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}
static void nifm_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 4 || req->cmd == 5)  // CreateGeneralServiceOld / CreateGeneralService
        ipc_return_object(resp, service_new("IGeneralService", nifm_general, NULL));
    else LOG("[nifm] cmd %u (ok)\n", req->cmd);
}
SERVICE("nifm:u", nifm_u) { nifm_root(s, req, resp); }
SERVICE("nifm:s", nifm_s) { nifm_root(s, req, resp); }
SERVICE("nifm:a", nifm_a) { nifm_root(s, req, resp); }

// ------------------------------------------------------------------ prepo:u / prepo:a / prepo:m / prepo:s
// Play report telemetry services (nn::prepo). Pokémon Legends Arceus periodically submits play reports
// in Jubilife Village and Hisui areas. Stubbed safely so reports succeed without attempting Nintendo telemetry,
// preventing null-dereference crashes on background report threads.
static void prepo_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    resp->result = 0;
    switch (req->cmd) {
    case 10103:  // GetTransmissionStatus
        OUT(uint32_t, 0) = 0;
        resp_size(resp, 4);
        break;
    case 10300: case 20300:  // IsSystemUpdateRequired
        OUT(uint8_t, 0) = 0;
        resp_size(resp, 1);
        break;
    case 10100: case 10101: case 10102: case 10104: case 10105:
    case 10200: case 20100: case 20101: case 20200:
    default:
        LOG("[prepo] %s cmd %u -> ok (local)\n", s->name, req->cmd);
        resp_size(resp, 0x10);
        break;
    }
}
SERVICE("prepo:u", prepo_u) { prepo_service(s, req, resp); }
SERVICE("prepo:a", prepo_a) { prepo_service(s, req, resp); }
SERVICE("prepo:m", prepo_m) { prepo_service(s, req, resp); }
SERVICE("prepo:s", prepo_s) { prepo_service(s, req, resp); }

// ------------------------------------------------------------------ csrng
// GenerateRandomBytes (cmd 0) into the output buffer. Z-A asks it for the seeds of its rolls, hundreds of times a
// session; the logging stub left the buffer as it was, so every roll came out the same - each ball caught (#61).
SERVICE("csrng", csrng) {
    (void)s;
    IpcBuf *b = req->cmd == 0 ? out_buf(req, 0) : NULL;
    if (!b) { LOG("[csrng] cmd %u (ok)\n", req->cmd); return; }
    uint8_t *p = (uint8_t *)(uintptr_t)b->addr;
    for (uint64_t i = 0; i < b->size; i += 4) {
        uint32_t r = arc4random();
        memcpy(p + i, &r, b->size - i < 4 ? b->size - i : 4);
    }
}

// ------------------------------------------------------------------ aoc:u
// Add-on content (DLC): the game root's aoc/<title id, 16 hex digits>.bin files, each the DLC's RomFS image (fs.c
// OpenDataStorageByDataId serves them). Their title ids share the base (application id + 0x1000); the game sees
// their indices (title id - base). None: no DLC.
static int aoc_ids(uint64_t *ids, int max) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", g_romfs_dir ? g_romfs_dir : "extracted/romfs");
    char *slash = strrchr(dir, '/');
    snprintf(slash ? slash + 1 : dir, sizeof dir - (slash ? (size_t)(slash + 1 - dir) : 0), "aoc");
    DIR *d = opendir(dir);
    int n = 0;
    for (struct dirent *e; d && (e = readdir(d)) && n < max;) {
        unsigned long long id;
        char ext[8] = "";
        if (strlen(e->d_name) == 20 && sscanf(e->d_name, "%16llx.%3s", &id, ext) == 2 && !strcmp(ext, "bin")) ids[n++] = id;
    }
    if (d) closedir(d);
    return n;
}

SERVICE("aoc:u", aoc_u) {
    (void)s;
    uint64_t ids[64];
    int n = aoc_ids(ids, 64);
    uint64_t base = n ? ids[0] & ~0xFFFull : 0x01006BB00C6F1000ull;
    static int logged;
    if (!logged++) LOG("[aoc] %d add-on content(s) installed\n", n);
    switch (req->cmd) {
    case 2: OUT(uint32_t, 0) = (uint32_t)n; resp_size(resp, 4); break;  // CountAddOnContent
    case 3: {  // ListAddOnContent(offset, count) -> their indices
        uint32_t off = IN(uint32_t, 0), cnt = IN(uint32_t, 4), k = 0;
        IpcBuf *b = out_buf(req, 0);
        for (int i = (int)off; i < n && k < cnt && b && (k + 1) * 4 <= b->size; i++, k++)
            ((uint32_t *)(uintptr_t)b->addr)[k] = (uint32_t)(ids[i] - base);
        OUT(uint32_t, 0) = k;
        resp_size(resp, 4);
        break;
    }
    case 5: OUT(uint64_t, 0) = base; resp_size(resp, 8); break;  // GetAddOnContentBaseId
    case 7: break;  // PrepareAddOnContent
    case 50: break;  // CheckAddOnContentMountStatus
    case 8: case 10: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;  // GetAddOnContentListChangedEvent(WithProcessId)
    default: ok_logger(s, req, resp);
    }
}

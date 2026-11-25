/* NVIDIA DLAA (DLSS Super Resolution at 1:1, no upscaling) through NVIDIA NGX.

   The NGX static library is built with MSVC and cannot be linked into this MinGW/Zig-built
   dxgi.dll, so all NGX calls live in a small separate DLL (azimuth_ngx.dll, see bridge/) that is
   loaded here at runtime. nvngx_dlss.dll (NVIDIA) must sit next to azimuth.exe as well.

   This file keeps the exact same job (and the same internal API) the FSR backend had:
   jitter sequence, feature creation, dispatch, copy of the result back into the game's
   scene colour. hooks.c is unchanged apart from names. */
#define COBJMACROS
#include "common.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* Exports of azimuth_ngx.dll. */
typedef int (*PfnNgxInit)(ID3D12Device *dev, const wchar_t *dir, int *ngx_result);
typedef int (*PfnNgxEvaluate)(ID3D12GraphicsCommandList *cl, ID3D12Resource *color, ID3D12Resource *depth,
                              ID3D12Resource *mv, ID3D12Resource *out, unsigned w, unsigned h, float jx, float jy,
                              float mv_scale_x, float mv_scale_y, int reset, int *ngx_result);
typedef const char *(*PfnNgxResultString)(int ngx_result);

typedef int (*PfnNgxEvaluateSR)(ID3D12GraphicsCommandList *, ID3D12Resource *, ID3D12Resource *,
                                ID3D12Resource *, ID3D12Resource *, unsigned, unsigned, unsigned, unsigned,
                                int, float, float, float, float, int, int *);
typedef int (*PfnNgxOptimal)(unsigned, unsigned, int, AzSROptimal *, int *);
static PfnNgxEvaluateSR p_evaluate_sr;
static PfnNgxOptimal p_optimal;
static PfnNgxInit p_init;
static PfnNgxEvaluate p_evaluate;
static PfnNgxResultString p_result_string;
static int load_failed;
static int ngx_ready;
static int ngx_failed;

static ID3D12Device *device;
static UINT ctx_w, ctx_h;
static ID3D12Resource *out_tex;
static int out_failed;

static volatile LONG enabled = 1;
static int need_reset = 1;
static int jitter_live;              /* DLSS ran last frame, so jitter the scene */
static int phase_count = 16;
static int jitter_index;
static float jx, jy;                 /* jitter for the frame being recorded */
static int jitter_used;              /* the scene of this frame was rendered with (jx, jy) */

static AzSRMode sr_mode = AZSR_DLAA;
static AzSRQuality sr_quality = AZSR_QUALITY;
static AzSRSize sr_output, sr_render, sr_swapchain;
static int sr_armed, sr_blocked, sr_optimal_checked;
static int sr_route_jitter, sr_trace_only, sr_debug_mode, sr_preview_mode;
static volatile LONG sr_route_visible;
static ID3D12Resource *sr_route_last_output;
static unsigned sr_log_budget = 12;

/* An explicit sidecar enables diagnostics for launcher/RenderDoc launches
   that cannot conveniently inherit environment variables. No config writes. */
unsigned dlss_debug_ini_uint(const wchar_t *key, unsigned fallback) {
    wchar_t path[MAX_PATH];
    DWORD n=GetModuleFileNameW(NULL,path,MAX_PATH);
    if (!n || n>=MAX_PATH) return fallback;
    while(n && path[n-1]!=L'\\') --n;
    if(!n || n+18>=MAX_PATH) return fallback;
    lstrcpyW(path+n,L"azimuth_debug.ini");
    return GetPrivateProfileIntW(L"Debug",key,(INT)fallback,path);
}
static void sr_read_config(void) {
    char b[64], tail;
    DWORD n = GetEnvironmentVariableA("AZIMUTH_SR_MODE", b, sizeof(b));
    if (!n && dlss_debug_ini_uint(L"Enabled",0)==1) { strcpy(b,"debug"); n=5; }
    if (!n || n >= sizeof(b)) return;
    if (!strcmp(b, "trace") || !strcmp(b,"debug")) { sr_mode = AZSR_DIAGNOSTIC; sr_trace_only = 1; sr_debug_mode = !strcmp(b,"debug"); log_msg("SR TRACE ONLY: no DLAA/NGX dispatch or injected motion replay"); }
    else if (!strcmp(b, "diagnostic")) sr_mode = AZSR_DIAGNOSTIC;
    else if (!strcmp(b, "probe")) sr_mode = AZSR_PROBE;
    else if (!strcmp(b,"preview")) { sr_mode=AZSR_PROBE; sr_debug_mode=sr_preview_mode=1; log_msg("SR PREVIEW: offscreen NGX and bounded diagnostics; no visible replacement or jitter"); }
    else if (!strcmp(b, "padded")) sr_mode = AZSR_PADDED;
    else if (!strcmp(b, "route")) sr_mode = AZSR_ROUTE;
    else if (strcmp(b, "dlaa")) log_msg("SR: unknown mode; retaining DLAA");
    if (sr_mode < AZSR_PROBE) return;
    n = GetEnvironmentVariableA("AZIMUTH_SR_OUTPUT", b, sizeof(b));
    if (!n || n >= sizeof(b) || sscanf(b, "%ux%u%c", &sr_output.w, &sr_output.h, &tail) != 2 ||
        sr_output.w < 320 || sr_output.h < 200 || sr_output.w > 7680 || sr_output.h > 4320) {
        sr_blocked = 1;
        log_msg("SR: invalid or missing AZIMUTH_SR_OUTPUT (e.g. 3840x1600); experiment disabled");
    }
    n = GetEnvironmentVariableA("AZIMUTH_SR_QUALITY", b, sizeof(b));
    if (n) {
        if (n >= sizeof(b)) sr_blocked = 1;
        else if (!strcmp(b, "quality")) sr_quality = AZSR_QUALITY;
        else if (!strcmp(b, "balanced")) sr_quality = AZSR_BALANCED;
        else if (!strcmp(b, "performance")) sr_quality = AZSR_PERFORMANCE;
        else sr_blocked = 1;
    }
    if (sr_mode == AZSR_ROUTE) {
        n = GetEnvironmentVariableA("AZIMUTH_SR_ROUTE_JITTER", b, sizeof(b));
        sr_route_jitter = n == 1 && b[0] == '1';
    }
    log_msg("SR experiment: mode=%d output=%ux%u quality=%d blocked=%d", (int)sr_mode,
            sr_output.w, sr_output.h, (int)sr_quality, sr_blocked);
}

AzSRMode dlss_sr_mode(void) { return sr_mode; }
void dlss_sr_note_swapchain(unsigned w, unsigned h) {
    /* Diagnostic only: Azimuth may own multiple swapchains. Never infer primary from last created. */
    sr_swapchain = (AzSRSize){w,h};
    if (sr_mode != AZSR_DLAA) log_msg("SR observed swapchain: %ux%u", w, h);
}
int dlss_sr_jitter_size(unsigned w, unsigned h) {
    if (sr_mode == AZSR_PROBE || sr_trace_only) return 0;
    if (sr_mode == AZSR_ROUTE) return sr_route_jitter && InterlockedCompareExchange(&sr_route_visible,0,0) &&
        sr_armed && !sr_blocked && w == sr_render.w && h == sr_render.h;
    if (sr_mode == AZSR_PADDED) return sr_armed && !sr_blocked && w == sr_render.w && h == sr_render.h;
    return 1;
}

int dlss_sr_trace_only(void) { return sr_trace_only; }
int dlss_sr_debug_mode(void) { return sr_debug_mode; }
int dlss_sr_preview_mode(void) { return sr_preview_mode; }
ID3D12Resource *dlss_sr_route_output(void) { return sr_route_last_output; }
void dlss_sr_route_result(int displayed) {
    InterlockedExchange(&sr_route_visible, displayed != 0);
    if (!displayed) dlss_end_frame(0);
}

static void exe_dir(wchar_t *path) {
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (n && path[n - 1] != L'\\') {
        --n;
    }
    path[n] = 0;
}

static int file_exists(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int load(void) {
    if (p_evaluate) {
        return sr_mode < AZSR_PROBE || (p_evaluate_sr && p_optimal);
    }
    if (load_failed) {
        return 0;
    }
    load_failed = 1;
    wchar_t dir[MAX_PATH], path[MAX_PATH + 32];
    exe_dir(dir);

    lstrcpyW(path, dir);
    lstrcatW(path, L"nvngx_dlss.dll");
    if (!file_exists(path)) {
        log_msg("DLSS: nvngx_dlss.dll not found next to azimuth.exe");
        return 0;
    }
    lstrcpyW(path, dir);
    lstrcatW(path, L"azimuth_ngx.dll");
    HMODULE m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        log_msg("DLSS: azimuth_ngx.dll not found next to azimuth.exe (error %lu)", GetLastError());
        return 0;
    }
    *(FARPROC *)&p_init = GetProcAddress(m, "azngx_init");
    *(FARPROC *)&p_evaluate = GetProcAddress(m, "azngx_evaluate");
    *(FARPROC *)&p_result_string = GetProcAddress(m, "azngx_result_string");
    if (!p_init || !p_evaluate || !p_result_string) {
        log_msg("DLSS: azimuth_ngx.dll exports missing");
        p_evaluate = NULL;
        return 0;
    }
    *(FARPROC *)&p_evaluate_sr = GetProcAddress(m, "azngx_evaluate_sr");
    *(FARPROC *)&p_optimal = GetProcAddress(m, "azngx_get_optimal");
    if (sr_mode >= AZSR_PROBE && (!p_evaluate_sr || !p_optimal)) {
        log_msg("SR experiment requires the rebuilt azimuth_ngx.dll");
        return 0;
    }
    load_failed = 0;
    return 1;
}

void dlss_init(ID3D12Device *d) {
    device = d;
    sr_read_config();
}

int dlss_enabled(void) {
    return enabled;
}

void dlss_toggle(void) {
    enabled = !enabled;
    need_reset = 1;
    jitter_live = 0;
    InterlockedExchange(&sr_route_visible,0);
    log_msg("DLAA %s", enabled ? "ON" : "OFF");
}


int dlss_jitter(float *x, float *y) {
    if (!enabled || !jitter_live || !ngx_ready || sr_trace_only || sr_mode == AZSR_PROBE || sr_blocked) {
        return 0;
    }
    *x = jx;
    *y = jy;
    jitter_used = 1;
    return 1;
}

static float halton(int i, int base) {
    float f = 1.0f, r = 0.0f;
    while (i > 0) {
        f /= (float)base;
        r += f * (float)(i % base);
        i /= base;
    }
    return r;
}

static void next_jitter(void) {
    jitter_index = (jitter_index + 1) % phase_count;
    jx = halton(jitter_index + 1, 2) - 0.5f;
    jy = halton(jitter_index + 1, 3) - 0.5f;
}

static int ensure_ready(UINT w, UINT h) {
    if (ngx_ready && ctx_w == w && ctx_h == h && out_tex) {
        return 1;
    }
    if (ngx_failed || out_failed || !device || !load()) {
        return 0;
    }
    if (!ngx_ready) {
        wchar_t dir[MAX_PATH];
        exe_dir(dir);
        int r = 0;
        if (!p_init(device, dir, &r)) {
            log_msg("DLSS: NGX init failed (%s, 0x%08x). Needs an NVIDIA RTX GPU, a recent driver and nvngx_dlss.dll.",
                    p_result_string(r), (unsigned)r);
            ngx_failed = 1;
            return 0;
        }
        log_msg("DLSS: NGX initialised, DLAA available");
        ngx_ready = 1;
    }
    if (out_tex) {
        /* Size change: in-flight work may still use the old texture, so it is leaked rather
           than released mid-frame. Rare (only when the render size changes). */
        log_msg("DLSS: render size changed to %ux%u", w, h);
        out_tex = NULL;
    }
    D3D12_HEAP_PROPERTIES hp;
    memset(&hp, 0, sizeof(hp));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                                                    &IID_ID3D12Resource, (void **)&out_tex))) {
        log_msg("DLSS: output texture creation failed");
        out_tex = NULL;
        out_failed = 1;
        return 0;
    }
    ID3D12Resource_SetName(out_tex, sr_mode >= AZSR_PROBE ? L"AzimuthSR Experimental Output" : L"AzimuthDLAA Output");
    ctx_w = w;
    ctx_h = h;
    need_reset = 1;
    return 1;
}

#define READ_STATE (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)

int dlss_run(ID3D12GraphicsCommandList *cl, ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
             ID3D12Resource *depth, D3D12_RESOURCE_STATES depth_state, ID3D12Resource *motion,
             D3D12_RESOURCE_STATES motion_state,
             AzSRSize resolve, int top_left, int fh2, int fresh_mv) {
    sr_route_last_output = NULL;
    if (!enabled) {
        return 0;
    }
    D3D12_RESOURCE_DESC cd;
    color->lpVtbl->GetDesc(color, &cd);
    UINT w = (UINT)cd.Width, h = cd.Height;
    UINT ow = w, oh = h;
    AzSRDecision decision = AZSR_REJECT;
    if (sr_mode != AZSR_DLAA) {
        D3D12_RESOURCE_DESC dd, md;
        depth->lpVtbl->GetDesc(depth, &dd);
        motion->lpVtbl->GetDesc(motion, &md);
        AzSRFrame f = {
            {(UINT)cd.Width, cd.Height}, {(UINT)dd.Width, dd.Height},
            {(UINT)md.Width, md.Height}, resolve, sr_output,
            fh2, top_left, cd.Format == DXGI_FORMAT_R11G11B10_FLOAT &&
                cd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                cd.DepthOrArraySize == 1 && cd.MipLevels == 1 &&
                cd.SampleDesc.Count == 1 && cd.SampleDesc.Quality == 0
        };
        if (sr_mode >= AZSR_PROBE && !sr_blocked && fresh_mv)
            decision = azsr_decide(sr_mode, f);
        if (sr_log_budget) {
            --sr_log_budget;
            log_msg("SR frame: color=%ux%u depth=%ux%u mv=%ux%u resolve=%ux%u requested=%ux%u observed_swapchain=%ux%u decision=%d fresh=%d top_left=%d",
                f.color.w,f.color.h,f.depth.w,f.depth.h,f.motion.w,f.motion.h,
                resolve.w,resolve.h,sr_output.w,sr_output.h,sr_swapchain.w,sr_swapchain.h,
                (int)decision,fresh_mv,top_left);
        }
        if (sr_mode >= AZSR_PROBE) {
            if (decision == AZSR_REJECT || cd.SampleDesc.Count != 1 || dd.SampleDesc.Count != 1 ||
                md.SampleDesc.Count != 1 || md.Format != DXGI_FORMAT_R16G16_FLOAT ||
                cd.Format != DXGI_FORMAT_R11G11B10_FLOAT) return 0;
            w = f.motion.w; h = f.motion.h; ow = sr_output.w; oh = sr_output.h;
            if (sr_armed && !azsr_equal(sr_render, f.motion)) {
                sr_blocked = 1;
                log_msg("SR: render dimensions changed; restart required, experiment disabled");
                return 0;
            }
            sr_render = f.motion;
            phase_count = (int)azsr_phase_count(sr_render, sr_output);
            /* Fixed resolution for the experiment. No new resource retirement paths. */
            sr_armed = 1;
        }
    }
    if (!ensure_ready(ow, oh)) return 0;
    if (sr_mode >= AZSR_PROBE && !sr_optimal_checked) {
        AzSROptimal o;
        int result = 0;
        if (!p_optimal(ow, oh, (int)sr_quality, &o, &result)) {
            sr_blocked = 1;
            log_msg("SR optimal query failed: 0x%08x", (unsigned)result);
            return 0;
        }
        log_msg("SR NGX optimal=%ux%u min=%ux%u max=%ux%u actual=%ux%u", o.optimal_w,o.optimal_h,
            o.min_w,o.min_h,o.max_w,o.max_h,w,h);
        if (!azsr_in_range(sr_render,o)) {
            sr_blocked = 1;
            log_msg("SR: actual render size outside selected NGX mode range; restart with supported size");
            return 0;
        }
        sr_optimal_checked = 1;
    }
    /* Everything DLSS reads goes to a shader-read state. */
    if (color_state != READ_STATE) {
        hooks_transition(cl, color, color_state, READ_STATE);
    }
    if (depth_state != READ_STATE) {
        hooks_transition(cl, depth, depth_state, READ_STATE);
    }
    if (motion_state != READ_STATE) {
        hooks_transition(cl, motion, motion_state, READ_STATE);
    }
    int r = 0;
    /* Our motion vectors are UV-space offsets from current to previous position, so the scale
       to pixels is the render size (same convention as the FSR version). */
    int ok;
    if (sr_mode >= AZSR_PROBE) {
        ok = p_evaluate_sr(cl,color,depth,motion,out_tex,w,h,ow,oh,(int)sr_quality,
                          jitter_used ? jx : 0.0f,jitter_used ? jy : 0.0f,
                          (float)w,(float)h,need_reset != 0,&r);
    } else {
        ok = p_evaluate(cl, color, depth, motion, out_tex, w, h, jitter_used ? jx : 0.0f, jitter_used ? jy : 0.0f,
                        (float)w, (float)h, need_reset != 0, &r);
    }
    if(decision==AZSR_EVALUATE_ONLY){
        static unsigned probe_samples;
        if(probe_samples<12||!ok){
            ++probe_samples;
            log_msg("SR PROBE RESULT sample=%u ok=%d ngx=0x%08x input=%ux%u output=%ux%u (CPU API return; GPU content unverified)",probe_samples,ok,(unsigned)r,w,h,ow,oh);
        }
    }
    if (ok && (decision == AZSR_EVALUATE_ONLY || decision == AZSR_ROUTE_OUTPUT)) {
        if (decision == AZSR_ROUTE_OUTPUT) sr_route_last_output = out_tex;
        if(sr_preview_mode && decision==AZSR_EVALUATE_ONLY)hooks_preview_record(cl,color,depth,motion,out_tex,READ_STATE,need_reset);
        /* Offscreen verification: no copy to game, no screen jitter. Order repeated UAV writes. */
        hooks_transition(cl, out_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        hooks_transition(cl, out_tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    } else if (ok) {
        hooks_transition(cl, color, READ_STATE, D3D12_RESOURCE_STATE_COPY_DEST);
        hooks_transition(cl, out_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->lpVtbl->CopyResource(cl, color, out_tex);
        hooks_transition(cl, out_tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        hooks_transition(cl, color, D3D12_RESOURCE_STATE_COPY_DEST, READ_STATE);
    } else {
        if (sr_mode >= AZSR_PROBE) {
            sr_blocked = 1;
            log_msg("SR evaluation failed: experiment disabled until restart");
        }
        static int warned;
        if (warned++ < 5) {
            log_msg("DLSS: evaluate failed (%s, 0x%08x)", p_result_string(r), (unsigned)r);
        }
    }
    if (color_state != READ_STATE) {
        hooks_transition(cl, color, READ_STATE, color_state);
    }
    if (depth_state != READ_STATE) {
        hooks_transition(cl, depth, READ_STATE, depth_state);
    }
    if (motion_state != READ_STATE) {
        hooks_transition(cl, motion, READ_STATE, motion_state);
    }
    if (ok) {
        static int once;
        if (!once) {
            once = 1;
            if (decision == AZSR_EVALUATE_ONLY)
                log_msg("SR PROBE EVALUATED %ux%u -> %ux%u (OFFSCREEN ONLY)", w,h,ow,oh);
            else if (decision == AZSR_ROUTE_OUTPUT)
                log_msg("SR ROUTE EVALUATED %ux%u -> %ux%u (consumer not yet verified)", w,h,ow,oh);
            else if (decision == AZSR_COPY_PADDED)
                log_msg("SR PADDED COPIED %ux%u -> %ux%u (verify downstream sampling)", w,h,ow,oh);
            else log_msg("DLAA active at %ux%u", w, h);
        }
        need_reset = 0;
        /* Next frame's jitter. Advancing here ties the sequence to rendered scenes. */
        next_jitter();
        jitter_used = 0;
    }
    return ok;
}

void dlss_end_frame(int ran) {
    jitter_live = ran && enabled;
    if (!ran) {
        if (sr_mode == AZSR_ROUTE) InterlockedExchange(&sr_route_visible,0);
        need_reset = 1;
        jitter_used = 0;
    }
}

/* NVIDIA DLAA (DLSS Super Resolution at 1:1, no upscaling) through NVIDIA NGX.

   The NGX static library is built with MSVC and cannot be linked into this MinGW/Zig-built
   dxgi.dll, so all NGX calls live in a small separate DLL (azimuth_ngx.dll, see bridge/) that is
   loaded here at runtime. nvngx_dlss.dll (NVIDIA) must sit next to azimuth.exe as well.

   This file keeps the exact same job (and the same internal API) the FSR backend had:
   jitter sequence, feature creation, dispatch, copy of the result back into the game's
   scene colour. hooks.c is unchanged apart from names. */
#define COBJMACROS
#include "common.h"
#include "replay_profile.h"
#include "lod_policy.h"
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
static int sr_preset,sr_play,sr_auto_render,sr_auto_quality;
static float sr_lod_offset;
static int sr_lod_enabled;
static AzSRStable sr_stable;
static AzSRGuideOptions sr_guides={0,0,1,1,1,1};
static int sr_guides_override;
static uint64_t sr_last_evaluate_ms;
static unsigned sr_guide_log_budget=16;
typedef int (*PfnNgxSetGuides)(const AzSRGuideOptions *);
static PfnNgxSetGuides p_set_guides;
static volatile LONG sr_play_disabled,sr_play_reset;
static AzSRSize sr_fixed_render;
typedef int (*PfnNgxSetPreset)(int);
static PfnNgxSetPreset p_set_preset;
static int sr_route_jitter, sr_trace_only, sr_debug_mode, sr_preview_mode, sr_preview_jitter;
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
/* Sidecar settings are read once. Explicit legacy/diagnostic env mode wins.
   Never modify Azimuth game settings or assume its internal resolution. */
static int sr_ini_path(wchar_t path[MAX_PATH]){
    DWORD n=GetModuleFileNameW(NULL,path,MAX_PATH);if(!n||n>=MAX_PATH)return 0;
    while(n&&path[n-1]!=L'\\')--n;
    if(!n||n+16>=MAX_PATH)return 0;
    lstrcpyW(path+n,L"azimuth_sr.ini");return 1;
}
static int sr_ini_value(const wchar_t *key,const wchar_t *fallback,char *out,unsigned cap){
    wchar_t path[MAX_PATH],value[64];
    if(!sr_ini_path(path))return 0;
    DWORD n=GetPrivateProfileStringW(L"SR",key,fallback,value,64,path);
    if(n>=63)return 0;
    return WideCharToMultiByte(CP_UTF8,0,value,-1,out,(int)cap,NULL,NULL)>0;
}
static int sr_play_config(void){
    char render[32],output[32],quality[32],hdr[8],jitter[8],preset[16],hud[16],di[8],mj[8],mx[32],my[32],pe[32],es[32],lod[32];
    if(!sr_ini_value(L"Render",L"auto",render,sizeof(render))||
       !sr_ini_value(L"Output",L"1920x1080",output,sizeof(output))||
       !sr_ini_value(L"Quality",L"quality",quality,sizeof(quality))||
       !sr_ini_value(L"HDR",L"0",hdr,sizeof(hdr))||
       !sr_ini_value(L"Jitter",L"1",jitter,sizeof(jitter))||
       !sr_ini_value(L"Preset",L"default",preset,sizeof(preset))||
       !sr_ini_value(L"HUD",L"native",hud,sizeof(hud))||
       !sr_ini_value(L"DepthInverted",L"0",di,sizeof(di))||
       !sr_ini_value(L"MVJittered",L"0",mj,sizeof(mj))||
       !sr_ini_value(L"MVScaleX",L"1",mx,sizeof(mx))||
       !sr_ini_value(L"MVScaleY",L"1",my,sizeof(my))||
       !sr_ini_value(L"PreExposure",L"1",pe,sizeof(pe))||
       !sr_ini_value(L"ExposureScale",L"1",es,sizeof(es))||
       !sr_ini_value(L"LodBias",L"off",lod,sizeof(lod)))return 0;
    AzReplayProfile profile=!strcmp(render,"auto")?azrp_profile_request(render,output):azrp_profile_explicit(render,output);
    if(!profile.ow||(strcmp(hdr,"0")&&strcmp(hdr,"1"))||(strcmp(jitter,"0")&&strcmp(jitter,"1"))||
       (strcmp(quality,"quality")&&strcmp(quality,"balanced")&&strcmp(quality,"performance")&&strcmp(quality,"ultraperformance")&&strcmp(quality,"auto"))||
       azsr_preset(preset)<0||(strcmp(hud,"native")&&strcmp(hud,"delta")))return 0;
    sr_fixed_render=(AzSRSize){profile.rw,profile.rh};
    const char *keys[]={"AZIMUTH_SR_RENDER","AZIMUTH_SR_OUTPUT","AZIMUTH_SR_QUALITY","AZIMUTH_SR_PRESET",
        "AZIMUTH_SR_HUD","AZIMUTH_SR_DEPTH_INVERTED","AZIMUTH_SR_MV_JITTERED","AZIMUTH_SR_MV_SCALE_X","AZIMUTH_SR_MV_SCALE_Y","AZIMUTH_SR_PRE_EXPOSURE","AZIMUTH_SR_EXPOSURE_SCALE","AZIMUTH_SR_LOD_BIAS",
        "AZIMUTH_DEBUG_PREVIEW_JITTER","AZIMUTH_DEBUG_VISIBLE_HDR","AZIMUTH_DEBUG_HDR_LIVE",
        "AZIMUTH_DEBUG_PREVIEW_POSTFX","AZIMUTH_DEBUG_POSTFX_REPLAY","AZIMUTH_DEBUG_SR_REPLAY",
        "AZIMUTH_DEBUG_VISIBLE_SR","AZIMUTH_DEBUG_AUTO_TEST","AZIMUTH_DEBUG_PRODUCER_GPU_WAIT",
        "AZIMUTH_DEBUG_PRODUCER_WAIT_MS","AZIMUTH_DEBUG_VISIBLE_MARKER",
        "AZIMUTH_DEBUG_PRESENT_TRACE","AZIMUTH_DEBUG_RENDERDOC","AZIMUTH_DEBUG_SHADER_DISASM","AZIMUTH_DEBUG_QUEUE_TRACE"};
    const char *values[]={render,output,quality,preset,hud,di,mj,mx,my,pe,es,lod,jitter,hdr,hdr,"1","1","1","1","1","1","0","0","0","0","0","1"};
    for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);++i)if(!SetEnvironmentVariableA(keys[i],values[i]))return 0;
    (void)SetEnvironmentVariableA("AZIMUTH_DEBUG_AUTO_READY_FILE",NULL);
    log_msg("SR PLAY_CONFIG render=%s output=%s quality=%s hdr=%s jitter=%s preset=%s gpuWait=1 capture=0 profileLatch=1",render,output,quality,hdr,jitter,preset);
    return 1;
}
int dlss_sr_play_mode(void){return sr_play;}
int dlss_sr_play_available(void){return sr_play&&!InterlockedCompareExchange(&sr_play_disabled,0,0);}
void dlss_sr_play_disable(const char *reason){
    if(sr_play&&!InterlockedExchange(&sr_play_disabled,1))log_msg("SR PLAY_DISABLED reason=%s restartRequired=1",reason);
}
void dlss_sr_play_miss(void){if(sr_play)InterlockedExchange(&sr_play_reset,1);}
static int sr_read_guides(void){
    const char *flags[]={"AZIMUTH_SR_DEPTH_INVERTED","AZIMUTH_SR_MV_JITTERED"};
    unsigned *fp[]={&sr_guides.depth_inverted,&sr_guides.mv_jittered};
    const char *numbers[]={"AZIMUTH_SR_MV_SCALE_X","AZIMUTH_SR_MV_SCALE_Y","AZIMUTH_SR_PRE_EXPOSURE","AZIMUTH_SR_EXPOSURE_SCALE"};
    float *np[]={&sr_guides.mv_scale_x,&sr_guides.mv_scale_y,&sr_guides.pre_exposure,&sr_guides.exposure_scale};
    char b[64],tail;
    for(unsigned i=0;i<2;++i){
        DWORD n=GetEnvironmentVariableA(flags[i],b,sizeof(b));
        if(n){if(n!=1||(b[0]!='0'&&b[0]!='1'))return 0;*fp[i]=(unsigned)(b[0]-'0');}
    }
    for(unsigned i=0;i<4;++i){
        DWORD n=GetEnvironmentVariableA(numbers[i],b,sizeof(b));
        if(n&&(n>=sizeof(b)||sscanf(b,"%f%c",np[i],&tail)!=1))return 0;
    }
    sr_guides_override=sr_guides.depth_inverted||sr_guides.mv_jittered||sr_guides.mv_scale_x!=1||
        sr_guides.mv_scale_y!=1||sr_guides.pre_exposure!=1||sr_guides.exposure_scale!=1;
    return azsr_guides_valid(sr_guides);
}
static void sr_read_lod(void){
    char value[64],tail;
    DWORD n=GetEnvironmentVariableA("AZIMUTH_SR_LOD_BIAS",value,sizeof(value));
    if(!n)return;
    if(n>=sizeof(value)){log_msg("LOD CONFIG disabled reason=invalid_value");return;}
    if(!strcmp(value,"off"))return;
    if(!sr_preview_mode||sr_blocked){log_msg("LOD CONFIG disabled reason=not_valid_sr_preview");return;}
    if(!strcmp(value,"auto")){
        /* Samplers may be created before the first resolve: only an explicit
           fixed input is usable here. Auto render must not guess a ratio. */
        if(sr_auto_render||!azsr_same_aspect(sr_fixed_render,sr_output)||!azlod_auto(sr_fixed_render.w,sr_output.w,&sr_lod_offset)){
            log_msg("LOD CONFIG disabled reason=auto_requires_fixed_render");return;
        }
    }else if(sscanf(value,"%f%c",&sr_lod_offset,&tail)!=1||!azlod_offset_valid(sr_lod_offset)){
        log_msg("LOD CONFIG disabled reason=invalid_offset allowed=-2.5..0");return;
    }
    sr_lod_enabled=sr_lod_offset!=0;
    log_msg("LOD CONFIG enabled=%d offset=%.6f scope=main_exe_dynamic_aniso_wrap staticUntouched=1 restartRequired=1 requestedRender=%ux%u requestedOutput=%ux%u",
        sr_lod_enabled,(double)sr_lod_offset,sr_fixed_render.w,sr_fixed_render.h,sr_output.w,sr_output.h);
}
int dlss_sr_lod_bias(float *bias){
    if(!bias||!sr_lod_enabled)return 0;
    *bias=sr_lod_offset;return 1;
}
static void sr_read_config(void) {
    char b[64], tail;
    DWORD n = GetEnvironmentVariableA("AZIMUTH_SR_MODE", b, sizeof(b));
    if(!n){wchar_t path[MAX_PATH];if(sr_ini_path(path)&&GetPrivateProfileIntW(L"SR",L"Enabled",0,path)==1){strcpy(b,"play");n=4;}}
    if (!n && dlss_debug_ini_uint(L"Enabled",0)==1) { strcpy(b,"debug"); n=5; }
    if (!n || n >= sizeof(b)) return;
    if (!strcmp(b, "trace") || !strcmp(b,"debug")) { sr_mode = AZSR_DIAGNOSTIC; sr_trace_only = 1; sr_debug_mode = !strcmp(b,"debug"); log_msg("SR TRACE ONLY: no DLAA/NGX dispatch or injected motion replay"); }
    else if (!strcmp(b, "diagnostic")) sr_mode = AZSR_DIAGNOSTIC;
    else if (!strcmp(b, "probe")) sr_mode = AZSR_PROBE;
    else if (!strcmp(b,"play")) {
        sr_play=1;sr_mode=AZSR_PROBE;sr_debug_mode=sr_preview_mode=1;
        if(!sr_play_config()){sr_blocked=1;dlss_sr_play_disable("invalid_ini_or_environment");return;}
    }
    else if (!strcmp(b,"preview")) { sr_mode=AZSR_PROBE; sr_debug_mode=sr_preview_mode=1; log_msg("SR PREVIEW: offscreen NGX and bounded diagnostics; no visible SR replacement"); }
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
        else if (!strcmp(b, "ultraperformance")) sr_quality = AZSR_ULTRA_PERFORMANCE;
        else if (!strcmp(b, "auto")&&sr_preview_mode) sr_auto_quality=1;
        else sr_blocked = 1;
    }
    n=GetEnvironmentVariableA("AZIMUTH_SR_RENDER",b,sizeof(b));
    if(n&&n<sizeof(b)&&!strcmp(b,"auto")&&sr_preview_mode){
        sr_auto_render=1;sr_fixed_render=(AzSRSize){0,0};
        if(sr_output.w>3840||sr_output.h>2160)sr_blocked=1;
    }else if(n&&(n>=sizeof(b)||!azrp_parse_size(b,&sr_fixed_render)||!azsr_less(sr_fixed_render,sr_output)||!azsr_same_aspect(sr_fixed_render,sr_output)))sr_blocked=1;
    n=GetEnvironmentVariableA("AZIMUTH_SR_PRESET",b,sizeof(b));
    if(n){sr_preset=n<sizeof(b)?azsr_preset(b):-1;if(sr_preset<0)sr_blocked=1;}
    if(!sr_read_guides())sr_blocked=1;
    if(sr_play&&sr_blocked)dlss_sr_play_disable("invalid_profile_or_preset_or_guides");
    log_msg("NR GUIDE_CONFIG depthInverted=%u mvJittered=%u scaleMultiplier=%.6f,%.6f preExposure=%.6f exposureScale=%.6f autoExposure=1 calibrated=%d engineExposure=unverified",sr_guides.depth_inverted,sr_guides.mv_jittered,(double)sr_guides.mv_scale_x,(double)sr_guides.mv_scale_y,(double)sr_guides.pre_exposure,(double)sr_guides.exposure_scale,sr_guides_override);
    log_msg("SR PRESET requested=%d hintOnly=1",sr_preset);
    sr_read_lod();
    if(sr_preview_mode){
        n=GetEnvironmentVariableA("AZIMUTH_DEBUG_PREVIEW_JITTER",b,sizeof(b));
        sr_preview_jitter=n==1&&b[0]=='1';
        log_msg("PREVIEW TEMPORAL viewportJitter=%d (experimental; original displayed scene may be jittered)",sr_preview_jitter);
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
    if (sr_trace_only||(sr_play&&!dlss_sr_play_available())) return 0;
    if (sr_mode == AZSR_PROBE) return sr_preview_mode&&sr_preview_jitter&&sr_armed&&!sr_blocked&&w==sr_render.w&&h==sr_render.h;
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
    *(FARPROC *)&p_set_preset=GetProcAddress(m,"azngx_set_sr_preset");
    *(FARPROC *)&p_set_guides=GetProcAddress(m,"azngx_set_sr_guides");
    if(sr_guides_override&&(!p_set_guides||!p_set_guides(&sr_guides))){log_msg("SR: guide override requires compatible bridge");p_evaluate=NULL;return 0;}
    if(sr_preset>0&&(!p_set_preset||!p_set_preset(sr_preset))){log_msg("SR: requested preset requires compatible bridge");p_evaluate=NULL;return 0;}
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
    return enabled&&(!sr_play||dlss_sr_play_available());
}

void dlss_toggle(void) {
    enabled = !enabled;
    need_reset = 1;
    jitter_live = 0;
    InterlockedExchange(&sr_route_visible,0);
    log_msg("DLAA %s", enabled ? "ON" : "OFF");
}


int dlss_jitter(float *x, float *y) {
    if (!dlss_enabled() || !jitter_live || !ngx_ready || sr_trace_only || (sr_mode == AZSR_PROBE && !(sr_preview_mode&&sr_preview_jitter)) || sr_blocked) {
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
    if (!dlss_enabled()) {
        return 0;
    }
    if(sr_play&&InterlockedExchange(&sr_play_reset,0))need_reset=1;
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
            int eligible=decision!=AZSR_REJECT&&cd.SampleDesc.Count==1&&dd.SampleDesc.Count==1&&
                md.SampleDesc.Count==1&&md.Format==DXGI_FORMAT_R16G16_FLOAT&&cd.Format==DXGI_FORMAT_R11G11B10_FLOAT;
            if(sr_auto_render&&!sr_armed){
                eligible=eligible&&azrp_profile_sizes(f.motion,sr_output).ow!=0;
                if(!azsr_stable(&sr_stable,f.motion,eligible))return 0;
            }
            if(!eligible)return 0;
            if(sr_fixed_render.w&&!azsr_equal(sr_fixed_render,f.motion)){
                static int mismatch_logged;
                if(!mismatch_logged++){log_msg("SR PROFILE_MISMATCH requested=%ux%u measured=%ux%u native=1",sr_fixed_render.w,sr_fixed_render.h,f.motion.w,f.motion.h);}
                if(sr_armed){sr_blocked=1;dlss_sr_play_disable("render_dimensions_changed");}
                return 0;
            }
            w = f.motion.w; h = f.motion.h; ow = sr_output.w; oh = sr_output.h;
            if (sr_armed && !azsr_equal(sr_render, f.motion)) {
                sr_blocked = 1;
                log_msg("SR: render dimensions changed; restart required, experiment disabled");
                dlss_sr_play_disable("render_dimensions_changed");
                return 0;
            }
            sr_render = f.motion;
            phase_count = (int)azsr_phase_count(sr_render, sr_output);
            /* Arm only after the runtime range and replay profile accept. */
        }
    }
    if (!ensure_ready(ow, oh)){dlss_sr_play_disable("ngx_or_output_init");return 0;}
    if (sr_mode >= AZSR_PROBE && !sr_optimal_checked) {
        AzSROptimal o={0};int result=0;
        if(sr_auto_quality){
            double best=-1.0;AzSRQuality chosen=AZSR_NATIVE;
            for(int q=AZSR_QUALITY;q<=AZSR_ULTRA_PERFORMANCE;++q){
                AzSROptimal candidate={0};int rc=0;
                int ok=p_optimal(ow,oh,q,&candidate,&rc);
                double score=ok?azsr_quality_distance(sr_render,candidate):-1.0;
                log_msg("SR AUTO_QUALITY mode=%d query=%d ngx=%08x min=%ux%u max=%ux%u optimal=%ux%u eligible=%d",
                    q,ok,(unsigned)rc,candidate.min_w,candidate.min_h,candidate.max_w,candidate.max_h,candidate.optimal_w,candidate.optimal_h,score>=0);
                if(score>=0&&(best<0||score<best)){best=score;chosen=(AzSRQuality)q;o=candidate;}
            }
            if(chosen==AZSR_NATIVE){sr_blocked=1;dlss_sr_play_disable("ngx_no_supported_auto_quality");return 0;}
            sr_quality=chosen;
            log_msg("SR AUTO_QUALITY_SELECTED mode=%d actual=%ux%u",sr_quality,w,h);
        }else if (!p_optimal(ow, oh, (int)sr_quality, &o, &result)) {
            sr_blocked = 1;
            log_msg("SR optimal query failed: 0x%08x", (unsigned)result);
            dlss_sr_play_disable("ngx_optimal_query");
            return 0;
        }
        log_msg("SR NGX optimal=%ux%u min=%ux%u max=%ux%u actual=%ux%u", o.optimal_w,o.optimal_h,
            o.min_w,o.min_h,o.max_w,o.max_h,w,h);
        if (!azsr_in_range(sr_render,o)) {
            sr_blocked = 1;
            log_msg("SR: actual render size outside selected NGX mode range; restart with supported size");
            dlss_sr_play_disable("ngx_render_outside_range");
            return 0;
        }
        if(sr_auto_render&&!hooks_sr_accept_profile(w,h,ow,oh)){
            sr_blocked=1;dlss_sr_play_disable("auto_profile_replay");return 0;
        }
        sr_optimal_checked=1;sr_armed=1;
        log_msg("SR PROFILE_ACCEPTED render=%ux%u output=%ux%u quality=%d autoRender=%d autoQuality=%d restartOnChange=1",w,h,ow,oh,sr_quality,sr_auto_render,sr_auto_quality);
        hooks_lod_profile_report(w,h,ow,oh);
    }
    if(sr_mode>=AZSR_PROBE){
        uint64_t now=GetTickCount64();
        if(sr_last_evaluate_ms&&now-sr_last_evaluate_ms>500){
            need_reset=1;log_msg("SR HISTORY_RESET reason=evaluation_gap gapMs=%llu",(unsigned long long)(now-sr_last_evaluate_ms));
        }
        sr_last_evaluate_ms=now;
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
        float sx=(float)w*sr_guides.mv_scale_x,sy=(float)h*sr_guides.mv_scale_y;
        int reset=need_reset!=0;
        ok = p_evaluate_sr(cl,color,depth,motion,out_tex,w,h,ow,oh,(int)sr_quality,
                          jitter_used ? jx : 0.0f,jitter_used ? jy : 0.0f,sx,sy,reset,&r);
        if(sr_guide_log_budget||!ok||reset){
            if(sr_guide_log_budget)--sr_guide_log_budget;
            log_msg("NR INPUT_EVALUATE list=%p color=%p depth=%p mv=%p output=%p render=%ux%u outputSize=%ux%u quality=%d mvScale=%.7f,%.7f jitter=%.7f,%.7f depthInverted=%u mvJittered=%u reset=%d fresh=%d preExposure=%.7f exposureScale=%.7f autoExposure=1 ok=%d ngx=%08x receiverUnverified=1",
                (void *)cl,(void *)color,(void *)depth,(void *)motion,(void *)out_tex,w,h,ow,oh,sr_quality,(double)sx,(double)sy,
                (double)(jitter_used?jx:0),(double)(jitter_used?jy:0),sr_guides.depth_inverted,sr_guides.mv_jittered,reset,fresh_mv,(double)sr_guides.pre_exposure,(double)sr_guides.exposure_scale,ok,(unsigned)r);
        }
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
        if(sr_preview_mode && decision==AZSR_EVALUATE_ONLY)hooks_preview_record(cl,color,depth,motion,out_tex,READ_STATE,need_reset,jitter_used,jitter_used?jx:0.0f,jitter_used?jy:0.0f,fresh_mv);
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
            dlss_sr_play_disable("ngx_evaluation");
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

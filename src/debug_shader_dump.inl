/* Opt-in local disassembly of recognized temporal passes and exact FH2
   postprocess candidates. Does not issue GPU commands or upload data. */
#define AZSD_LIMIT 40
#define AZSD_MAX_BYTES (1024u*1024u)
static SRWLOCK azsd_lock=SRWLOCK_INIT;
static int azsd_enabled,azsd_folder_ready;
static unsigned azsd_count;
static struct {DWORD hash[4];char stage;} azsd_seen[AZSD_LIMIT];
static wchar_t azsd_dir[MAX_PATH];
static char azsd_name[80];

static void debug_shader_dump_init(void) {
    char b[8];DWORD n=GetEnvironmentVariableA("AZIMUTH_DEBUG_SHADER_DISASM",b,sizeof(b));
    azsd_enabled=n?(n==1&&b[0]=='1'):dlss_debug_ini_uint(L"ShaderDisasm",0)==1;
    if(azsd_enabled)log_msg("DEBUG SHADER_DUMP enabled: local selected disassembly; review before sharing");
}
static int azsd_hash(const D3D12_SHADER_BYTECODE *s,DWORD h[4]) {
    if(!s||!s->pShaderBytecode||s->BytecodeLength<20||memcmp(s->pShaderBytecode,"DXBC",4))return 0;
    memcpy(h,(const BYTE *)s->pShaderBytecode+4,16);
    return (h[0]|h[1]|h[2]|h[3])!=0;
}
static int azsd_make_dir(void) {
    if(azsd_folder_ready)return 1;
    DWORD n=GetModuleFileNameW(NULL,azsd_dir,MAX_PATH);
    if(!n||n>=MAX_PATH)return 0;
    while(n&&azsd_dir[n-1]!=L'\\')--n;
    if(!n)return 0;
    int m=snprintf(azsd_name,sizeof(azsd_name),"azimuth_shader_debug_%lu_%llu",
        (unsigned long)GetCurrentProcessId(),(unsigned long long)GetTickCount64());
    if(m<=0||(size_t)m>=sizeof(azsd_name)||n+(DWORD)m+1>=MAX_PATH)return 0;
    if(!MultiByteToWideChar(CP_UTF8,0,azsd_name,-1,azsd_dir+n,(int)(MAX_PATH-n)))return 0;
    if(!CreateDirectoryW(azsd_dir,NULL))return 0;
    azsd_folder_ready=1;return 1;
}
static void azsd_dump(const D3D12_SHADER_BYTECODE *s,char stage,const DWORD hash[4]) {
    if(azsd_count>=AZSD_LIMIT)return;
    for(unsigned i=0;i<azsd_count;++i)
        if(azsd_seen[i].stage==stage&&!memcmp(azsd_seen[i].hash,hash,16))return;
    unsigned index=azsd_count++;
    azsd_seen[index].stage=stage;memcpy(azsd_seen[index].hash,hash,16);
    char filename[80];
    int m=snprintf(filename,sizeof(filename),"%cs-%08lx-%08lx-%08lx-%08lx.asm.txt",stage,
        (unsigned long)hash[0],(unsigned long)hash[1],(unsigned long)hash[2],(unsigned long)hash[3]);
    if(m<=0||(size_t)m>=sizeof(filename))return;
    if(!azsd_make_dir()){log_msg("DEBUG SHADER_DUMP cannot create local directory");return;}
    typedef HRESULT(WINAPI *DisassembleFn)(LPCVOID,SIZE_T,UINT,LPCSTR,ID3DBlob **);
    static DisassembleFn fn;
    if(!fn){
        HMODULE mod=LoadLibraryExW(L"d3dcompiler_47.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(mod)fn=(DisassembleFn)(void *)GetProcAddress(mod,"D3DDisassemble");
        if(!fn){log_msg("DEBUG SHADER_DUMP D3DDisassemble unavailable");return;}
    }
    ID3DBlob *code=NULL;
    HRESULT hr=fn(s->pShaderBytecode,s->BytecodeLength,0,NULL,&code);
    if(FAILED(hr)||!code){
        log_msg("DEBUG SHADER_DUMP failed file=%s hr=%08lx",filename,(unsigned long)hr);
        if(code)ID3D10Blob_Release(code);
        return;
    }
    SIZE_T size=ID3D10Blob_GetBufferSize(code);
    const BYTE *bytes=(const BYTE *)ID3D10Blob_GetBufferPointer(code);
    if(size&&bytes&&bytes[size-1]==0)--size;
    wchar_t file[MAX_PATH];int n=lstrlenW(azsd_dir);
    if(bytes&&size>0&&size<=AZSD_MAX_BYTES&&n+1+m+1<MAX_PATH){
        lstrcpyW(file,azsd_dir);file[n++]=L'\\';
        if(MultiByteToWideChar(CP_UTF8,0,filename,-1,file+n,MAX_PATH-n)){
            HANDLE out=CreateFileW(file,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
            if(out!=INVALID_HANDLE_VALUE){
                DWORD written=0;
                BOOL ok=WriteFile(out,bytes,(DWORD)size,&written,NULL);CloseHandle(out);
                if(ok&&written==(DWORD)size)
                    log_msg("DEBUG SHADER_DUMP directory=%s file=%s bytes=%lu",azsd_name,filename,(unsigned long)written);
                else {DeleteFileW(file);log_msg("DEBUG SHADER_DUMP write failed file=%s",filename);}
            }else log_msg("DEBUG SHADER_DUMP file creation failed file=%s",filename);
        }
    }else log_msg("DEBUG SHADER_DUMP skipped file=%s (size or path limit)",filename);
    ID3D10Blob_Release(code);
}
/* Diagnostic selection only: observed 671/672 overlay and small
   auxiliary pass hashes. Never use these prefixes to authorize draws. */
static int azsd_hud_candidate(const AztPso *p){
    static const DWORD ps[]={0x117c5517,0x5d0b144f,0x63e8c8ed,0x6fe1ad18,0xb276dc3a,0xdceb99b4,0xe792b40f,0xe86fb202,0x2c4e97c6,0xcd61498f,0xf28be0e9};
    for(unsigned i=0;i<sizeof(ps)/sizeof(ps[0]);++i)if(p->hash[0]==ps[i])return 1;
    return 0;
}
static void debug_shader_dump_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,int role) {
    if(!azsd_enabled||!desc)return;
    AztPso candidate;memset(&candidate,0,sizeof(candidate));
    if(!azsd_hash(&desc->PS,candidate.hash)||!azsd_hash(&desc->VS,candidate.vs_hash))return;
    char visible[8];DWORD n=GetEnvironmentVariableA("AZIMUTH_DEBUG_VISIBLE_SR",visible,sizeof(visible));
    if(role==ROLE_NONE&&!azfg_known_candidate(&candidate)&&!(n==1&&visible[0]=='1'&&azsd_hud_candidate(&candidate)))return;
    AcquireSRWLockExclusive(&azsd_lock);
    azsd_dump(&desc->PS,'p',candidate.hash);azsd_dump(&desc->VS,'v',candidate.vs_hash);
    ReleaseSRWLockExclusive(&azsd_lock);
}

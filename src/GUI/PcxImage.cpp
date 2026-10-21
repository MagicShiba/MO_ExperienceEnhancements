/* PcxImage - DLL 内嵌 PCX 的加载与绘制。
 *
 * 资源由 icons.rc 编译进 DLL。ForceLoadFile 只接受文件名, 因此首次加载时把
 * 资源写到 %TEMP%, 读入引擎缓存后删除; 之后每帧只查引擎面表, 不碰磁盘。
 * 1 plane 8bpp -> 索引面 (文本覆盖率); 3 plane 24bpp -> 16 位真彩面。
 * 只读; 不改模拟状态。 */
#include <windows.h>
#include "GdiText.h"
#include "IconRes.h"
#include "PcxImage.h"

#define ADDR_PCX_INSTANCE 0xAC4848u
#define PCX_RES_N         5

typedef void* (__thiscall *PcxGetSurfFn)(void* pcx, const char* name, void* pal);
/* 引擎返回 bool (al), 用 unsigned char 读。 */
typedef unsigned char (__thiscall *PcxLoadFn)(void* pcx, const char* name,
                                              int f1, int f2);
typedef unsigned char (__thiscall *PcxBlitFn)(void* pcx, RectangleStruct* r,
                                              void* target, void* surf,
                                              unsigned short key);

#define FN_PCXGET  ((PcxGetSurfFn)(void*)0x6BA140u)
#define FN_PCXLOAD ((PcxLoadFn)(void*)0x6B9D00u)
#define FN_PCXBLIT ((PcxBlitFn)(void*)0x6BA580u)

typedef struct {
    const char* name;
    int res;
} PcxDef;

static const PcxDef s_Defs[PCX_RES_N] = {
    { "icon_aggressive.pcx", ICONRES_AGGRESSIVE },
    { "icon_cost.pcx",       ICONRES_COST },
    { "icon_power_down.pcx", ICONRES_POWER_DOWN },
    { "icon_power_up.pcx",   ICONRES_POWER_UP },
    { "icon_time.pcx",       ICONRES_TIME }
};

/* 每个资源只拼一次路径、只真正加载一次; 之后每帧仅查引擎面表。 */
typedef struct {
    char path[MAX_PATH_LEN];
    unsigned char warned;
    unsigned char logged;
    unsigned char loaded;
    unsigned char failed;
} PcxEntry;

static PcxEntry s_Ent[PCX_RES_N];
static int s_UnknownWarned = 0;
static const char* s_LastName = NULL;
static int s_LastIdx = -1;

/* 调用方每帧都传同一批字符串字面量, 命中上次结果可省掉整表比较。 */
static int PcxIndexOf(const char* name)
{
    int i;
    if (!name) return -1;
    if (name == s_LastName) return s_LastIdx;
    for (i = 0; i < PCX_RES_N; i++)
    {
        if (lstrcmpiA(name, s_Defs[i].name) == 0)
        {
            s_LastName = name;
            return s_LastIdx = i;
        }
    }
    s_LastName = name;
    s_LastIdx = -1;
    return -1;
}

static void PcxWarnAt(int idx, const char* name, const char* why)
{
    char buf[256];
    unsigned q = 0;
    if (idx >= 0)
    {
        if (s_Ent[idx].warned) return;
        s_Ent[idx].warned = 1;
    }
    else
    {
        if (s_UnknownWarned) return;
        s_UnknownWarned = 1;
    }
    q = AppendCStr(buf, q, "pcx: ", sizeof(buf));
    q = AppendCStr(buf, q, name ? name : "?", sizeof(buf));
    q = AppendCStr(buf, q, " ", sizeof(buf));
    AppendCStr(buf, q, why, sizeof(buf));
    LogLine(buf);
}

/* 只在真正加载成功时打一次 (预载即加载)。 */
static void PcxLogOkAt(int idx, const char* name, int w, int h, int bpp,
                       unsigned short key)
{
    char buf[256], b[16];
    unsigned q = 0;
    if (idx < 0 || s_Ent[idx].logged) return;
    s_Ent[idx].logged = 1;
    q = AppendCStr(buf, q, "pcx: ", sizeof(buf));
    q = AppendCStr(buf, q, name, sizeof(buf));
    q = AppendCStr(buf, q, " ", sizeof(buf));
    UtoA((unsigned)w, b);
    q = AppendCStr(buf, q, b, sizeof(buf));
    q = AppendCh(buf, q, 'x', sizeof(buf));
    UtoA((unsigned)h, b);
    q = AppendCStr(buf, q, b, sizeof(buf));
    q = AppendCStr(buf, q, " bpp=", sizeof(buf));
    UtoA((unsigned)bpp, b);
    q = AppendCStr(buf, q, b, sizeof(buf));
    if (bpp == 2)
    {
        HtoA((unsigned)key, b);
        q = AppendCStr(buf, q, " key=", sizeof(buf));
        q = AppendCStr(buf, q, b, sizeof(buf));
    }
    LogLine(buf);
}

/* 引擎的 PCX 名称表按小写键查找; 路径必须整体小写, 否则 GetSurface 找不到。 */
static void PcxLowerAscii(char* s)
{
    if (!s) return;
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

static int PcxTempPath(const char* name, char* out, int cap)
{
    char dir[MAX_PATH], b[16];
    DWORD n;
    unsigned q;
    if (cap < 2) return 0;
    n = GetTempPathA(MAX_PATH, dir);
    if (!n || n >= MAX_PATH) return 0;
    q = AppendCStr(out, 0, dir, (unsigned)cap);
    q = AppendCStr(out, q, "moe_", (unsigned)cap);
    UtoA(GetCurrentProcessId(), b);
    q = AppendCStr(out, q, b, (unsigned)cap);
    q = AppendCh(out, q, '_', (unsigned)cap);
    q = AppendCStr(out, q, name, (unsigned)cap);
    if (q >= (unsigned)cap) return 0;
    PcxLowerAscii(out);
    return 1;
}

static HMODULE PcxThisModule(void)
{
    HMODULE h = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void*)&PcxThisModule, &h);
    return h;
}

static int PcxWriteTemp(const unsigned char* data, DWORD size, const char* path)
{
    HANDLE h;
    DWORD total = 0, wrote = 0;
    if (!data || !size) return 0;
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    while (total < size)
    {
        if (!WriteFile(h, data + total, size - total, &wrote, NULL) || wrote == 0)
        {
            CloseHandle(h);
            DeleteFileA(path);
            return 0;
        }
        total += wrote;
    }
    if (!CloseHandle(h))
    {
        DeleteFileA(path);
        return 0;
    }
    return 1;
}

/* 把内嵌资源写到临时文件, 让引擎读入面表, 然后删除临时文件。 */
static int PcxLoadTemp(void* pcx, int idx, const char* name, PcxEntry* e)
{
    HMODULE mod;
    HRSRC res;
    HGLOBAL hg;
    const unsigned char* data;
    DWORD size;
    mod = PcxThisModule();
    if (!mod)
    {
        PcxWarnAt(idx, name, "no module");
        return 0;
    }
    res = FindResourceA(mod, MAKEINTRESOURCEA(s_Defs[idx].res), RT_RCDATA);
    if (!res)
    {
        PcxWarnAt(idx, name, "missing resource");
        return 0;
    }
    hg = LoadResource(mod, res);
    data = hg ? (const unsigned char*)LockResource(hg) : NULL;
    size = SizeofResource(mod, res);
    if (!data || !size)
    {
        PcxWarnAt(idx, name, "bad resource");
        return 0;
    }
    if (!PcxWriteTemp(data, size, e->path))
    {
        PcxWarnAt(idx, name, "temp write failed");
        return 0;
    }
    if (!FN_PCXLOAD(pcx, e->path, 1, 0))
    {
        DeleteFileA(e->path);
        PcxWarnAt(idx, name, "load failed");
        return 0;
    }
    DeleteFileA(e->path);
    return 1;
}

static void* PcxGetAt(int idx, const char* name)
{
    void* pcx = (void*)ADDR_PCX_INSTANCE;
    void* surf;
    PcxEntry* e;
    if (idx < 0 || !name || !name[0])
    {
        PcxWarnAt(idx, name, "unknown file");
        return NULL;
    }
    e = &s_Ent[idx];
    if (e->failed) return NULL;
    if (!IsPlausiblePtr(pcx))
    {
        e->failed = 1;
        PcxWarnAt(idx, name, "no pcx instance");
        return NULL;
    }
    if (!e->path[0] && !PcxTempPath(name, e->path, (int)sizeof(e->path)))
    {
        e->failed = 1;
        PcxWarnAt(idx, name, "temp path failed");
        return NULL;
    }
    /* 已加载过才查面表; 首次必定 miss, 直接写盘加载, 省一次无谓查找。 */
    if (e->loaded)
    {
        surf = FN_PCXGET(pcx, e->path, NULL);
        if (surf && IsPlausiblePtr(surf)) return surf;
    }
    if (!PcxLoadTemp(pcx, idx, name, e))
    {
        e->failed = 1;
        return NULL;
    }
    e->loaded = 1;
    surf = FN_PCXGET(pcx, e->path, NULL);
    if (!surf || !IsPlausiblePtr(surf))
    {
        e->failed = 1;
        PcxWarnAt(idx, name, "no surface");
        return NULL;
    }
    return surf;
}

/* BSurface: +04 W / +08 H / +10 bpp / +14 Buffer / +18 Size。 */
static int PcxInfo(void* surf, int* w, int* h, int* bpp,
                   const unsigned char** buf)
{
    int ww, hh, bb, sz;
    unsigned char* p;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    if (!IsPlausiblePtr(*(void**)surf)) return 0;
    ww = *(int*)((char*)surf + 0x04);
    hh = *(int*)((char*)surf + 0x08);
    bb = *(int*)((char*)surf + 0x10);
    p  = *(unsigned char**)((char*)surf + 0x14);
    sz = *(int*)((char*)surf + 0x18);
    if (ww < 1 || ww > 2048 || hh < 1 || hh > 2048) return 0;
    if (bb != 1 && bb != 2) return 0;
    if (!p || !IsPlausiblePtr(p)) return 0;
    if (sz != ww * hh * bb) return 0;
    *w = ww; *h = hh; *bpp = bb; *buf = p;
    return 1;
}

int PcxImage_Load(PcxImage* img, const char* name, int wantBpp)
{
    void* s;
    int w, h, bpp, idx;
    const unsigned char* p;
    unsigned short key;
    if (!img) return 0;
    img->surf = NULL;
    img->px = NULL;
    img->width = img->height = img->bpp = 0;
    img->key = 0;
    idx = PcxIndexOf(name);
    s = PcxGetAt(idx, name);
    if (!PcxInfo(s, &w, &h, &bpp, &p))
    {
        PcxWarnAt(idx, name, "bad surface");
        return 0;
    }
    if (wantBpp && bpp != wantBpp)
    {
        PcxWarnAt(idx, name, "wrong bpp");
        return 0;
    }
    if (bpp == 2) key = ((const unsigned short*)p)[0];
    else key = 0;
    img->surf = s;
    img->px = p;
    img->width = w;
    img->height = h;
    img->bpp = bpp;
    img->key = key;
    PcxLogOkAt(idx, name, w, h, bpp, key);
    return 1;
}

int PcxImage_Draw(const PcxImage* img, void* target, int x, int y)
{
    int tw, th, tbpp;
    RectangleStruct r;
    if (!img || !img->surf || img->bpp != 2) return -1;
    if (img->width < 1 || img->height < 1) return -1;
    if (!target || !IsPlausiblePtr(target)) return -1;
    if (!IsPlausiblePtr(*(void**)target)) return -1;
    tw = *(int*)((char*)target + 0x04);
    th = *(int*)((char*)target + 0x08);
    tbpp = *(int*)((char*)target + 0x10);
    if (tw < 1 || th < 1 || tbpp != 2) return -1;
    if (img->width > tw || img->height > th) return -1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > tw - img->width) x = tw - img->width;
    if (y > th - img->height) y = th - img->height;
    r.X = x;
    r.Y = y;
    r.Width = img->width;
    r.Height = img->height;
    if (!FN_PCXBLIT((void*)ADDR_PCX_INSTANCE, &r, target, img->surf, img->key))
        return -1;
    return 0;
}

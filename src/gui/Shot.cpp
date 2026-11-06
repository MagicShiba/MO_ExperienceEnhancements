/* Shot.cpp - 视口原图截图 (底栏相机按钮), 见 Shot.h。
 *
 * 取图点是消息帧钩里 SB_RenderSwapOut 之后、任何 UI 与缩放呈现之前, 此刻
 * 三种档位的原图都齐了:
 *   1x     = 窗口 Composite 的 R (== A)
 *   放大档 = 窗口 Composite 的 R (就地放大之前那块, 分辨率 < 视图区)
 *   缩小档 = 大面 s_LastFinal 的整面 R (分辨率 > 视图区)
 * 标签是 1/scale, 标 0.5x 时截图宽高正好是屏上的两倍。
 *
 * 落盘是异步的: 取图只把像素拷进自己的缓冲, 编码 (PNG 的 LZ77) 交给一个线程,
 * 下一帧回来收结果写日志。屏幕上冻 1.5 秒的截图没人受得了, 而 0.5x 档的
 * 6120x3735 原图编一次要 1.6 秒。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "ScaleBar.h"
#include "PauseView.h"
#include "ShotPng.h"
#include "Shot.h"

#define SHOT_SECTION "BottomBar"
#define SHOT_FLASH 12                 /* 截成功后描黄边的帧数 */
#define SHOT_JOBS 4                   /* 同时在编的截图上限 (连点也不会卡) */

/* ---------------- 配置 / 状态 ---------------- */
static int s_Init = 0;
static int s_Show = 0;                 /* [BottomBar] ShotEnable */
static int s_BaseOk = 0;
static char s_Base[MAX_PATH_LEN];      /* 输出目录全路径 */

static int s_Hov = 0;
static int s_LbPrev = 0;
static int s_Req = 0;                  /* 排队中的一次截图 */
static int s_Flash = 0;                /* 成功反馈倒计时 */

/* 一次异步编码。state: 0 空闲 / 1 线程在跑 / 2 已出结果待收。 */
typedef struct {
    unsigned short* pix;               /* 线程负责释放 */
    int w, h, gbits;
    char path[MAX_PATH_LEN];
    volatile LONG state;
    int ok;
} ShotJob;

static ShotJob s_Job[SHOT_JOBS];


/* ---------------- 路径 ---------------- */

/* 输出目录 = <游戏目录>\Screenshots —— 就是游戏自带截图那个文件夹, 不开放配置。 */
static void Shot_BuildTarget(void)
{
    const char* dir;
    unsigned p;
    if (s_BaseOk) return;
    s_BaseOk = 1;
    dir = GtGameDir();
    if (!dir[0]) return;
    p = AppendCStr(s_Base, 0, dir, MAX_PATH_LEN);
    AppendCStr(s_Base, p, "\\Screenshots", MAX_PATH_LEN);
    CreateDirectoryA(s_Base, NULL);   /* 已存在返回 ERROR_ALREADY_EXISTS, 忽略 */
}

/* 定宽数字 (无 CRT, 自己补零)。返回写入位数。 */
static unsigned Shot_PutNum(char* p, unsigned v, int digits)
{
    int i;
    for (i = digits - 1; i >= 0; i--)
    {
        p[i] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    return (unsigned)digits;
}

/* SCRN.<yyyyMMdd-HHmmss-mmm>.PNG, 与 Ares 同名口径。out 需 MAX_PATH_LEN。 */
static void Shot_MakeName(char* out)
{
    SYSTEMTIME st;
    unsigned p = 0;
    Shot_BuildTarget();
    if (s_Base[0])
    {
        p = AppendCStr(out, p, s_Base, MAX_PATH_LEN);
        p = AppendCh(out, p, '\\', MAX_PATH_LEN);
    }
    p = AppendCStr(out, p, "SCRN.", MAX_PATH_LEN);
    GetLocalTime(&st);
    p += Shot_PutNum(out + p, (unsigned)st.wYear, 4);
    p += Shot_PutNum(out + p, (unsigned)st.wMonth, 2);
    p += Shot_PutNum(out + p, (unsigned)st.wDay, 2);
    out[p++] = '-';
    p += Shot_PutNum(out + p, (unsigned)st.wHour, 2);
    p += Shot_PutNum(out + p, (unsigned)st.wMinute, 2);
    p += Shot_PutNum(out + p, (unsigned)st.wSecond, 2);
    out[p++] = '-';
    p += Shot_PutNum(out + p, (unsigned)st.wMilliseconds, 3);
    AppendCStr(out, p, ".PNG", MAX_PATH_LEN);
}

/* ---------------- 16 位 BITFIELDS BMP ---------------- */

#pragma pack(push, 1)
typedef struct {
    unsigned char magic[2];
    unsigned int filesz;
    unsigned short creator1;
    unsigned short creator2;
    unsigned int off;
    unsigned int hdrSize;
    int width;
    int height;              /* 负 = 自上而下 */
    unsigned short planes;
    unsigned short bits;
    unsigned int compress;   /* BI_BITFIELDS = 3 */
    unsigned int imgSize;
    int hres;
    int vres;
    unsigned int nColors;
    unsigned int nImpColors;
    unsigned int maskR;
    unsigned int maskG;
    unsigned int maskB;
} ShotBmpHdr;               /* 66 字节, 与 Ares 的 bmpfile_full_header 同构 */
#pragma pack(pop)

int Shot_WriteBmp565(const char* path, const unsigned short* pix, int w, int h)
{
    ShotBmpHdr hd;
    HANDLE f;
    DWORD put = 0;
    unsigned sz = (unsigned)((size_t)w * (size_t)h * 2u);

    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;

    hd.magic[0] = 'B'; hd.magic[1] = 'M';
    hd.creator1 = 0; hd.creator2 = 0;
    hd.hdrSize = 40u;
    hd.width = w;
    hd.height = -h;
    hd.planes = 1;
    hd.bits = 16;
    hd.compress = 3u;
    hd.imgSize = sz;
    hd.hres = 4000;
    hd.vres = 4000;
    hd.nColors = 0; hd.nImpColors = 0;
    /* 掩码跟引擎实际像素布局走: g_GBits==6 ⇒ 565, 否则 555 (Ares 恒写 565)。 */
    if (g_GBits == 6) { hd.maskR = 0xF800u; hd.maskG = 0x07E0u; hd.maskB = 0x001Fu; }
    else              { hd.maskR = 0x7C00u; hd.maskG = 0x03E0u; hd.maskB = 0x001Fu; }
    hd.off = (unsigned)sizeof(ShotBmpHdr);
    hd.filesz = hd.off + sz;

    if (!WriteFile(f, &hd, (DWORD)sizeof(hd), &put, NULL) ||
        put != (DWORD)sizeof(hd) ||
        !WriteFile(f, pix, sz, &put, NULL) || put != sz)
    {
        CloseHandle(f);
        DeleteFileA(path);
        return 0;
    }
    CloseHandle(f);
    return 1;
}

/* ---------------- 异步编码 ---------------- */

/* 线程里只做"像素 → 文件"; 日志与黄边反馈留给主线程收尾, 免得和引擎的日志
 * 抢同一个文件指针。 */
static DWORD WINAPI Shot_Worker(void* arg)
{
    ShotJob* j = (ShotJob*)arg;
    int ok = ShotPng_Write565(j->path, j->pix, j->w, j->h, j->gbits);
    VirtualFree(j->pix, 0, MEM_RELEASE);
    j->pix = NULL;
    j->ok = ok;
    InterlockedExchange(&j->state, 2);
    return 0;
}

/* 收回已出结果的槽: 写日志, 释放槽。每帧调一次。 */
static void Shot_ReapJobs(void)
{
    char buf[MAX_PATH_LEN + 48], b1[16], b2[16];
    unsigned p;
    int i;
    for (i = 0; i < SHOT_JOBS; i++)
    {
        ShotJob* j = &s_Job[i];
        if (j->state != 2) continue;
        p = AppendCStr(buf, 0, j->ok ? "shot: " : "shot: write failed ",
                       sizeof(buf));
        p = AppendCStr(buf, p, j->path, sizeof(buf));
        if (j->ok)
        {
            p = AppendCh(buf, p, ' ', sizeof(buf));
            UtoA((unsigned)j->w, b1);
            p = AppendCStr(buf, p, b1, sizeof(buf));
            p = AppendCh(buf, p, 'x', sizeof(buf));
            UtoA((unsigned)j->h, b2);
            p = AppendCStr(buf, p, b2, sizeof(buf));
        }
        LogLine(buf);
        j->state = 0;
    }
}

static ShotJob* Shot_AllocJob(void)
{
    int i;
    for (i = 0; i < SHOT_JOBS; i++)
        if (s_Job[i].state == 0) return &s_Job[i];
    return NULL;
}

/* ---------------- 取图 ---------------- */

static int Shot_Capture(void)
{
    void* surf;
    int vx, vy, vw, vh, pitch, i, j;
    unsigned char* base;
    unsigned short* pix;
    ShotJob* job;
    HANDLE th;

    if (!SB_RawView(&surf, &vx, &vy, &vw, &vh)) return 0;
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (pitch < GtSurfGet(surf, GtVT_GET_WIDTH) * 2) return 0;
    job = Shot_AllocJob();
    if (!job) return 0;            /* 四个都在编: 丢掉这次点击, 不排队积压 */
    pix = (unsigned short*)VirtualAlloc(NULL, (size_t)vw * (size_t)vh * 2u,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pix) return 0;
    /* 读锁只包住整块拷出, 编码与写盘都在解锁之后。 */
    base = GtLock(surf);
    if (!base) { VirtualFree(pix, 0, MEM_RELEASE); return 0; }
    for (j = 0; j < vh; j++)
    {
        const unsigned short* src = (const unsigned short*)
            (base + (size_t)(vy + j) * pitch + (size_t)vx * 2);
        unsigned short* dst = pix + (size_t)j * vw;
        for (i = 0; i < vw; i++) dst[i] = src[i];
    }
    GtUnlock(surf);

    Shot_MakeName(job->path);
    job->pix = pix;
    job->w = vw;
    job->h = vh;
    job->gbits = g_GBits;
    job->ok = 0;
    job->state = 1;
    th = CreateThread(NULL, 0, Shot_Worker, job, 0, NULL);
    if (th) CloseHandle(th);
    else Shot_Worker(job);         /* 起不了线程就当场编, 至少图不丢 */
    s_Flash = SHOT_FLASH;          /* 反馈给点击, 不等结果 */
    return 1;
}

/* ---------------- 初始化 ---------------- */

void Shot_EnsureInit(void)
{
    char buf[128];
    unsigned p;
    if (s_Init) return;
    s_Init = 1;
    BuildPaths();
    GtEnsureLog();
    s_Show = GetPrivateProfileIntA(SHOT_SECTION, "ShotEnable", 0, g_IniPath) ? 1 : 0;
    p = AppendCStr(buf, 0, "shot: btn=", sizeof(buf));
    p += UtoA((unsigned)s_Show, buf + p);
    p = AppendCStr(buf, p, " dir=Screenshots", sizeof(buf));
    LogLine(buf);
    /* 黄边反馈之外没有别的进度提示, 编码器在这里预热 (GDI+ 加载 ~1.5ms),
     * 免得第一次截图在工作线程里现装。 */
    if (s_Show)
        LogLine(ShotPng_Init() ? "shot: png=gdiplus"
                               : "shot: png unavailable (no gdiplus)");
}

int Shot_Visible(void)
{
    Shot_EnsureInit();
    return s_Show;
}

void Shot_SetVisible(int enable)
{
    Shot_EnsureInit();
    s_Show = enable ? 1 : 0;
    s_Hov = 0;
    s_LbPrev = 0;
    s_Req = 0;
    s_Flash = 0;
    /* ESC 菜单打开的功能不走 EnsureInit 的预热分支, 在这里补上, 免得第一次
     * 截图才在编码线程里装 GDI+ (那段有 "只装一次" 的静态标志, 并发不划算)。 */
    if (s_Show) ShotPng_Init();
}

int Shot_ButtonWidth(void)
{
    Shot_EnsureInit();
    return s_Show ? PV_BS : 0;
}

/* ---------------- 输入 ---------------- */

void Shot_Poll(void)
{
    int mx, my, x = 0, y = 0, down, edge;
    Shot_EnsureInit();
    if (s_Flash > 0) s_Flash--;
    s_Hov = 0;
    if (!s_Show || !GtMouseGameXY(&mx, &my) || !PV_SlotGeom(3, &x, &y))
    {
        s_LbPrev = 0;
        return;
    }
    s_Hov = (mx >= x && mx < x + PV_BS && my >= y && my < y + PV_BS) ? 1 : 0;
    down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    edge = (down && !s_LbPrev) ? 1 : 0;
    s_LbPrev = down;
    /* 只在 prepare / 侧栏呈现里跑: 大厅与载入屏无战术渲染 ⇒ 无按钮也无点击。 */
    if (edge && s_Hov) s_Req = 1;
}

void Shot_CapturePending(void)
{
    Shot_ReapJobs();
    if (!s_Req) return;
    s_Req = 0;
    if (!Shot_Capture())
        LogLine("shot: no raw view this frame");
}

/* ---------------- 绘制 ---------------- */

static void Shot_Fill(unsigned char* base, int pitch, int W, int H,
                      int x, int y, int w, int h,
                      unsigned char r, unsigned char g, unsigned char b)
{
    unsigned short px = Blend16(0, r, g, b, 255);
    int i, j;
    unsigned short* row;
    if (x < 0 || y < 0 || x + w > W || y + h > H) return;
    for (j = 0; j < h; j++)
    {
        row = (unsigned short*)(base + (size_t)(y + j) * pitch) + x;
        for (i = 0; i < w; i++) row[i] = px;
    }
}

static void Shot_PaintBtn(unsigned char* base, int pitch, int W, int H,
                          int x, int y)
{
    /* 边色: 截成功一闪黄 / 悬停亮白 / 常态灰。取景凸起 + 机身 + 镜头。 */
    unsigned char er = 140, eg = 146, eb = 156;
    int i, j;
    if (s_Flash > 0) { er = 250; eg = 214; eb = 80; }
    else if (s_Hov) { er = 232; eg = 236; eb = 242; }
    Shot_Fill(base, pitch, W, H, x, y, PV_BS, PV_BS, 20, 22, 28);
    Shot_Fill(base, pitch, W, H, x, y, PV_BS, 1, er, eg, eb);
    Shot_Fill(base, pitch, W, H, x, y + PV_BS - 1, PV_BS, 1, er, eg, eb);
    Shot_Fill(base, pitch, W, H, x, y, 1, PV_BS, er, eg, eb);
    Shot_Fill(base, pitch, W, H, x + PV_BS - 1, y, 1, PV_BS, er, eg, eb);
    Shot_Fill(base, pitch, W, H, x + 8, y + 6, 5, 2, 232, 236, 242);
    Shot_Fill(base, pitch, W, H, x + 5, y + 8, 12, 8, 232, 236, 242);
    for (j = -2; j <= 2; j++)
        for (i = -2; i <= 2; i++)
            if (i * i + j * j <= 4)
                Shot_Fill(base, pitch, W, H, x + 11 + i, y + 12 + j, 1, 1,
                          30, 32, 40);
    Shot_Fill(base, pitch, W, H, x + 10, y + 11, 1, 1, 232, 236, 242);
}

int Shot_DrawToComposite(void)
{
    void* surf;
    unsigned char* base;
    int pitch, w, h, x = 0, y = 0;
    Shot_EnsureInit();
    if (!s_Show) return 0;
    if (!PV_SlotGeom(3, &x, &y)) return 0;
    surf = *(void**)ADDR_SURF_COMPOSITE;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    w = GtSurfGet(surf, GtVT_GET_WIDTH);
    h = GtSurfGet(surf, GtVT_GET_HEIGHT);
    if (pitch < w * 2 || w <= 0 || h <= 0) return 0;
    base = GtLock(surf);
    if (!base) return 0;
    Shot_PaintBtn(base, pitch, w, h, x, y);
    GtUnlock(surf);
    return 1;
}

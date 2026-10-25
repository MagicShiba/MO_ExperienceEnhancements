/* Shot.cpp - 视口原图截图 (底栏相机按钮), 见 Shot.h。
 *
 * 取图点是消息帧钩里 SB_RenderSwapOut 之后、任何 UI 与缩放呈现之前, 此刻
 * 三种档位的原图都齐了:
 *   1x     = 窗口 Composite 的 R (== A)
 *   放大档 = 窗口 Composite 的 R (就地放大之前那块, 分辨率 < 视图区)
 *   缩小档 = 大面 s_LastFinal 的整面 R (分辨率 > 视图区)
 * 标签是 1/scale, 标 0.5x 时截图宽高正好是屏上的两倍。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "ScaleBar.h"
#include "PauseView.h"
#include "Shot.h"

#define SHOT_SECTION "BottomBar"
#define SHOT_FLASH 12                 /* 截成功后描黄边的帧数 */

/* ---------------- 配置 / 状态 ---------------- */
static int s_Init = 0;
static int s_Show = 0;                 /* [BottomBar] ShotEnable */
static int s_BaseOk = 0;
static char s_Dir[MAX_PATH_LEN];       /* [BottomBar] ShotDir */
static char s_Base[MAX_PATH_LEN];      /* 输出目录全路径 */

static int s_Hov = 0;
static int s_LbPrev = 0;
static int s_Req = 0;                  /* 排队中的一次截图 */
static int s_Flash = 0;                /* 成功反馈倒计时 */

/* ---------------- 路径 ---------------- */

/* 去掉 INI 值里手写的 ';' 注释与尾空格。 */
static void Shot_TrimIniValue(char* s)
{
    int i;
    for (i = 0; s[i]; i++)
        if (s[i] == ';') { s[i] = 0; break; }
    while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t')) s[--i] = 0;
}

/* 输出目录 = 游戏目录 \ ShotDir; 空 = 游戏目录本身, 含盘符或前导 '\' = 绝对路径。 */
static void Shot_BuildTarget(void)
{
    char exe[MAX_PATH_LEN];
    DWORD n, i;
    unsigned p;
    if (s_BaseOk) return;
    s_BaseOk = 1;
    s_Base[0] = 0;
    n = GetModuleFileNameA(NULL, exe, MAX_PATH_LEN);
    if (n == 0 || n >= MAX_PATH_LEN) return;
    i = n;
    while (i > 0 && exe[i] != '\\' && exe[i] != '/') i--;
    exe[i] = 0;
    if (s_Dir[0] == 0)
        AppendCStr(s_Base, 0, exe, MAX_PATH_LEN);
    else if (s_Dir[0] == '\\' || s_Dir[1] == ':')
        AppendCStr(s_Base, 0, s_Dir, MAX_PATH_LEN);
    else
    {
        p = AppendCStr(s_Base, 0, exe, MAX_PATH_LEN);
        p = AppendCStr(s_Base, p, "\\", MAX_PATH_LEN);
        AppendCStr(s_Base, p, s_Dir, MAX_PATH_LEN);
    }
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

/* SCRN.<yyyyMMdd-HHmmss-mmm>.BMP, 与 Ares 同名口径。out 需 MAX_PATH_LEN。 */
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
    AppendCStr(out, p, ".BMP", MAX_PATH_LEN);
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

static int Shot_Save(const char* path, const unsigned short* pix, int w, int h)
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

/* ---------------- 取图 ---------------- */

static int Shot_Capture(void)
{
    void* surf;
    int vx, vy, vw, vh, pitch, i, j;
    unsigned char* base;
    unsigned short* pix;
    char path[MAX_PATH_LEN];
    char b1[16], b2[16], buf[MAX_PATH_LEN + 40];
    unsigned p;

    if (!SB_RawView(&surf, &vx, &vy, &vw, &vh)) return 0;
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (pitch < GtSurfGet(surf, GtVT_GET_WIDTH) * 2) return 0;
    pix = (unsigned short*)VirtualAlloc(NULL, (size_t)vw * (size_t)vh * 2u,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pix) return 0;
    /* 读锁只包住整块拷出, 写盘放到解锁之后。 */
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

    Shot_MakeName(path);
    if (!Shot_Save(path, pix, vw, vh))
    {
        VirtualFree(pix, 0, MEM_RELEASE);
        LogLine("shot: write failed");
        return 0;
    }
    VirtualFree(pix, 0, MEM_RELEASE);

    p = AppendCStr(buf, 0, "shot: ", sizeof(buf));
    p = AppendCStr(buf, p, path, sizeof(buf));
    p = AppendCh(buf, p, ' ', sizeof(buf));
    UtoA((unsigned)vw, b1);
    p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, 'x', sizeof(buf));
    UtoA((unsigned)vh, b2);
    p = AppendCStr(buf, p, b2, sizeof(buf));
    LogLine(buf);
    s_Flash = SHOT_FLASH;
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
    GetPrivateProfileStringA(SHOT_SECTION, "ShotDir", "Screenshots",
                             s_Dir, (DWORD)sizeof(s_Dir), g_IniPath);
    Shot_TrimIniValue(s_Dir);
    p = AppendCStr(buf, 0, "shot: btn=", sizeof(buf));
    p += UtoA((unsigned)s_Show, buf + p);
    p = AppendCStr(buf, p, " dir=", sizeof(buf));
    AppendCStr(buf, p, s_Dir[0] ? s_Dir : "(game)", sizeof(buf));
    LogLine(buf);
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
    /* 未开局 (大厅/载入屏) 不截: 那时渲染面里没有地图。 */
    if (edge && s_Hov && *(volatile unsigned char*)(size_t)ADDR_SCEN_STARTED)
        s_Req = 1;
}

void Shot_CapturePending(void)
{
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

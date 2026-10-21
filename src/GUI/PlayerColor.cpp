/* PlayerColor.cpp - local player color switching for the ESC settings page.
 * Only the current player's HouseClass::ColorSchemeIndex and the selected
 * scheme's LightConvert palette are changed.  No simulation state is touched. */

#include <windows.h>
#include "PlayerColor.h"
#include "GdiText.h"
#include "OrderUtil.h"

#define PC_SCHEME_ITEMS       0x00B054D4u
#define PC_SCHEME_COUNT       0x00B054E0u
#define PC_HOUSE_CURRENT      0x00A83D4Cu
#define PC_HOUSE_ITEMS        0x00A8022Cu
#define PC_HOUSE_COUNT        0x00A80238u
#define PC_HOUSE_COLOR_SCHEME 0x16054u
#define PC_FN_HOUSE_COLOR     0x0050B840u
#define PC_FN_HOUSE_LASER     0x0050BA00u
#define PC_FN_UPDATE_COLORS   0x00556090u
#define PC_FN_DIRTY_AREA      0x006D2790u
#define PC_TACTICAL_INSTANCE  0x00887324u

#define PC_SCHEME_ID          0x304u
#define PC_SCHEME_LIGHT       0x30Cu
#define PC_SCHEME_SHADES      0x310u
#define PC_SCHEME_MAIN_SHADE  0x330u
#define PC_SCHEME_BASE        0x308u

#define PC_LC_BPP             0x004u
#define PC_LC_SHADE_COUNT     0x16Cu
#define PC_LC_FULL_COLOR      0x170u
#define PC_LC_PALETTE         0x174u
#define PC_LC_USED_PALETTE1   0x188u
#define PC_LC_COLOR1          0x198u
#define PC_LC_COLOR2          0x1A4u
#define PC_LC_TINTED          0x1B0u

#define PC_SHADE_COUNT        53
#define PC_RAINBOW_MAX_PAL    0x8000
#define PC_RAINBOW_HUE_MAX    1536
/* 色相每 3 帧推进一次；步长按 3 帧合并，一圈仍是约 12.8 秒。 */
#define PC_RAINBOW_FRAME_SKIP 3
#define PC_RAINBOW_STEP       6
/* 每次刷新的战术视图脏区 = 整视图的 1/5，相邻区域多带 64 像素重叠。 */
#define PC_RAINBOW_DIRTY_PARTS 5

typedef void (__fastcall *PcFnHouseColor)(void*);
typedef void (__attribute__((thiscall)) *PcFnUpdateColors)(void*, int, int, int, int);
typedef void (__attribute__((thiscall)) *PcFnDirtyArea)(void*, RectangleStruct, int);

static int s_Index[PLAYER_COLOR_MAX];
static int s_NormalCount = 0;
static int s_ListTried = 0;

static int s_Rainbow = 0;
static void* s_RainbowHouse = NULL;
static void* s_RainbowLight = NULL;
static unsigned char* s_RainbowScheme = NULL;
static int s_RainbowOrigSize = 0;
static unsigned char s_RainbowOrigBase[3];
static unsigned char s_RainbowOrig[PC_RAINBOW_MAX_PAL];
static unsigned char* s_RainbowColors = NULL;
static unsigned char* s_RainbowData = NULL;
static unsigned char s_RainbowOrigColors[16 * 3];
static unsigned char s_RainbowValues[16];
static int s_RainbowBpp = 0;
static int s_RainbowShades = 0;
static int s_RainbowOrigColor1[3];
static int s_RainbowOrigColor2[3];
static unsigned char s_RainbowOrigTinted = 0;
static int s_Hue = 0;
static int s_Frame = 0;
static int s_Band = 0;

static void Pc_BuildList(void)
{
    unsigned char** items;
    int count, i, n = 0;

    if (s_ListTried) return;
    items = *(unsigned char***)(size_t)PC_SCHEME_ITEMS;
    count = *(volatile int*)(size_t)PC_SCHEME_COUNT;
    if (!items || count <= 0) return;

    s_ListTried = 1;
    for (i = 0; i < count && n < PLAYER_COLOR_MAX - 1; i++)
    {
        unsigned char* scheme = items[i];
        char* id;
        int j, dup = 0;

        if (!scheme) continue;
        if (*(int*)(scheme + PC_SCHEME_SHADES) != PC_SHADE_COUNT) continue;
        id = *(char**)(scheme + PC_SCHEME_ID);
        if (!id || !id[0]) continue;

        for (j = 0; j < n; j++)
        {
            unsigned char* prev = items[s_Index[j]];
            char* pid = prev ? *(char**)(prev + PC_SCHEME_ID) : NULL;
            if (pid && lstrcmpiA(pid, id) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        s_Index[n++] = i;
    }
    s_NormalCount = n;
    if (n == 0) s_ListTried = 0;
}

static void* Pc_CurrentHouse(void)
{
    return *(void* const volatile*)(size_t)PC_HOUSE_CURRENT;
}

static int Pc_HouseAlive(void* house)
{
    void** items;
    int count, i;

    if (!house || !IsPlausiblePtr(house)) return 0;
    items = *(void***)(size_t)PC_HOUSE_ITEMS;
    count = *(volatile int*)(size_t)PC_HOUSE_COUNT;
    if (!items || !IsPlausiblePtr(items) || count <= 0 || count > 64) return 0;
    for (i = 0; i < count; i++)
        if (items[i] == house) return 1;
    return 0;
}

static unsigned short Pc_Rgb565(unsigned r, unsigned g, unsigned b)
{
    return Blend16(0, r, g, b, 255);
}

static void Pc_HueToRgbV(int hue, int value, int* r, int* g, int* b)
{
    int sector = hue >> 8;
    int f = hue & 255;
    int q = value * (255 - f) / 255;
    int t = value * f / 255;

    switch (sector)
    {
    case 0: *r = value; *g = t;     *b = 0;     break;
    case 1: *r = q;     *g = value; *b = 0;     break;
    case 2: *r = 0;     *g = value; *b = t;     break;
    case 3: *r = 0;     *g = q;     *b = value; break;
    case 4: *r = t;     *g = 0;     *b = value; break;
    default:*r = value; *g = 0;     *b = q;     break;
    }
}

static int Pc_SaveLight(void* light)
{
    int bpp = *(int*)((char*)light + PC_LC_BPP);
    int shades = *(int*)((char*)light + PC_LC_SHADE_COUNT);
    unsigned char* data = *(unsigned char**)((char*)light + PC_LC_FULL_COLOR);
    unsigned char* colors = *(unsigned char**)((char*)light + PC_LC_USED_PALETTE1);
    int size, i;

    if (bpp != 1 && bpp != 2) return 0;
    if (shades <= 0 || shades > 256) return 0;
    size = shades * 256 * bpp;
    if (!data || size > (int)sizeof(s_RainbowOrig)) return 0;
    if (!colors || !IsPlausiblePtr(colors)) return 0;

    memcpy(s_RainbowOrig, data, size);
    memcpy(s_RainbowOrigColors, colors + 16 * 3, sizeof(s_RainbowOrigColors));
    for (i = 0; i < 16; i++)
    {
        int r0 = s_RainbowOrigColors[i * 3 + 0];
        int g0 = s_RainbowOrigColors[i * 3 + 1];
        int b0 = s_RainbowOrigColors[i * 3 + 2];
        s_RainbowValues[i] = (unsigned char)(r0 > g0 ? (r0 > b0 ? r0 : b0)
                                                     : (g0 > b0 ? g0 : b0));
    }
    s_RainbowColors = colors;
    s_RainbowData = data;
    s_RainbowOrigSize = size;
    s_RainbowBpp = bpp;
    s_RainbowShades = shades;
    for (i = 0; i < 3; i++)
    {
        s_RainbowOrigColor1[i] = *(int*)((char*)light + PC_LC_COLOR1 + i * 4);
        s_RainbowOrigColor2[i] = *(int*)((char*)light + PC_LC_COLOR2 + i * 4);
    }
    s_RainbowOrigTinted = *(unsigned char*)((char*)light + PC_LC_TINTED);
    return 1;
}

static void Pc_RestoreLight(void)
{
    void* light = s_RainbowLight;
    int i;

    if (!light) return;
    memcpy(s_RainbowColors + 16 * 3, s_RainbowOrigColors, sizeof(s_RainbowOrigColors));
    memcpy(s_RainbowData, s_RainbowOrig, s_RainbowOrigSize);
    s_RainbowScheme[PC_SCHEME_BASE + 0] = s_RainbowOrigBase[0];
    s_RainbowScheme[PC_SCHEME_BASE + 1] = s_RainbowOrigBase[1];
    s_RainbowScheme[PC_SCHEME_BASE + 2] = s_RainbowOrigBase[2];
    for (i = 0; i < 3; i++)
    {
        *(int*)((char*)light + PC_LC_COLOR1 + i * 4) = s_RainbowOrigColor1[i];
        *(int*)((char*)light + PC_LC_COLOR2 + i * 4) = s_RainbowOrigColor2[i];
    }
    *(unsigned char*)((char*)light + PC_LC_TINTED) = s_RainbowOrigTinted;
}

static void Pc_UpdateHouseRaw(void* house)
{
    ((PcFnHouseColor)(void*)PC_FN_HOUSE_COLOR)(house);
    ((PcFnHouseColor)(void*)PC_FN_HOUSE_LASER)(house);
}

static void Pc_RefreshTacticalView(int full)
{
    void* tactical;
    RectangleStruct r;
    int w, h, part;

    tactical = *(void* const volatile*)(size_t)PC_TACTICAL_INSTANCE;
    if (!tactical || !IsPlausiblePtr(tactical)) return;

    w = *(volatile int*)(size_t)(ADDR_SURF_VIEWBOUNDS + 8);
    h = *(volatile int*)(size_t)(ADDR_SURF_VIEWBOUNDS + 12);
    if (w < 64 || w > 8192 || h < 64 || h > 8192) return;

    r.Y = -64;
    r.Height = h + 128;
    if (full)
    {
        r.X = -64;
        r.Width = w + 128;
    }
    else
    {
        part = w / PC_RAINBOW_DIRTY_PARTS;
        r.X = s_Band * part - 64;
        r.Width = part + 128;
        if (s_Band + 1 == PC_RAINBOW_DIRTY_PARTS)
            r.Width = w - s_Band * part + 128;
        if (++s_Band >= PC_RAINBOW_DIRTY_PARTS) s_Band = 0;
    }
    ((PcFnDirtyArea)(void*)PC_FN_DIRTY_AREA)(tactical, r, 1);
}

static void Pc_PreserveNonHouseColors(void)
{
    unsigned char* data = s_RainbowData;
    int bpp = s_RainbowBpp;
    int shades = s_RainbowShades;
    int s;

    for (s = 0; s < shades; s++)
    {
        unsigned char* dst = data + s * 256 * bpp;
        const unsigned char* src = s_RainbowOrig + s * 256 * bpp;
        memcpy(dst, src, 16 * bpp);
        memcpy(dst + 32 * bpp, src + 32 * bpp, (256 - 32) * bpp);
    }
}

static void Pc_ApplyRainbow(int full)
{
    int i;

    for (i = 0; i < 16; i++)
    {
        int r, g, b;

        Pc_HueToRgbV(s_Hue, s_RainbowValues[i], &r, &g, &b);
        s_RainbowColors[16 * 3 + i * 3 + 0] = (unsigned char)r;
        s_RainbowColors[16 * 3 + i * 3 + 1] = (unsigned char)g;
        s_RainbowColors[16 * 3 + i * 3 + 2] = (unsigned char)b;
    }

    {
        unsigned hue = (unsigned)s_Hue * 255u / PC_RAINBOW_HUE_MAX;

        s_RainbowScheme[PC_SCHEME_BASE + 0] = (unsigned char)hue;
        s_RainbowScheme[PC_SCHEME_BASE + 1] = s_RainbowOrigBase[1];
        s_RainbowScheme[PC_SCHEME_BASE + 2] = s_RainbowOrigBase[2];
    }

    ((PcFnUpdateColors)(void*)PC_FN_UPDATE_COLORS)(s_RainbowLight, 1000, 1000, 1000, 0);
    Pc_PreserveNonHouseColors();
    Pc_UpdateHouseRaw(s_RainbowHouse);
    Pc_RefreshTacticalView(full);
}

static void Pc_StopRainbow(void)
{
    void* house = s_RainbowHouse;

    if (!s_Rainbow) return;
    Pc_RestoreLight();
    s_Rainbow = 0;
    s_RainbowHouse = NULL;
    s_RainbowLight = NULL;
    s_RainbowScheme = NULL;
    s_RainbowColors = NULL;
    s_RainbowData = NULL;
    s_RainbowOrigSize = 0;
    s_RainbowBpp = 0;
    s_RainbowShades = 0;
    if (house && Pc_HouseAlive(house)) Pc_UpdateHouseRaw(house);
    Pc_RefreshTacticalView(1);
}

static int Pc_StartRainbow(void)
{
    unsigned char** items;
    unsigned char* scheme;
    void* house;
    void* light;
    int slot, index;

    if (OU_IsMP()) return 0;
    house = Pc_CurrentHouse();
    if (!Pc_HouseAlive(house)) return 0;

    slot = PlayerColor_Current();
    if (slot < 0 || slot >= s_NormalCount) return 0;

    items = *(unsigned char***)(size_t)PC_SCHEME_ITEMS;
    index = s_Index[slot];
    scheme = items ? items[index] : NULL;
    if (!scheme) return 0;
    light = *(void**)(scheme + PC_SCHEME_LIGHT);
    if (!light || !IsPlausiblePtr(light)) return 0;
    if (!Pc_SaveLight(light)) return 0;

    s_RainbowOrigBase[0] = scheme[PC_SCHEME_BASE + 0];
    s_RainbowOrigBase[1] = scheme[PC_SCHEME_BASE + 1];
    s_RainbowOrigBase[2] = scheme[PC_SCHEME_BASE + 2];
    s_RainbowScheme = scheme;
    s_RainbowHouse = house;
    s_RainbowLight = light;
    s_Rainbow = 1;
    s_Frame = 0;
    s_Band = 0;
    Pc_ApplyRainbow(1);
    return 1;
}

int PlayerColor_Count(void)
{
    Pc_BuildList();
    return s_NormalCount > 0 ? s_NormalCount + 1 : 0;
}

int PlayerColor_Current(void)
{
    void* house = Pc_CurrentHouse();
    int index, i;

    if (!house) return -1;
    if (s_Rainbow && house == s_RainbowHouse) return s_NormalCount;

    index = *(volatile int*)((char*)house + PC_HOUSE_COLOR_SCHEME);
    Pc_BuildList();
    for (i = 0; i < s_NormalCount; i++)
        if (s_Index[i] == index) return i;
    return -1;
}

int PlayerColor_Set(int slot)
{
    void* house;
    int index;

    Pc_BuildList();
    if (s_NormalCount <= 0) return 0;

    if (slot == s_NormalCount)
    {
        if (s_Rainbow) return 0;
        return Pc_StartRainbow();
    }

    if (slot < 0 || slot >= s_NormalCount) return 0;
    if (s_Rainbow) Pc_StopRainbow();

    house = Pc_CurrentHouse();
    if (!Pc_HouseAlive(house)) return 0;
    index = s_Index[slot];
    if (*(volatile int*)((char*)house + PC_HOUSE_COLOR_SCHEME) == index)
        return 0;

    *(volatile int*)((char*)house + PC_HOUSE_COLOR_SCHEME) = index;
    Pc_UpdateHouseRaw(house);
    Pc_RefreshTacticalView(1);
    return 1;
}

unsigned short PlayerColor_Swatch(int slot)
{
    unsigned char** items;
    unsigned char* scheme;
    int index;

    Pc_BuildList();
    if (s_NormalCount <= 0) return 0x7BDE;

    if (slot == s_NormalCount)
    {
        int r, g, b;
        Pc_HueToRgbV(s_Hue, 255, &r, &g, &b);
        return Pc_Rgb565(r, g, b);
    }

    if (slot < 0 || slot >= s_NormalCount) return 0x7BDE;
    items = *(unsigned char***)(size_t)PC_SCHEME_ITEMS;
    index = s_Index[slot];
    scheme = items[index];
    if (scheme)
    {
        unsigned char* light = *(unsigned char**)(scheme + PC_SCHEME_LIGHT);
        if (light)
        {
            int bpp = *(int*)(light + PC_LC_BPP);
            unsigned char* palette = *(unsigned char**)(light + PC_LC_PALETTE);
            int shade = *(int*)(scheme + PC_SCHEME_MAIN_SHADE);
            if (bpp == 2 && palette && shade >= 0 && shade < 256)
                return *(unsigned short*)(palette + shade * 2);
        }
        return Pc_Rgb565(scheme[PC_SCHEME_BASE], scheme[PC_SCHEME_BASE + 1],
                         scheme[PC_SCHEME_BASE + 2]);
    }
    return 0x7BDE;
}

unsigned short PlayerColor_RainbowSwatch(int index)
{
    static const int hues[PLAYER_COLOR_RAINBOW_SEGS] = { 0, 256, 512, 768, 1024, 1280 };
    int r, g, b;

    Pc_HueToRgbV(hues[index], 255, &r, &g, &b);
    return Pc_Rgb565(r, g, b);
}

void PlayerColor_Tick(void)
{
    if (!s_Rainbow) return;
    if (OU_IsMP())
    {
        Pc_StopRainbow();
        return;
    }
    if (s_RainbowHouse != Pc_CurrentHouse())
    {
        Pc_StopRainbow();
        return;
    }
    if (++s_Frame < PC_RAINBOW_FRAME_SKIP) return;
    s_Frame = 0;

    s_Hue = (s_Hue + PC_RAINBOW_STEP) % PC_RAINBOW_HUE_MAX;
    Pc_ApplyRainbow(0);
}

extern "C" __declspec(dllexport) DWORD __cdecl PlayerColor_TickHook(void* regs)
{
    (void)regs;
    PlayerColor_Tick();
    return 0;
}

/* OreLight.cpp - 矿石/宝石受环境光照影响系数。
 *
 * 引擎画覆盖物 (CellClass::DrawOverlay 0x47F6A0) 时, 明暗由 CC_Draw_Shape
 * (0x4AED70) 的 Brightness 栈参决定 (0~2000, 最终色 = 原色 × Brightness/1000)。
 * 普通覆盖物传格子的 CellClass::Intensity_Normal (+0x10E, 随 [Lighting] 环境光
 * 重算), 夜里会变暗; 而 Tiberium 类覆盖物 (矿石/宝石) 的两个绘制分支都把
 * Brightness 硬编码成 0x3E8 (1000 = 满亮度), 调色板也是启动时建好的静态全亮
 * convert ([0x87F6B8], 不参与光照重算) —— 所以矿在深夜仍然全亮, 像自发光,
 * 这是引擎从泰伯利亚之日继承的刻意设计。
 *
 * 本模块在两处 push 0x3E8 之后的钩点上, 把栈里的 Brightness 改写成
 *   1000 + (Intensity_Normal - 1000) × Factor/100
 * Factor=0 保持原版 (矿不受光照); 100 完全遵循环境光 (与普通覆盖物同亮度)。
 * 矿石调色板是静态全亮 convert, Brightness 是它唯一的光照乘子, 改它即可,
 * 不必换调色板。
 *
 * 钩点 (都在 Brightness 已入栈之后, ESI = CellClass):
 *   0x47F939 (6B, mov dl,[esi+0x11E])  斜坡分支, Brightness 在 [esp+0xC]
 *     (其后已再 push 3 层)。仅矿石分支可达。
 *   0x47FA3D (5B, mov ecx,-2)          平地分支入口 (0x47F965 jmp 进来),
 *     Brightness 刚 push 完在 [esp]。此块也被矿脉 (OVERLAY_VEINS 0x7E) 走
 *     fallthrough 进来 —— 矿脉的 Brightness 是它自己算的 Ambient (+0x10C),
 *     不能动。矿石路径在 0x47F87F 处已把 EBX 重赋成 TiberiumClass*, 矿脉路径
 *     的 EBX 才是 OverlayTypeClass*, 所以用 "EBX ∈ TiberiumClass::Array" 区分
 *     (两个数组的分配不相交, 无误判)。
 * 两处原指令都不含相对地址, Syringe 回放安全, 返回 0 = 原样执行。
 *
 * 只读模拟数据 (格子光照), 改的是渲染调用参数, 不碰模拟状态, 联机安全。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OreLight.h"
#include "EnvIni.h"

#define OL_SECTION ENV_SECTION

/* Syringe REGISTERS 偏移 (EngineAddr.h 没有的两个, 同一套布局) */
#define OL_REG_ESP  0x14u
#define OL_REG_EBX  0x18u

/* CellClass::Intensity_Normal (WORD, 0~2000, 1000 = 不变暗), 随环境光重算 */
#define OL_CELL_INTENSITY_NORMAL 0x10Eu
/* TiberiumClass::Array (YRpp ABSTRACTTYPE_ARRAY 0xB0F4E8)。
 * DynamicVectorClass 布局 (引擎 0x4E7220 清理代码证实): 0x00 vtable, 0x04 Items,
 * 0x08 Capacity, 0x0C/0x0D IsInitialized/IsAllocated, 0x10 Count, 0x14 Increment。 */
#define OL_TIB_ITEMS 0xB0F4ECu
#define OL_TIB_COUNT 0xB0F4F8u

static int s_Init, s_On = 1, s_Factor = 50;

static void OL_EnsureInit(void)
{
    if (s_Init) return;
    s_Init = 1;
    s_On = GetPrivateProfileIntA(OL_SECTION, ENV_KEY_OREENABLE, 1, g_IniPath) ? 1 : 0;
    s_Factor = GetPrivateProfileIntA(OL_SECTION, ENV_KEY_OREFACTOR, 50, g_IniPath);
    if (s_Factor < 0) s_Factor = 0;
    if (s_Factor > ORELIGHT_FACTOR_MAX) s_Factor = ORELIGHT_FACTOR_MAX;
}

/* 新 Brightness: 在 1000 与格子 Intensity_Normal 之间按 Factor/100 插值 */
static int OL_Brightness(void* regs)
{
    unsigned cell = *(unsigned*)((char*)regs + REG_OFF_ESI);
    int intensity, bright;

    if (!cell) return 1000;
    intensity = (int)*(unsigned short*)(cell + OL_CELL_INTENSITY_NORMAL);
    if (intensity > 2000) intensity = 2000;
    bright = 1000 + (intensity - 1000) * s_Factor / 100;
    if (bright < 0) bright = 0;
    if (bright > 2000) bright = 2000;
    return bright;
}

/* 0x47F939: 斜坡分支, 原指令 mov dl,[esi+0x11E] (6B), ESI=CellClass。
 * 栈: [esp+0xC] = Brightness (其后已再 push 3 层)。 */
extern "C" __declspec(dllexport) DWORD __cdecl OreLight_Slope(void* regs)
{
    unsigned esp;

    OL_EnsureInit();
    if (!s_On) return 0;
    esp = *(unsigned*)((char*)regs + OL_REG_ESP);
    if (esp) *(unsigned*)(size_t)(esp + 0xC) = (unsigned)OL_Brightness(regs);
    return 0;
}

/* 0x47FA3D: 平地分支入口, 原指令 mov ecx,-2 (5B), ESI=CellClass。
 * 栈: [esp] = Brightness。矿石路径 EBX = TiberiumClass*, 矿脉路径 EBX =
 * OverlayTypeClass* —— 按数组隶属关系区分, 只改矿石。 */
extern "C" __declspec(dllexport) DWORD __cdecl OreLight_Flat(void* regs)
{
    unsigned esp, ebx, items, count, i;

    OL_EnsureInit();
    if (!s_On) return 0;
    ebx = *(unsigned*)((char*)regs + OL_REG_EBX);
    items = *(unsigned*)(size_t)OL_TIB_ITEMS;
    count = *(unsigned*)(size_t)OL_TIB_COUNT;
    if (!ebx || !items || !count || count > 16) return 0;
    for (i = 0; i < count; i++) {
        if (*(unsigned*)(size_t)(items + i * 4) == ebx) break;
    }
    if (i >= count) return 0;   /* 不是矿石路径 (矿脉等), 原样放行 */
    esp = *(unsigned*)((char*)regs + OL_REG_ESP);
    if (esp) *(unsigned*)(size_t)esp = (unsigned)OL_Brightness(regs);
    return 0;
}

/* 菜单读写。SetFactor 先走 EnsureInit (读 ini), 显式设置的值再盖过去。 */
int OreLight_Factor(void)
{
    OL_EnsureInit();
    return s_Factor;
}

void OreLight_SetFactor(int factor)
{
    OL_EnsureInit();
    if (factor < 0) factor = 0;
    if (factor > ORELIGHT_FACTOR_MAX) factor = ORELIGHT_FACTOR_MAX;
    s_Factor = factor;
}

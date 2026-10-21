/* PlayerColor.cpp - local player color switching for the ESC settings page.
 * Only the current player's HouseClass::ColorSchemeIndex is changed. */

#include <windows.h>
#include "PlayerColor.h"

#define PC_SCHEME_ITEMS       0x00B054D4u
#define PC_SCHEME_COUNT       0x00B054E0u
#define PC_HOUSE_CURRENT      0x00A83D4Cu
#define PC_HOUSE_COLOR_SCHEME 0x16054u
#define PC_FN_HOUSE_COLOR     0x0050B840u
#define PC_FN_HOUSE_LASER     0x0050BA00u

#define PC_SHADE_COUNT        53

typedef void (__fastcall *PcFnHouseColor)(void*);

static int s_Index[PLAYER_COLOR_MAX];
static int s_Count = 0;
static int s_ListTried = 0;

static void Pc_BuildList(void)
{
    unsigned char** items;
    int count, i, n = 0;

    if (s_ListTried) return;
    items = *(unsigned char***)(size_t)PC_SCHEME_ITEMS;
    count = *(volatile int*)(size_t)PC_SCHEME_COUNT;
    if (!items || count <= 0) return;

    s_ListTried = 1;
    for (i = 0; i < count && n < PLAYER_COLOR_MAX; i++)
    {
        unsigned char* scheme = items[i];
        char* id;
        int j, dup = 0;

        if (!scheme) continue;
        if (*(int*)(scheme + 0x310) != PC_SHADE_COUNT) continue;
        id = *(char**)(scheme + 0x304);
        if (!id || !id[0]) continue;

        for (j = 0; j < n; j++)
        {
            unsigned char* prev = items[s_Index[j]];
            char* pid = prev ? *(char**)(prev + 0x304) : NULL;
            if (pid && lstrcmpiA(pid, id) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        s_Index[n++] = i;
    }
    s_Count = n;
    if (n == 0) s_ListTried = 0;
}

static void* Pc_CurrentHouse(void)
{
    return *(void* const volatile*)(size_t)PC_HOUSE_CURRENT;
}

static unsigned short Pc_Rgb565(unsigned r, unsigned g, unsigned b)
{
    return (unsigned short)((((r * 31u + 127u) / 255u) << 11) |
                            (((g * 63u + 127u) / 255u) << 5) |
                             ((b * 31u + 127u) / 255u));
}

int PlayerColor_Count(void)
{
    Pc_BuildList();
    return s_Count;
}

int PlayerColor_Current(void)
{
    void* house = Pc_CurrentHouse();
    int index, i;

    if (!house) return -1;
    index = *(volatile int*)((char*)house + PC_HOUSE_COLOR_SCHEME);
    Pc_BuildList();
    for (i = 0; i < s_Count; i++)
        if (s_Index[i] == index) return i;
    return -1;
}

int PlayerColor_Set(int slot)
{
    void* house;
    int index;

    Pc_BuildList();
    if (slot < 0 || slot >= s_Count) return 0;
    house = Pc_CurrentHouse();
    if (!house) return 0;

    index = s_Index[slot];
    if (index < 0) return 0;
    if (*(volatile int*)((char*)house + PC_HOUSE_COLOR_SCHEME) == index)
        return 0;

    *(volatile int*)((char*)house + PC_HOUSE_COLOR_SCHEME) = index;
    ((PcFnHouseColor)(void*)PC_FN_HOUSE_COLOR)(house);
    ((PcFnHouseColor)(void*)PC_FN_HOUSE_LASER)(house);
    return 1;
}

unsigned short PlayerColor_Swatch(int slot)
{
    unsigned char** items;
    unsigned char* scheme;
    int index;

    Pc_BuildList();
    if (s_Count <= 0) return 0x7BDE;
    if (slot < 0 || slot >= s_Count) slot = 0;

    items = *(unsigned char***)(size_t)PC_SCHEME_ITEMS;
    index = s_Index[slot];
    scheme = (items && index >= 0 &&
              index < *(volatile int*)(size_t)PC_SCHEME_COUNT)
           ? items[index] : NULL;
    if (scheme)
    {
        unsigned char* light = *(unsigned char**)(scheme + 0x30C);
        if (light)
        {
            int bpp = *(int*)(light + 4);
            unsigned char* palette = *(unsigned char**)(light + 0x174);
            int shade = *(int*)(scheme + 0x330);
            if (bpp == 2 && palette && shade >= 0 && shade < 256)
                return *(unsigned short*)(palette + shade * 2);
        }
        return Pc_Rgb565(scheme[0x308], scheme[0x309], scheme[0x30A]);
    }
    return 0x7BDE;
}

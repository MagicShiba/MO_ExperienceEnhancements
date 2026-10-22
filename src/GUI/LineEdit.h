/* LineEdit.h - 单行文本/按键轮询公共小工具 (ChatEdit 与 ESC 数值框共用)。
 * 按键走 GetAsyncKeyState: 边缘立刻一步, 按住 400ms 后 33ms 连发。
 * 文本函数不带选中区, 调用方需要时先自行删除选区。 */
#pragma once

#include <windows.h>

typedef struct { int vk; int prev; unsigned nextRep; int repeat; } LiKey;

static inline int LiKeyDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
}

static inline void LiKeysReset(LiKey* keys, int n, int fromCurrent)
{
    unsigned now = GetTickCount();
    int i;
    for (i = 0; i < n; i++)
    {
        keys[i].prev = fromCurrent ? LiKeyDown(keys[i].vk) : 0;
        keys[i].nextRep = keys[i].prev ? now + 400 : 0;
    }
}

static inline int LiKeyFire(LiKey* k, unsigned now)
{
    int down = LiKeyDown(k->vk);
    if (down && !k->prev) { k->nextRep = now + 400; k->prev = 1; return 1; }
    if (down && k->repeat && (int)(now - k->nextRep) >= 0)
    { k->nextRep = now + 33; k->prev = 1; return 1; }
    k->prev = down;
    return 0;
}

static inline int LiInsert(wchar_t* buf, int cap, int* len, int* cur,
                           wchar_t ch, int maxLen)
{
    int i;
    if (maxLen > 0 && *len >= maxLen) return 0;
    if (*len >= cap - 1) return 0;
    if (*cur < 0) *cur = 0;
    if (*cur > *len) *cur = *len;
    for (i = *len; i >= *cur; i--) buf[i + 1] = buf[i];
    buf[(*cur)++] = ch;
    (*len)++;
    return 1;
}

static inline void LiDeleteRange(wchar_t* buf, int* len, int* cur,
                                 int lo, int hi)
{
    int i;
    if (lo < 0 || hi <= lo) return;
    if (hi > *len) hi = *len;
    if (hi <= lo) return;
    for (i = hi; i <= *len; i++) buf[i - (hi - lo)] = buf[i];
    *len -= hi - lo;
    *cur = lo;
}

static inline void LiBackspace(wchar_t* buf, int* len, int* cur)
{
    if (*cur > 0) LiDeleteRange(buf, len, cur, *cur - 1, *cur);
}

static inline void LiDelete(wchar_t* buf, int* len, int* cur)
{
    if (*cur < *len) LiDeleteRange(buf, len, cur, *cur, *cur + 1);
}

static inline void LiMove(int vk, int* cur, int len)
{
    if (vk == VK_LEFT && *cur > 0) (*cur)--;
    else if (vk == VK_RIGHT && *cur < len) (*cur)++;
    else if (vk == VK_HOME) *cur = 0;
    else if (vk == VK_END) *cur = len;
}

static inline int LiDigit(int vk, wchar_t* ch)
{
    if (vk >= '0' && vk <= '9') { *ch = (wchar_t)vk; return 1; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9 &&
        (GetKeyState(VK_NUMLOCK) & 1))
    { *ch = (wchar_t)(L'0' + (vk - VK_NUMPAD0)); return 1; }
    return 0;
}

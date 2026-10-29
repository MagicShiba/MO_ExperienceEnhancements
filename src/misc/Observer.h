/* Observer.h - 多观战修复 + 观战面板排序，实现见 Observer.cpp。
 * 只写本机 CurrentPlayer 与纯 UI 面板名单，不碰模拟字段。
 */
#pragma once

/* 每循环轮询，挂 PowerDrawBoth。 */
void Observer_Poll(void);

/* 本机是否为观战者（只读，与 [Features] Observer 开关无关）。
 * 判据同 Observer_Poll：CurrentPlayer == Observer，或本机房是任一观战槽。 */
int Observer_LocalIsSpectator(void);

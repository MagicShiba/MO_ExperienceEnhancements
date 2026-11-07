/* CheerSelect.h - C 键只让选中单位欢呼 (单机)。实现与推导见 notes/欢呼.md。 */
#pragma once

/* 0x730F30 (AllToCheerCommandClass::Execute 入口, 8 字节) 钩子。
 * 回 0 = 跑原指令 (原版: 发 ALLCHEER 事件, 本家全体欢呼);
 * 非 0 = 跳该地址 (0x730FDB = 裸 ret, 已由本模块处理完)。 */
extern "C" __declspec(dllexport) unsigned __cdecl CheerSelect_Hook(void* regs);

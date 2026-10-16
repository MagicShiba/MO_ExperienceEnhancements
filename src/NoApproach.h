/* NoApproach.h - 无法攻击目标的单位不再冲向该目标 (左键点敌时)。
 * 由 Syringe 内嵌钩子直接调用, 无每帧轮询。实现与推导见 NoApproach.cpp。 */
#pragma once

/* 0x4D77BD (ObjectClickedAction 的 NoMove 处理器入口, 6 字节) 钩子。
 * regs: ESI=this(FootClass*), EBX=pTarget(ObjectClass*)。
 * 回 0=跑原指令; 回 0x4D7CC0=当已处理 (单位停原地)。 */
extern "C" __declspec(dllexport) unsigned __cdecl NoApproach_Hook(void* regs);

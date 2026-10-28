/* RepairAll.h - 建筑维修操作量简化 (Ctrl+维修 / 双击维修 一键全修, 或全自动修)。
 * 实现见 RepairAll.cpp。
 *
 * 钩子铁律: 钩点必须落在**无直接相对跳转**的指令边界上, 且被偷的字节里不能有
 * E8/E9/0F 8x 之类的直接相对分支 —— 钩子返回 0 时 Syringe 会把被偷字节搬到
 * 跳板里原样重放, 里面若有 rel32 就会因基址变了而跳飞。改任何 .inj 钩子后先跑
 * tools/hook-rel-audit.py。 */
#pragma once

/* 每循环轮询 (挂 PowerDrawBoth, 见 MOExperience.cpp):
 * 队列滴发 + Mode=2 的周期扫描 + 记维修快捷键是否松开过。
 * 纯只读查询 + OutList 标准事件入队。 */
void RepairAll_Poll(void);

/* 维修快捷键 ToggleRepariModeCommandClass::Execute (0x536FA0, 偷 7B:
 * `push -1 ; mov ecx,0x87F7E8`, 无 rel)。按住 Ctrl 或连按两下时吞掉原版
 * (不进维修光标模式), 改为给所有该修的建筑排队发 Repair(0x15) 事件;
 * 否则返回 0 跑原版。 */
extern "C" __declspec(dllexport) unsigned __cdecl RepairAll_HotKey(void* regs);

/* 侧栏维修图标点击 (维修分支内 0x6A78D9, 偷 6B: `mov ecx,[0x8871E0]`, 无 rel;
 * 只有 eax==egui_BUTTON_REPAIR(0x8065) 才会走到这里)。语义同热键, 另外
 * 认双击。吞掉时落点 0x6A78FF (分支公共尾 jmp, 在原地执行)。 */
extern "C" __declspec(dllexport) unsigned __cdecl RepairAll_Button(void* regs);

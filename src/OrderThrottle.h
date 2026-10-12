/* OrderThrottle.h - 大量单位下令分帧派发 (原版 OutList 一帧 128 条, 多的静默丢)。
 * 实现见 OrderThrottle.cpp; 引擎事实与推导见 notes/选中与下令上限.md。
 * 构建见 build-fast.bat。
 *
 * 钩子是 .text 直接补丁 (不进 Syringe .inj), 幂等且冲突时自动放弃:
 * ClickedMission @ 0x6FFBE0 (右键移动/攻击、G 警戒) 与槽 0x374 @ 0x6FFE00
 * (S 停止、E 进入系组命令)。中键托管见 OT_SubmitRaw。 */
#pragma once

/* 每循环轮询 (挂 PowerDrawBoth, 见 MOExperience.cpp): 懒装 ClickedMission 钩 +
 * 按 OutList 水位补发排队指令 + 换图清队列。 */
void OrderThrottle_Poll(void);

/* 中键托管 (SmartOrder/SmartArea/FuzzyAttack/PatrolCircle/TreeChop/AttackMove/
 * DeployPreview 用): 预制好的标准 MegaMission 事件 (EVT_SIZE=111B) 交 OT 跨帧补发。
 * whom/whomID/idxOk=Whom 存活校验 (OU_PtrAlive 语义)。
 * 回 1=已接管 (调用方当成功); 0=没接管 (OT 关/联机未启用/队满), 调用方走原 drip。 */
int OT_SubmitRaw(const char* ev, void* whom, int whomID, int idxOk);
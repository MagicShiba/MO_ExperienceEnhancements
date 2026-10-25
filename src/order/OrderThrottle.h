/* OrderThrottle.h - 大量单位下令: 单机直写 DoList, 联机分帧排队
 * (原版 OutList 一帧 128 条, 多的静默丢)。
 * 实现见 OrderThrottle.cpp; 引擎事实与推导见 notes/选中与下令上限.md。
 *
 * 钩子是 .text 直接补丁, 幂等且冲突时自动放弃:
 * ClickedMission @ 0x6FFBE0 与槽 0x374 @ 0x6FFE00,
 * 另三处满分支 (Mega/374/正统入队, 单机直写用)。
 * 联机默认 (mode<2) 完全绕过: 不留钩 (已装则恢复原版并释放跳板)。
 * 中键托管见 OT_SubmitRaw。 */
#pragma once

/* 每循环轮询 (挂 PowerDrawBoth, 见 MOExperience.cpp): 懒装 ClickedMission 钩 +
 * 按 OutList 水位补发排队指令 + 换图清队列。 */
void OrderThrottle_Poll(void);

/* 中键托管 (SmartOrder/SmartArea/FuzzyAttack/PatrolCircle/TreeChop/AttackMove/
 * DeployPreview 用): 预制好的标准 MegaMission 事件 (EVT_SIZE=111B) 交 OT 队列。
 * 单机永不接管 (回 0, 调用方 OU_Enqueue 直写 DoList);
 * 联机仅 OrderThrottle=2 时接管 (回 1, 跨帧补发)。
 * whom/whomID/idxOk=Whom 存活校验 (OU_PtrAlive 语义)。
 * 回 1=已接管 (调用方当成功); 0=没接管, 调用方走自家 OU_Enqueue。 */
int OT_SubmitRaw(const char* ev, void* whom, int whomID, int idxOk);

/* 发一条自家事件: 先 OT_SubmitRaw (联机 mode=2 才接管), 没接管则 OU_Enqueue。
 * 即上面两行的固定组合, 各家 Fire* 尾统一走这里。 */
int OT_Fire(char* ev, void* whom, int whomID, int idxOk);

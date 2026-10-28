/* SingleUnload.h - 载员面板入口: 点载员图标 = 单个下车; Z+点载员图标 = 给他规划路径点
 * (实现与引擎事实见 SingleUnload.cpp, 界面入口在 UnitPanel.cpp)。 */
#pragma once

/* unit = 载具/驻军建筑, pax = 面板上被点的那个载员。
 * 回 1 = 已派发 (单卸事件 / 开始规划), 调用方不要再走选中逻辑; 0 = 不归我们管。 */
int SU_ClickPassenger(void* unit, void* pax);

/* ---- 载员规划 (Z 按住点载员格 → 之后的 Z 地图点击只给这个载员加点) ----
 * 点击即发标准 MegaMission(IsPlanningEvent=1), 提交由引擎在松开 Z 时自己做;
 * 认人时先垫一个"走到原地"的节点, 供引擎提交吃掉 (它会把队首节点转成真令)。 */
/* 面板点了本车/本建筑那一格 (单选版式第 0/1 格) 且 Z 按住 = 给里面所有载员规划。
 * 回 1 = 已接管这次点击 (调用方不要再走选中逻辑)。 */
int SU_PlanArmAll(void* host);
/* 只给名单里的这几个载员规划 (面板 shift 多选的结果)。不在这个容器里的会被跳过,
 * 一个都不剩就回 0。回 1 = 已接管这次点击。 */
int SU_PlanArmList(void* host, void** list, int n);
/* 按 Z 那一刻自动认下 shift 名单 (有名单就不必再点面板一次, 直接在地图上点)。
 * 与 SU_PlanArmList 的区别: 不吞当前这次按下 —— 这一次要当第一个节点。
 * 已经有规划在进行时回 0。 */
int SU_PlanAutoArm(void* host, void** list, int n);
/* 规划进行中 shift 补选: 新出现的载员加进集合 (只增不减, 见 .cpp)。回 1 = 变了。 */
int SU_PlanResync(void* host, void** list, int n);
/* 现在有没有正在进行的规划。 */
int SU_PlanActive(void);
/* 本次规划已经发了几轮节点 (结束会清零, 所以要在这之前读)。 */
int SU_PlanFiredCount(void);
/* 这个载员是否正参与本次规划 (面板据此给第一个被 Z+点认下的人补上高亮框:
 * 他不是 shift 选进名单的, 名单里查不到)。 */
int SU_PlanHasPax(void* pax);
/* 地图点击门调用: 这次点击归载员规划就回 1 (调用方吞掉, 别让引擎给选中的载具加点)。 */
int SU_PlanClick(int mx, int my);
/* 每帧: 发待决节点、掉线 (Z 松开 / 载员没了 / 已经出舱) 清理。 */
void SU_PlanPoll(void);

/* PvpNotice.h - 无 AI 的对局里定时在聊天中声明本机使用了拓展 (非纯净版本)。
 * 实现与钩点原理见 PvpNotice.cpp / notes/联机纯净提示.md。 */
#pragma once

/* 每主循环轮询 (挂 PowerDrawBoth): 判定游戏模式与房表, 每 30 秒发一条聊天广播。
 * 联机恒发; 单人按 [PvpNotice] Single (0=不发 / 1=也发 / 2=只发一次)。
 * 大厅与载入屏不发 (等 ScenarioStarted), 开局后再静默 3 秒才发首条。
 * 纯只读查询 + 引擎自带聊天报文, 不发模拟事件、不写模拟内存。 */
void PvpNotice_Poll(void);

/* 选文本 + 读配置 (幂等, MOE_EnsureInit 里调)。 */
void PvpNotice_EnsureInit(void);

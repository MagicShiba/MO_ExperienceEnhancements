/* LightConvFix.h - 光照变更后的每格 LightConvert 刷新 (见 .cpp 文件头)。 */
#ifndef MOE_LIGHTCONVFIX_H
#define MOE_LIGHTCONVFIX_H

/* 按当前光照字段逐格重建 convert, 不看 [Environment] LightFixEnable 开关。
 * 手动改光照与场景重开都走这条 —— 与建图同路, 逐像素一致。 */
void LightConv_RebuildCells(void);

/* 上色: 让引擎把 ColorScheme (玩家色) 与 0x626690 容器 (单位/建筑) 染成
 * (r,g,b), 地形那批 LightConvert 不动 —— 池的 Count 在调用期间被压到 1, 所以
 * 每格自己的点光源色 (rules 的 LightRedTint/GreenTint/BlueTint, 如 [INORANLAMP])
 * 不会被抹成亮度。调用后须接 LightConv_RebuildCells 重建地形。
 * r/g/b 单位 = tint 字段 ×10 (150/90/60 表示 1.5/0.9/0.6), 上限 2000。
 * mix = 0..100: 三个分量按它从原色 1000 线性插值过去, 0 = 写回原色 (等于没染,
 * 同时清掉上一次的染色), 100 = 完全染色。 */
void LightConv_TriggerTint(int r, int g, int b, int mix);

/* 地形重建完了没有 (格子指针数组基址非 NULL)。重建期间写光照/扫格子会把
 * 半成品当成成品, 必须等到它真回来。 */
int  LightConv_WorldReady(void);

/* [Environment] LightFixEnable, 默认 1: 自动修复 (改光照后的逐格重建 / 读档后补刷)。
 * 关掉就是引擎原版表现 (锯齿残留 + 读档后亮度停在重建那一刻)。 */
int  LightFix_Enabled(void);
void LightFix_SetEnabled(int on);

#endif /* MOE_LIGHTCONVFIX_H */

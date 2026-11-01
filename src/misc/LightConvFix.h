/* LightConvFix.h - 光照变更后的每格 LightConvert 刷新 (见 .cpp 文件头)。 */
#ifndef MOE_LIGHTCONVFIX_H
#define MOE_LIGHTCONVFIX_H

/* 按当前光照字段逐格重建 convert, 不看 [LightingRefresh] 开关。
 * 手动改光照与场景重开都走这条 —— 与建图同路, 逐像素一致。 */
void LightConv_RebuildCells(void);

/* 统一上色: 原样调引擎触发器那条 RecalcLighting(r,g,b,0), 整池 LightConvert +
 * 每份 ColorScheme(玩家色) + 0x626690 容器一起改色。
 * r/g/b 单位 = tint 字段 ×10 (150/90/60 表示 1.5/0.9/0.6), 不归一化、上限 2000。 */
void LightConv_TriggerTint(int r, int g, int b);

/* 切回逐格前复原两类底件 (池 [0] 默认件 + 每份在用 ColorScheme) 的 Color1 =
 * 1000,1000,1000 —— 它们建出来就是这个常量, 所以不需要记录原值。返回复原件数。 */
int  LightConv_ResetBase(void);

/* 地形重建完了没有 (格子指针数组基址非 NULL)。重建期间写光照/扫格子会把
 * 半成品当成成品, 必须等到它真回来。 */
int  LightConv_WorldReady(void);

/* [LightingRefresh] Enable, 默认 1: 自动修复 (改光照后的逐格重建 / 读档后补刷)。
 * 关掉就是引擎原版表现 (锯齿残留 + 读档后亮度停在重建那一刻)。 */
int  LightFix_Enabled(void);

#endif /* MOE_LIGHTCONVFIX_H */

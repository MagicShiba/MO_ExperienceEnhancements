/* LightConvFix.h - 光照变更后的每格 LightConvert 刷新 (修锯齿残留), 见 LightConvFix.cpp */
#ifndef MOE_LIGHTCONVFIX_H
#define MOE_LIGHTCONVFIX_H

/* 无条件全格重建 (不看签名): 场景重开 (读档/换图) 后按引擎现值重算一遍,
 * 必然留日志 (普通刷新按 规模+1 秒 去重)。 */
void LightConv_RefreshNow(void);

/* 地形重建完了没有 (格子指针数组基址非 NULL)。重建期间写光照/扫格子会把
 * 半成品当成成品, 必须等到它真回来。 */
int LightConv_WorldReady(void);

#endif /* MOE_LIGHTCONVFIX_H */

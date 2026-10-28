/* CrateIcon.h - 已定型箱子在格子上方显示类型图标。
 *
 * 箱子类型 = CellClass + 0x11E。随机刷的箱子在被踩之前恒为 0xFF (引擎只在
 * CollectCrate 里 roll), 没有类型可显示, 直接跳过; 地图预置格和触发动作 108
 * 创建的箱子都有确定值, 按图标表画。
 *
 * 图标表 icon_crate_type.pcx: 两行 9 列, 行列均分:
 *   行0: 金钱 随机单位 医疗 隐形 爆炸 大爆炸 重置黑幕 显示全图 防御强化
 *   行1: 速度强化 火力强化 一次核弹 无敌 升级 离子风暴 (后 3 格空)
 * 类型字节与格位的对应见 CrateIcon.cpp 的 s_Slot。
 *
 * 画在 Composite (天气之后、UI 之前); 黑幕格按游戏目录有没有 shroud.shp 决定
 * 压暗一半照画 (半透明黑幕) 还是跳过 (不透明黑幕)。
 */
#pragma once

/* 往 Composite 画一遍图标。surf/W/H/pitch 是窗口 Composite; 返回 1 = 画了东西。 */
int  CrateIcon_Draw(void* surf, int W, int H, int pitch);

/* 设置页开关 (落盘 [CrateIcon] Enable)。SetOn 会作废扫描缓存。 */
int  CrateIcon_On(void);
void CrateIcon_SetOn(int on);

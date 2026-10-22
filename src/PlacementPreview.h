/* PlacementPreview.h - 建筑类型半透明虚影绘制 (供放置预览/展开预览共用)。 */
#pragma once

/* 视图空间 (引擎 CoordsToClient 输出, 未折界面缩放) 锚点画虚影:
 *   type   BuildingTypeClass* (vtable 39/48 取 Image/Buildup, 按需懒加载)
 *   owner  TechnoClass* / BuildingClass*, 用于取玩家色; 可传 NULL (退回 UNITx_PAL)
 *   surf   目标面; 内部自己锁/解锁, 调用方别持锁
 *   trans  0/25/50/75
 * 1x 走引擎 blit; 缩放档 (ScaleBar) 自己按缩放比缩放位图后混合写回
 * (SHP blit 本身不跟随界面缩放)。
 * 返回消耗的目标面像素数 (1x 恒 1, 0 = 没画), 供调用方做每帧预算。
 * 只读类型数据 + 渲染面绘制, 不写模拟状态。 */
int PlacementPreview_DrawGhostView(void* type, void* owner, void* surf,
    int vx, int vy, int trans);

/* [PlacementPreview] Translucency 当前生效值 (0/25/50/75, 已归一)。
 * 首次调用会触发本模块配置读取; 其它模块 (如展开预览) 直接取用,
 * 保证两处虚影同一档透明度, 不在各自 section 里重复配置。 */
int PlacementPreview_GetTranslucency(void);

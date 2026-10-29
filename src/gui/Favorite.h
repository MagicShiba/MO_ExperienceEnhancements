/* Favorite.h - 选中单位的收藏槽 (4 个) + 头顶星星 + 指向标记。
 *
 * 收藏只活在本机: 每个槽记一组 ObjectClass* + Fetch_ID, 每帧验活后剪枝。
 * 星星按钮: Ctrl+左键 = 把当前选中集收进该槽; 单击 = 镜头跳到该组;
 * 长按 (HoldMs) = 清空该槽。
 * 星星画在 UI 层之下, 超武侧栏/面板会盖住它; 指向箭头画在最上层。
 * 跳转只动本地摄像机 (TacticalClass::SetTacticalPosition),
 * 不写模拟、不发事件, 联机安全。实现见 Favorite.cpp。 */
#pragma once

#define FAV_SLOT_MAX  4     /* 收藏槽个数; 按钮按使用进度逐个露出, 最多这么多个 */
#define FAV_GROUP_MAX 64    /* 每个槽最多记多少个单位 (整组收藏的上限) */

/* 读 [Favorite] 配置 (幂等)。 */
void Fav_EnsureInit(void);

/* 每循环调一次: 换图清空 / 成员验活剪枝。 */
void Fav_Poll(void);

/* 当前该显示几个收藏按钮 (1..FAV_SLOT_MAX)。只增不减: 取消收藏不收回按钮。 */
int Fav_RevealCount(void);

/* 该槽此刻是否有收藏 (有存活成员)。 */
int Fav_Used(int slot);

/* 该槽的标记颜色 (按槽位固定: 绿/黄/红/青, ini 可改)。 */
void Fav_Color(int slot, unsigned* r, unsigned* g, unsigned* b);

/* 星星按钮按下 (左键按下沿)。ctrl=1 立即收当前选中集, 否则等松开/长按结算。 */
void Fav_PressBegin(int slot, void** sel, int selN, int ctrl);

/* 每循环调一次 (Fav_PressBegin 之后): 按住到 HoldMs 清空该槽, 松开则跳转。 */
void Fav_PressPoll(void);

/* 右键点星星按钮: 指示箭头在"贴边"与"中心同心圆"之间切换 (只改运行时, 不回写 ini)。 */
void Fav_ToggleArrowMode(void);

/* 画进 Composite: 每个成员的头顶星星。必须在 UI 层之前调用 (不许盖面板)。 */
int Fav_DrawStars(void* surf, int w, int h, int pitch);

/* 画进 Composite: 每个离屏成员一个指向箭头 (按槽限数)。放在最上层。 */
int Fav_DrawArrows(void* surf, int w, int h, int pitch);

/* 画进 Composite: 离屏通信信标的指向箭头 (用该信标所属玩家的颜色)。
 * 放在最上层, 与收藏箭头同一套 ArrowMode 摆位。 */
int Fav_DrawBeaconArrows(void* surf, int w, int h, int pitch);

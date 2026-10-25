/* SidebarSkin.h - 强制使用指定阵营的侧栏/UI 资源 (纯本地视觉)。
 *
 * 配置: [Sidebar] SkinSide=0..4
 *   0 = 跟随当前阵营 (默认), 1=盟军, 2=苏联, 3=厄普西隆, 4=焚风
 *
 * 只重载 UI 资源并在加载期间临时改 Scenario->PlayerSideIndex, 返回后立即恢复;
 * 不写模拟状态, 联机时强制保持默认 (不重载)。 */
#pragma once

void SidebarSkin_EnsureInit(void);

/* 侧栏 InitGUI 入口钩不再参与: 首次加载由 0x534FA0 调用点包装完成。 */

/* 每循环 FM_Poll 之后调: 在视口/表面重建完成后应用 ESC 切换。 */
void SidebarSkin_Poll(void);

/* ESC 菜单用: 0=默认, 1..4=强制阵营。Set 标记待应用, 主循环或设置页立即应用。 */
int  SidebarSkin_Get(void);
void SidebarSkin_Set(int value);

/* 当前实际使用的 UI 阵营 (0=盟军 1=苏联 2=厄普西隆 3=焚风)。 */
int  SidebarSkin_Side(void);

/* Action 900: switch the local sidebar skin; visual-only, works in MP too. */
void SidebarSkin_SetAction(int value);

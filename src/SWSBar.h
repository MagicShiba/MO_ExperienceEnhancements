/* SWSBar.h - 超武侧栏每循环轮询 + 渲染期绘制 (实现见 SWSBar.cpp)。 */

#pragma once

/* 每循环轮询 (挂 PowerDrawBoth, 见 MOExperience.cpp):
 * 切换键边沿 + 超武扫描 (只读, 联机安全)。开关关/隐藏时直接返回。 */
void SWS_Poll(void);

/* 渲染期绘制 (挂 MOExperience_MessageDrawHook, Composite 面):
 * 右侧 4 列图标 (盟/苏/厄/焚风, 空列隐藏)。只读 + 自有绘制, 联机安全。 */
void SWS_Draw(void* surf, int w, int h, int pitch);

/* 点击吞没门 (0x692419, 7B, 见 dist/MOExperience.dll.inj):
 * 鼠标落在侧栏区内时跳往 0x6925FC (Phobos 同式), 否则返回 0 跑原版。 */
extern "C" __declspec(dllexport) unsigned __cdecl SWS_ClickGate(void* regs);

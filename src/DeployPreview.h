/* DeployPreview.h - 中键基地车展开预览+到位自动展开入口 (实现见 DeployPreview.cpp)。 */
#pragma once

/* 认领中键 (单可展开单位选中+裸中键按住+自动展开开, SmartOrder 按下点让路用)。 */
int DeployPreview_ClaimsMiddle(void);
/* 每循环轮询 (挂 PowerDrawBoth): 点按边沿下单+到位跟踪+Deploy 事件。 */
void DeployPreview_Poll(void);
/* 渲染期展开框 (MOExperience_MessageDrawHook 内调用, 中键按住且选中含可展开单位时才画)。 */
void DeployPreview_Draw(void* surf, int w, int h, int pitch);

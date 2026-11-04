/* SWPanel.h - 超武面板 (右下整块重绘), 见 SWPanel.cpp。
 * 原版列表由 VecText 的 Print 入口整体隐藏+收集, 此处每帧从收集表取
 * 新鲜行画统一面板: 单个半透明底 + 右/底边距锚定右下 + 名左/计时器右分栏
 * + Ready 变绿。收集不到新鲜行即隐藏, 不可能画到别处。
 * 配置节 [SWPanel]。
 */
#pragma once

#define SWPANEL_SECTION "SWPanel"

void SWPanel_EnsureInit(void);
/* 渲染期: 主画布尺寸同时回填给收集门 (坐标系一致才收, 否则走原版)。
 * 返回 1=画出, 0=无新鲜行, -1=GDI失败, -2=Lock失败。 */
int SWPanel_Draw(void* surf, int bitLineH, int surfW, int surfH, int surfPitch);

/* ESC 设置页运行期读写 (只改内存, 不写 INI; 帧内下次绘制即生效)。 */
enum {
    SWO_ENABLE = 0,   /* [SWPanel] Enable */
    SWO_FONT_PX,      /* FontPx (0 = 跟点阵字高) */
    SWO_BOLD,         /* Bold */
    SWO_CLASSIC,      /* Classic: 1=原本样式, 0=统一面板 */
    SWO_OPACITY       /* Opacity 底板不透明度 */
};
int  SWPanel_GetOption(int id);
void SWPanel_SetOption(int id, int value);
void SWPanel_GetFace(wchar_t* out, int cap);
void SWPanel_SetFace(const wchar_t* face);

/* 取消设置时整份退回 (与 MsgText_SaveOptions/RestoreOptions 同语义)。 */
void SWPanel_SaveOptions(void);
void SWPanel_RestoreOptions(void);

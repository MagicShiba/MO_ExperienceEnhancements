/* SaveShot.h - 存/读档界面的存档缩略图, 详见 notes/存档截图.md。
 *
 * 落盘走旁挂文件 <存档目录>\Thumbs\<存档名>.bmp, 不碰 .SAV 本体;
 * 显示走 0x622396 的整屏面: 面板内部重排 (列表拉宽 + 右侧竖排两个截图框,
 * 上=当前画面, 下=列表选中那档已存的图)。
 * 纯读渲染面 + 写本地文件, 不参与模拟, 联机无操作。
 */
#pragma once

/* 读 [SaveShot] (幂等)。 */
void SaveShot_EnsureInit(void);

/* 消息帧钩里紧接 Shot_CapturePending 之后调: 按墙钟节流缓存一张视口缩略图。 */
void SaveShot_Tick(void);

/* 0x67CF42 (StgCreateDocfileW 调用点, EDX = 宽字符完整路径): 记路径。 */
void SaveShot_OnSaveBegin(void* regs);

/* 0x67D2F1 (存档尾部, BL = 1 表示成功): 把当前缓存写成旁挂 BMP。 */
void SaveShot_OnSaveEnd(void* regs);

/* 0x622396 (对话框内容画进临时面之后) 里调, 自己判断是不是存/读档对话框。 */
void SaveShot_PaintDialog(void* regs, void* surf);

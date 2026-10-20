/* GridView.h - 单机地图格网显示 (缓存 + 绘制)。
 * 从 PauseView.cpp 分离: 逐格自有平面缓存 (停稳重建) + 灰线/红线绘制。
 * 只读查询 + 渲染面写入, 仅单机生效。配置见 [BottomBar] GridCullBack /
 * GridShowBlocked。构建见 build-fast.bat。
 */
#pragma once

/* 读 [BottomBar] 格网配置 (幂等)。 */
void GV_EnsureInit(void);

/* 配置值 (供日志用)。 */
int GV_CullBack(void);
int GV_ShowBlocked(void);

/* 开关状态; Toggle 翻转并处理缓存 (关时释放内存, 开时作废等重建), 返回新状态。 */
int GV_IsOn(void);
int GV_Toggle(void);
void GV_SetOn(int on);

/* 缓存作废 (新开/读档/换图/开关切换时调, 保留已分配内存供复用)。 */
void GV_Drop(void);

/* 每循环调: 移动的帧隐藏, 停稳 (key 连 150ms 一致) 后重建。 */
void GV_PollStable(void);

/* 把缓存格网画进已 Lock 的 16 位面 (压底用)。
 * cx0/cy0/cx1/cy1 = 视图区 A 裁剪 (clip=0 时不限)。 */
void GV_DrawTo(unsigned char* base, int pitch, int W, int H,
               int cx0, int cy0, int cx1, int cy1, int clip);

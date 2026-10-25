/* PiP.h - 画中画 (第二摄像机小窗)。
 * 临时摆一套摄像机状态和离屏面, 按引擎 mode 0/1/2 趟序调 TacticalClass::Render,
 * 再把画面贴回 Composite。只写渲染面, 不改模拟状态。原理见 notes/画中画.md。
 */
#pragma once

/* 读 [PiP] 节 (幂等)。 */
void PiP_EnsureInit(void);

/* AfterMap 钩内 (窗口面已还原) 调: 跑第二摄像机离屏渲染 (不贴窗)。 */
void PiP_AfterMap(void);

/* 1 = 正处于 PiP 离屏渲染中。项目自身的渲染钩子 (迷雾护栏/拉伸 blit 兜底等)
 * 在此期间必须空转, 否则会按主视图状态换面/拦矩形, 把 PiP 画面弄坏。 */
int PiP_InRender(void);

/* RenderEntry 钩调 (主视图渲染之前):
 *   1) 窗内右键拖动期间钉住主相机 (引擎的右键滚屏每帧都在改它);
 *   2) 聚焦时小地图点击只切小窗, 主摄像机退回原位 (改回在渲染之前, 故主视图不闪)。 */
void PiP_PreRender(void);

/* ProcessClickCoords 入口处理方式 (物理坐标): 0=原版, 1=吞掉, 2=重映射 (操作模式)。
 * 重映射需配合 PiP_BeginClickMap / PiP_EndClickMap; 右键放行给游戏。 */
int PiP_ClickCoordsMode(int gx, int gy);

/* 操作模式重映射: 取小窗视口内坐标 + 临时摆好小窗相机状态 (需成对调用)。
 * 返回 1 = 成功 (px/py 为离屏视口坐标), 0 = 失败 (调用方应吞掉这次点击)。 */
int PiP_BeginClickMap(int gx, int gy, int* px, int* py);
void PiP_EndClickMap(void);

/* 相机定位钩 (0x6D6070 入口) 调: 聚焦时标记"这次是程序化相机跳转" (小地图点击等),
 * 由 PiP_PreRender 截胡。world = 引擎传入的相机世界坐标指针 (可为 NULL)。 */
void PiP_OnCameraSet(const int* world);

/* 小地图原始格钩 (0x653D92, 钳制前) 调: 存下未钳制格, 供小窗按自身尺寸重钳。 */
void PiP_OnRadarRawCell(int cx, int cy);

/* Composite 上的实际贴窗 (必须在缩放的放大/降采样呈现之后调, 否则被一起缩放)。 */
void PiP_DrawOnComposite(void* surf);

/* 侧栏开关按钮: 画进侧栏面, 在条带上屏前调 (同底部电量)。 */
void PiP_DrawButton(void);

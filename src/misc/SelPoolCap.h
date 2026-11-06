/* 本地渲染/选择候选池, 每帧由 Render 重建, 不参与联机模拟。 */
#pragma once

/* 延迟安装整组补丁。 */
void SelPool_Poll(void);

/* 实际容量: 安装后 4096, 否则 500。 */
unsigned SelPool_Capacity(void);

/* 实际基址: 安装后为模块静态缓冲, 否则为引擎原表。
 * 所有读取和快照/还原必须使用此接口, 避免新旧表混用。 */
unsigned char* SelPool_Base(void);

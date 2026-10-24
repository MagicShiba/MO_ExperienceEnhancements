/* SWSTest.h - 自动化时序回归 (`[Log] TestSws`)。只测试用, 不产功能。
 * 动作走与鼠标/热键同一条状态路径, 量到的就是真实路径的开销。 */

#pragma once

/* 读 ini 里的时序串。只在 SWS 初始化时调一次 (此时 g_IniPath 已就绪)。 */
void SWSTest_Init(void);

/* 每逻辑帧一次。没配时序时就是一次 int 判断。 */
void SWSTest_Tick(void);

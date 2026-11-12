/* BuildAlly.cpp - 读档后恢复「盟友基地旁建造」。
 *
 * 引擎只在遭遇战 (GameMode=5) 存/读整块 GameModeOptions, 战役读档后
 * BuildOffAlly 不恢复, 建造范围扫描就不把盟友建造场算进基地范围。启动器进
 * 战役恒启用该选项, 所以每循环保持它为 1; 读档丢的值下一帧补回, 不依赖
 * 读档钩子的执行顺序。
 * 联机 (GameMode 3/4) 一律不碰: 该字节决定建造合法性, 各端必须一致。 */

#include "EngineAddr.h"
#include "GdiText.h"

void BuildAlly_Poll(void)
{
    volatile unsigned char* flag;
    int mode;

    mode = *(volatile int*)ADDR_GAMEMODE;
    if (mode == 3 || mode == 4) return;
    if (!*(volatile unsigned char*)ADDR_SCEN_STARTED) return;

    flag = (volatile unsigned char*)ADDR_BUILD_OFF_ALLY;
    if (*flag == 1) return;
    *flag = 1;
    LogLine("buildally: BuildOffAlly restored to 1");
}

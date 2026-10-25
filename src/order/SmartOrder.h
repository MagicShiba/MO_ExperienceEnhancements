/* SmartOrder.h - 中键智能下令 (点按自适应/划线散开) 入口 (实现见 SmartOrder.cpp)。 */
#pragma once

void SmartOrder_Poll(void);
/* 渲染期橡皮线 (MOExperience_MessageDrawHook 内调用, armed 时才画)。 */
void SmartOrder_Draw(void* surf, int w, int h, int pitch);

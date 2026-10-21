#pragma once

#define PLAYER_COLOR_MAX 96

int PlayerColor_Count(void);
int PlayerColor_Current(void);
int PlayerColor_Set(int slot);
unsigned short PlayerColor_Swatch(int slot);

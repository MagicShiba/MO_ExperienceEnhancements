#pragma once

#define PLAYER_COLOR_MAX 96
#define PLAYER_COLOR_RAINBOW_SEGS 6

int PlayerColor_Count(void);
int PlayerColor_Current(void);
int PlayerColor_Set(int slot);
unsigned short PlayerColor_Swatch(int slot);
unsigned short PlayerColor_RainbowSwatch(int index);
void PlayerColor_Tick(void);

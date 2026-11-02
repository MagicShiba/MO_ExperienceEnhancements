/* SoundFx.h - 按 sound.ini 的名字播引擎音效, 实现见 SoundFx.cpp。
 *
 * 名字是 sound.ini 的条目名 (rules.ini 里 `StormSound=WeatherIntro` 那种), 不是
 * wav 文件名。纯本地发声, 联机安全。
 */
#pragma once

/* 循环音通道: 雨声与雪天的风声各占一条, 可以同时响。 */
#define SFX_LOOP_RAIN 0
#define SFX_LOOP_WIND 1

/* 名字 -> 音效下标 (缓存)。找不到 (表未装载 / 名字写错) 返回 -1, 失败不缓存。 */
int  Sfx_Index(const char* name);

/* 播一次。pan: 0=左 / 0x2000=中 / 0x4000=右;
 * vol: 在 sound.ini Volume 之上的百分比, 100 = 原音量。
 * 返回 1 = 已交给引擎 (声音总开关关着时引擎自己静默)。 */
int  Sfx_Play(const char* name, int pan, int vol);

/* 循环音 (sound.ini 里 Control 含 loop 的条目): 在指定通道起一条并持有它的
 * AudioController, 直到 Sfx_LoopStop。同一通道只跟一条, 换名字会先停旧的;
 * 不同通道互不影响。返回 1 = 正在循环。 */
int  Sfx_LoopStart(int ch, const char* name, int pan, int vol);
void Sfx_LoopStop(int ch);

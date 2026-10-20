/* Lang.h - 多语言文本加载器。 */
#pragma once

#include <windows.h>

/* 从系统语言选择 MOExperienceResources\<locale>.ini；一次初始化。 */
void Lang_EnsureInit(void);

/* 系统 UI 语言是否为 zh 系列 (用于选择内置缺失提示文案)。 */
int Lang_IsChinese(void);

/* 语言覆盖 (ESC 菜单运行期切换): 0=AUTO 跟随系统, 1=ZH, 2=EN。
 * 只改运行时选择, 不写 ini; 切换后调用方需自行重载缓存文本。 */
#define LANG_MODE_AUTO 0
#define LANG_MODE_ZH   1
#define LANG_MODE_EN   2
int  Lang_GetMode(void);
void Lang_SetMode(int mode);

/* section/key 使用语言文件内的短节名 (如 "EscMenu"/"MsgRadar")。
 * 语言文件缺失时回退主 ini 同名节，再回退默认值。
 * 返回 1 = 内容来自语言文件或主 ini，0 = 使用了 def。 */
void Lang_GetA(const char* section, const char* key, const char* def,
               char* buf, int cap);
int  Lang_GetW(const char* section, const char* key, const wchar_t* def,
               wchar_t* buf, int cap);

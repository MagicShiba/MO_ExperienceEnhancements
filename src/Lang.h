/* Lang.h - 多语言文本加载器。 */
#pragma once

#include <windows.h>

/* 从系统语言选择 MOExperienceResources\<locale>.ini；一次初始化。 */
void Lang_EnsureInit(void);

/* section/key 使用语言文件内的短节名 (如 "EscMenu"/"MsgRadar")。
 * 语言文件缺失时回退主 ini 同名节，再回退默认值。 */
void Lang_GetA(const char* section, const char* key, const char* def,
               char* buf, int cap);
void Lang_GetW(const char* section, const char* key, const wchar_t* def,
               wchar_t* buf, int cap);

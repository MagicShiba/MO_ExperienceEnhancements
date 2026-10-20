# ESC 菜单自定义页 (EscMenu)

代码: `src/GUI/EscMenu.cpp` / `EscMenu.h`。配置: `dist/MOExperience.ini` 的
`[EscMenu] Enable=1`，并同步到游戏目录。只改显示与输入，不写模拟状态。

## 1. 定位

ESC 菜单是引擎创建的无模式 Win32 对话框：

- 入口 `0x004F10E0`，对话框创建包装 `0x00622650` (`CreateDialogIndirectParamA`),
  DialogProc `0x004F11B0`。
- 模板统一 533x369 对话框单位；181 单机 / 255 联机 / 3002 其它模式 /
  3003 游戏控制子页。
- `1313` 是 ESC 首页的“游戏控制”按钮；用 `GetDlgItem(hDlg, 1313)` 过滤 3003
  子页。`GameMode 3/4` (LAN/Internet) 直接跳过。
- 公共控件：`1670` 返回、`1684` 右侧栏标签、`1685` 状态/底栏标签。

## 2. 绘制

游戏在 `OwnerDraw::Paint (0x621E90)` 中先把菜单画进临时 16 位 BSurface，再
blit 到 `[0x887310]`。在 `0x622396` (6B = `mov ecx,[0x887310]`) 处绘制：

- ESI = 临时 BSurface；栈 `[esp+0x10]` = 对话框 HWND。
- BSurface: vtable `0x7E2070`, Width `+4`, Height `+8`, BPP `+0x10`,
  Buffer `+0x14`；`GtLock/GtUnlock/GtSurfGet` 可用。
- `FillRectTrans` 在 BSurface 上返回 0，像素填充需用 `Blend16` 手写。
- 子窗口不会被 owner-draw 注册，下一次菜单 blit 会盖住，因此不创建子窗口。
- 避免使用 `[esp+0x18]` 取 HWND，类型分支会改写它。

钩子：`0x4F1140 = EscMenu_OnShow, 5` (仅记录/子类化)；
`0x622396 = EscMenu_OnPaintSurface, 6` (绘制)。两处都要在
`src/SyringeHooks.cpp` 与 `dist/MOExperience.dll.inj` 同步。

## 3. 布局

内容框四边使用同一像素边距 `m = W*6/533`，下限 8px：

- 左: `m`
- 上: `m`。`1684` 在右侧栏，不在面板上方，不能用它的底边作为上界。
- 右: `W - 168 - m`。右侧栏宽固定 168px (Ares `WorkAreaWidth=632`，
  800-632=168)；返回按钮 `1670` 在栏内，比栏左边更靠右，不能作为右界。
- 下: `1685.top - m`。底栏仍在面板下方，屏幕底边会额外留下底栏高度。

左右面板间距同样为 `m`，左侧占 40%。1920x1080 实测约为
`L=21,21 R=1731,1037`，状态栏顶 `1058`，右侧栏左边 `1752`。

## 4. 文字与交互

- 标题 18px，正文 16px；GDI 槽位 10=标题，11=正文 (`GDI_SLOTS=12`)。
- 文字在 2x DIB 上渲染，取 2x2 区域的红通道平均值作为覆盖率，再 `Blend16`
  写入 16 位 BSurface。`ANTIALIASED_QUALITY` 下 RGB 相同，`BlitCoverage`
  也读红通道，因此行缓存只需写回红通道。
- 实测行高：正文 16px -> 显示约 22px，标题 18px -> 约 24px；`ESC_ROW_H=24`。
- 面板内边距 8，文字内缩 6；边框 2px `#7B797B`；选中行 `#7B207B`。
- 仅子类化对话框处理输入，不创建子窗口：`WM_LBUTTONDOWN` 命中左侧行时更新
  `s_Sel`，`InvalidateRect + UpdateWindow` 重绘；其它消息原样转发。
- 原 WndProc 存在窗口属性 `MOE_EscOldProc`；按 `IsWindowUnicode` 选择
  `CallWindowProcA/W` 或 `DefWindowProcA/W`。

## 5. 多语言

- 每个语言一个文件：`MOExperienceResources\<locale>.ini`，节名固定为
  `[EscMenu]` 和 `[MsgRadar]`。
- 语言取系统 UI 语言；存在同名的 `<locale>.ini` 就优先使用。
  zh 系列无对应文件用 `zh-CN.ini`，其它语言无对应文件用 `en-US.ini`。
- 菜单标题和占位项读取 `[EscMenu]` 的 `LeftTitle`、`LeftItem1/2`、
  `RightTitle`、`RightItem1/2`；雷达文案读取 `[MsgRadar]` 的
  `Name0..Name16`、`NameOther`。
- 语言文件缺失或条目为空时，回退旧主 ini 同名节/内置默认值。

## 6. 日志

`MOExperience.log`：`esc: init on/off`、`esc: host set`、`esc: subclass ok`、
`esc: subclass fail`、`esc: select N`。

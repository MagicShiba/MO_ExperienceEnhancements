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
`0x622396 = EscMenu_OnPaintSurface, 6` (绘制)。在
`src/SyringeHooks.cpp` 注册，构建时编入 `.syhks00`，不再需要 `.inj`。

## 3. 布局

内容框四边使用同一像素边距 `m = W*6/533`，下限 8px：

- 左/右: `m` 与 `W - 168 - m`。右侧栏宽固定 168px (Ares
  `WorkAreaWidth=632`，800-632=168)。
- 上/下: 在 `m` 基础上再留 `ESC_VMARGIN=24`，底边优先取 `1685.top - m -
  24`，让面板上下留白更大。
- 左面板固定 `300`，右面板固定 `600`，中间 `10`。小分辨率下先压缩右面板，
  仍不足时再压缩左面板，避免侵入右侧栏；空间充足时整组面板水平居中。

## 4. 文字与交互

- 标题 18px，正文 16px；GDI 槽位 10=标题，11=正文 (`GDI_SLOTS=12`)。
- 文字在 2x DIB 上渲染，取 2x2 区域的红通道平均值作为覆盖率，再 `Blend16`
  写入 16 位 BSurface。`ANTIALIASED_QUALITY` 下 RGB 相同，`BlitCoverage`
  也读红通道，因此行缓存只需写回红通道。
- 实测行高：正文 16px -> 显示约 22px，标题 18px -> 约 24px；`ESC_ROW_H=26`。
- 面板内边距 8，文字内缩 6；边框 2px `#7B797B`；选中行 `#7B207B`。
- 仅子类化对话框处理输入，不创建子窗口：`WM_LBUTTONDOWN` 命中左侧行时更新
  `s_Sel`，命中右侧设置行时执行开关；数值行中间的文本框为内联编辑。编辑/按键
  轮询复用 `LineEdit.h`（ChatEdit 同套）：16ms `WM_TIMER` + `GetAsyncKeyState`
  （数字、退格、Delete、方向键、Home/End，方向/删除键按住连发），数字在光标处
  插入，点击文本框按最近字符边界定位光标；回车提交、Esc 取消，失去焦点或点击
  别处时提交。
  `InvalidateRect + UpdateWindow` 重绘；其它消息原样转发。
- 左侧第一项为“GUI功能”，右侧按功能组显示：`通用`（建筑队列、放置时建筑
  驱逐单位）、`超级武器侧边栏`（侧栏总开关、排序方式、显示行数、面板停靠边）、
  `底栏功能`（视图缩放、暂停和帧进、网格显示）、`侧边栏`（侧栏滚动条、
  隐藏侧栏功能）与`杂项`（玩家颜色、阵营 UI）。分组竖线与分组文本同高，
  分组文本与子项文本左对齐。
- 左侧第二项为“文本显示调整”，右侧按三组显示：`作战文本信息`（战役文本框
  开关、文本行数、背景透明度、显示位置、水平对齐、宽度、打字机显示、滚动条）、
  `聊天文本框`（自定义输入框、单机名字前缀）与`操作与战斗提示`（启用开关、
  文本颜色、显示行数、宽度、滚动条、显示雷达事件）。组间用分割线隔开。
  该页内容较高，超出面板时用鼠标滚轮滚动（滚动量吸附到行顶，右侧画滑块）。
- 文本页设置回写主 INI 的 `[MsgText] CampEnable/MaxLines/BoardOpacity/
  CampBottom/CampCentered/MaxWidth/CampAnim/CampBarSide/ChatEdit/ChatNameSP/
  HintShow/HintColorFollow/HintMaxLines/HintMaxWidth/HintBarSide/
  HintRadarShow`，并经 `MsgText_GetOption/SetOption`、`ChatEdit_Enabled/
  SetEnabled`、`ChatEdit_ShowNameSP/SetShowNameSP` 立即生效。
- “战役文本框”关闭时 `MsgText_DrawFrame` 与 `MsgText_Enabled` 一起返回 0，
  整个消息框交回引擎原版绘制（战役与操作/战斗提示一起回原版），此时提示组
  选项只写入 INI、不生效，重新开启战役文本框后恢复。
- ESC 菜单选中项与文本页滚动位置跨次打开保留。
- 设置页点击只改内存并排队 (`Esc_QueueValue/Esc_QueueRaw`)，切左栏项不落盘，
  仅在正常关闭菜单（隐藏/销毁/新菜单句柄）时 `Esc_FlushPending` 统一写 INI；
  同一 `section+key` 只保留最后一次值。鼠标左键 `WM_LBUTTONDBLCLK` 与
  `WM_LBUTTONDOWN` 同处理，连续点 `+`/`-` 不会被双击吞掉。
- 右侧面板下方靠右有“取消”按钮（文本 `EscMenu/Cancel`）：按下变暗，松开且仍在
  按钮内才触发（按下后移出则取消本次点击）；随后丢弃本次待写队列，恢复打开菜单
  时的运行期快照（GUI/文本/颜色/侧栏皮肤等），再向原生返回按钮 `1670` 发
  `WM_COMMAND` 回到游戏。正常返回仍保存并落盘。
- “隐藏侧栏功能”为四段选择：`禁用` / `完全显示` / `小地图` / `不显示`，对应
  `[Sidebar] HideEnable` 与 `Mode`。运行期选`禁用`时模块内部仍按完全显示跑
  （缩放开着时视口补偿不能停），只是不再响应切换键；下次启动按
  `HideEnable=0` 真正关掉。若启动时本就禁用（全宽视口补丁未安装），其它三档
  只记录选择、重启后生效。
- “面板停靠边”为两段选择：`右侧` / `左侧`，对应 `[SWSidebar] Side=right|left`，
  切换时同步列序、滚动位置与命中缓存。
- 左侧`功能菜单`标题栏右端显示 `ZH` / `EN` / `AUTO` 三个固定分段（不额外增加
  分组或说明文本），点击某段立即切换语言（仅本次运行有效，不写 INI）。切换后
  菜单文案、缺失提示与雷达文案缓存同步重载。
- 开关文字在左、胶囊开关和开/关状态在右；开启轨道使用紫色主题。胶囊与圆点
  使用 8x8 超采样抗锯齿，圆角横纵半径一致，圆点在轨道内垂直居中。只有点击
  胶囊开关本身才会切换，点击整行文字不生效。排序行右侧同时显示“按阵营/填充”，
  点击整行切换；行数行右侧为 `- N +`，点减/加按钮调整，点中间文本框可直接输入
  （1~40，提交时夹取范围）。文本页所有数值行同样支持直接输入。
- 设置项回写主 INI 的 `[BottomBar] ScaleEnable/PauseEnable/GridEnable`、
  `[SWSidebar] Enable/FillMode/MaxRows/Side`、`[Sidebar] ScrollEnable/
  HideEnable/Mode`、`[Features] BuildQueue` 与 `[PlaceScatter] Mode`。
  `Side` 按字符串写 `right`/`left`，其余按整数。回写按字节定位 `key=value`，
  只替换值，保留行尾注释、空行和文件编码。
- “玩家颜色”只保留界面布局/绘制/命中测试；颜色枚举、当前色、切换与
  刷新逻辑在 `src/GUI/PlayerColor.cpp`，详见 `notes/玩家颜色切换.md`。
- 原 WndProc 存在窗口属性 `MOE_EscOldProc`；按 `IsWindowUnicode` 选择
  `CallWindowProcA/W` 或 `DefWindowProcA/W`。

## 5. 多语言

- 每个语言一个文件：`MOExperienceResources\<locale>.ini`，节名固定为
  `[EscMenu]` 和 `[MsgRadar]`。
- 默认取系统 UI 语言；存在同名的 `<locale>.ini` 就优先使用。
  zh 系列无对应文件用 `zh-CN.ini`，其它语言无对应文件用 `en-US.ini`。
- 左侧标题栏的语言分段可运行时覆盖为 `ZH` 或 `EN`，`AUTO` 恢复跟随系统。
  覆盖只改内存选择（不写 INI），重启游戏后回到 AUTO。
- 菜单标题和占位项读取 `[EscMenu]` 的 `LeftTitle`、`LeftItem1/2`、
   `RightTitle`、`RightItem1/2`；GUI 设置读取 `GuiTitle`、`GuiGroupBottom`、
   `GuiScale`、`GuiPause`、`GuiGrid`、`GuiGroupSws`、`GuiSws`、`GuiSort`、
   `GuiRows`、`GuiBuildQueue`、`GuiScatter`、`GuiScatterOff/OnPlace/Auto`、
   `GuiGroupSidebar`、`GuiSideScroll`、`GuiSideHide`、
   `GuiSideHideOff/Full/Radar/Bare`、`GuiSwsSide`、`GuiSwsSideRight/Left`、
   `GuiOn`、`GuiOff`、`GuiSortFaction`、`GuiSortFill`、`GuiSkin*`、`GuiColor`；
   文本页读取 `TextTitle`、`TextGroupCamp/Chat/Hint`、`TextCamp*`、`TextChat*`、
   `TextHint*`、`TextBar*`、`TextAlign*`、`TextColor*` 与 `TextPos*`；
   雷达文案读取 `[MsgRadar]` 的 `Name0..Name16`、`NameOther`。
- 所有菜单文案缺失时先回退旧主 ini 同名节，仍缺失则显示“文本缺失”；
  设置面板顶部会显示“文本缺失，请检查多语言文件”。代码内不保留默认文案。
- 缺失提示本身只按系统语言分两组：zh 系列用中文，其它语言用英文
  (`Text missing` / `Text missing - check the language file`)。

## 6. 日志

`MOExperience.log` 只保留功能启用与关键失败：`esc: init on/off`、
`esc: subclass fail`；只有语言文件完全缺失时记 `lang: no language file`。
INI 回写与运行期语言切换不记录日志。

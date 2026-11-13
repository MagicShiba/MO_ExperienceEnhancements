# ESC 菜单自定义页 (EscMenu)

代码: `src/gui/EscMenu.cpp` / `EscMenu.h`。配置: `dist/MOExperience.ini` 的
`[EscMenu] Enable=1`，并同步到游戏目录。只改显示与输入，不写模拟状态。

## 1. 定位

ESC 菜单是引擎创建的无模式 Win32 对话框：

- 入口 `0x004F10E0`，对话框创建包装 `0x00622650` (`CreateDialogIndirectParamA`),
  DialogProc `0x004F11B0`。
- 模板统一 533x369 对话框单位；181 单机 / 255 联机 / 3002 其它模式 /
  3003 游戏控制子页。
- `1313` 是 ESC 首页的“游戏控制”按钮；用 `GetDlgItem(hDlg, 1313)` 过滤 3003
  子页。联机模板 255/3002 同样带这个按钮，所以**联机也接管首页**，只把会分叉
  的项置灰只读（见 §4 末尾“联机”）。
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
`src/core/SyringeHooks.cpp` 注册，构建时编入 `.syhks00`，不再需要 `.inj`。

## 3. 布局

内容框四边使用同一像素边距 `m = W*6/533`，下限 8px：

- 左/右: `m` 与 `W - 168 - m`。右侧栏宽固定 168px (Ares
  `WorkAreaWidth=632`，800-632=168)。
- 上/下: 在 `m` 基础上再留 `ESC_VMARGIN=24`，底边优先取 `1685.top - m -
  24`，让面板上下留白更大。
- 左面板固定 `300`，右面板固定 `600`，中间 `10`。右栏是内容栏，宽度先保它：
  空间不足时差额全从左栏扣，左栏扣到 `ESC_LEFT_MIN=176` 为止，剩下的差额才由
  右栏承担（那时行内控件开始折行，见本节「控件流」）；空间充足时整组面板水平居中。

### 控件流 (行内自动换行)

一行 = `[行标题][控件原子...]`。原子的宽度各不相同（一段文字 / 一个数值格 /
一个开关），宽度由 `Esc_RowAtoms` 按实测字宽给出，位置由 `Esc_FlowPlace` 统一
排布 —— **加一种行类型只需在 `Esc_RowAtoms` 里给它一组原子宽度，不许哪一行自己
判断放不放得下**，否则每行的折行行为都会不一样。

- 整串在同一行放得下：整串右对齐（原观感）。
- 放不下：先按 [标题右缘的控件列首, 右界] 的容量（以及该行的"每行最多几段"）
  把原子分成若干行，再**每行各自右对齐**到右界 —— 折行后的参数一律贴右，不与
  行标题左对齐，也不会跑到名称底下去。行宽是按那个容量分出来的，所以右对齐后
  行的起点必然 >= 列首，标题永远压不到参数。
- 行有固定列数时传 `per`（当前只有云影风格：`ESC_STYLE_PER=4`，8 个预设固定
  两行、每行四个，不随栏宽变成 5+3）。栏实在太窄时仍按容量提前折行（宁可三行，
  不裁剪文字）。
- 只有列首连第一个原子都放不下（超长标题 + 窄栏）时，标题独占第一行，整串从
  行首 `xIns` 用满整行；那时 `EscFlow.x0line = 0`，标题按整行宽绘制。
- 行高 = `ESC_ROW_H × f.lines`，绘制与命中都读同一份 `f.x[]/f.ln[]`，所以
  “画在哪就点在哪”。拖动条是单控件，放不下时整条挪到第二行并用满整行宽
  （`f.lines=2`，见 `Esc_FlowCtlY`）。

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
- 左栏第一项为“GUI功能”，右侧按功能组显示：`建筑`（建筑队列、放置时建筑
  驱逐单位、放置建筑时显示预览 0/25/50/75/100、折线集结点、快捷维修
  关闭/自动/半自动）、`超级武器侧边栏`（侧栏总开关、排序方式、显示行数、
  面板停靠边）、`底栏功能`（视图缩放、暂停和帧进、网格显示、底栏截图键）、
  `侧边栏`（侧栏滚动条、隐藏侧栏功能、显示耗电与耗时）、`单位面板`（启用单位
  面板、左侧按钮四段、收藏星标、箭头指示）、`小窗口`（启用、窗内左键操作、
  自动跟随雷达事件、尺寸一行（宽/高）、分辨率倍率）。分组竖线与
  分组文本同高，分组文本与子项文本左对齐。
  “游戏控制”与“杂项”两组在第 5 页（见下）。
- 左栏第三项为“显示设置”（`EDIT_PAGE_DISP` / `s_DispLayout`）：`环境显示`
  （光照预设、光照参数、调色比例、矿石光照、天气绘制方式、天气、云层
  （云影/雾气/关闭 三段）、云影风格 → 分割线 → 光照修复总开关）与
  `界面外观`（玩家颜色、阵营 UI）。光照修复这个总开关管的是上面整组参数
  "改完要不要自动重建 convert"，所以摆在组末、与参数之间用分割线（`EG_SEP`）隔开。
  这一页与 GUI 页**共用 item 枚举、段名表和 `Esc_ApplyGui` 分派**，只是行表不同，
  所以加行/移动分组只改 `s_GuiLayout` / `s_DispLayout` 两张表。
  页身份只影响编辑态归属（`Esc_EditIs(page, item)`、`Esc_ApplyNum(page, ...)`），
  "非文本页"一律按 `page != EDIT_PAGE_TEXT` 判，别写回 `== EDIT_PAGE_GUI`。
- 左栏第四项为“快捷键”（`EDIT_PAGE_KEYS` / `s_KeyLayout`）：`下令`（中键移动
  (划战线) = SmartOrder / 模糊下令 (分配目标) = SmartArea / 快速建立巡逻圈 /
  快速砍树 / 模糊攻击）与`杂项`（选择被心控单位/移动攻击/侵略模式）。这一页
  只有 `EG_HOTKEY` 一种控件：标签 + 右侧值框。**两套中键的标签容易写串**：
  `[SmartOrder]` 是裸中键按住划一条线让单位沿线散开，`[SmartArea]` 是 Shift+
  中键的范围分配。
  - 点行 = 进**捕获态**：16ms 定时器（`ESC_HK_TIMER`）扫 `GetAsyncKeyState`，
    新按下的非修饰键按顺序攒进组合，**全部松开**才结算 —— 这样 `Ctrl+Z+Middle`
    能等齐三个键，单键也不会等超时。进捕获时先把"已按住的键"快照进 `s_HkPrev`
    （点这一行的左键、切页残留的修饰键都不算用户输入）；还没按主键就松开修饰键
    = 改主意，该修饰位撤掉。Esc 或点别处取消，8 秒无动作也自动取消。
  - 结算走 `OU_HotkeyText` 规范化成 `Ctrl+Z+Middle` 这种串，**直接 `Esc_WriteIniRaw`
    写盘 + 调该模块的 `XX_ReloadHotkey()`**。热键是模块里的运行期状态，走
    `Esc_QueueRaw` 排队的话这一局不生效，所以不进队列；取消菜单时按
    `s_HkBak`（打开菜单时备份的 ini 原串）写回并重新回读。
  - 表是 `s_Hotkeys`（item / section / key / 默认串 / 回读函数）。加一个热键 =
    加一行枚举 + `kLabelKey` 一格 + `s_Hotkeys` 一行 + `s_KeyLayout` 一行 + 语言键。
    `def` 必须与模块自己 `OU_ParseHotkey` 的默认串一致（ini 缺键时显示用）。
  - 显示串由 ini 现读再规范化，所以 ini 里手写 `ctrl+mmb` 这里会显示成
    `Ctrl+Middle`；读不到就用 `def`。
  - **侵略模式行是个例外**：它把目标范围（仅建筑 / 所有敌方单位，`[Aggressive]
    Target`）与热键并成一行，`Esc_RowAtoms` 给这一行 3 个原子（段 0、段 1、
    热键框），热键框永远是最后一个原子。点在前两段 = 切范围（直接
    `Aggressive_SetAllTargets` + `Esc_QueueRaw`），点其它地方才进捕获态。
- 左栏第五项为“游戏控制”（`EDIT_PAGE_CTRL` / `s_CtrlLayout`）：`游戏控制`
  （自定义游戏速度开关与速度滑条 → 分割线 → 游戏存档缩略图、观战修复、
  显示单位范围 (Ctrl)（数值格 + 胶囊同一行）、阻止无攻击力单位靠近目标）与
  `杂项`（箱子类型图标、设置页帧率上限 60、结算页退出按钮）。这一页原本散在
  GUI 页末尾，因为条目变多单独开了一页；它同样与 GUI 页共用 item / 控件 /
  分派表。游戏速度是**本局临时参数**，其余都是长期选项，所以滑条下面画一条
  分割线（`{ EG_SEP, 0 }`，item 不参与任何查表）。
- 页表由左栏行下标直接选（`Esc_PageAt(sel)`，下标 = `EDIT_PAGE_*`）：加一页 =
  `ESC_LEFT_ROWS + 1` + 一个 `LeftItemN` 文案 + `Esc_PageAt` 加一条分支 +
  自己的行表/滚动量。首组判定用 `i != 0` 而不是 `item != 0`，否则第一组不是
  `GUI_GROUP_GENERAL` 的页会平白多一段组间距。
- “天气”一行放雨/雪/火雨/雷电四个独立开关（火雨只有粒子画法下有效），样式与
  分段选择行一致：开 = 金色文字 + 金色横线，关 = 灰色文字 + 灰色横线，点
  文字切换、可多个同时开。四个开关只在本次运行内有效，不写 INI；以后加
  天气种类只需扩 s_Toggle 表和该行的段数（`ESC_TRIO_N`）。
- “左侧按钮”一行是 `EG_UPBTN`：四段 [关闭][折叠][wasd][收藏]。段 0 是**状态段**
  （三组全关时亮，点它 = 三组全关），段 1..3 各自独立取反；打开任一段就自动
  脱离“关闭”。三个开关落在 `[UnitPanel] BtnFold/BtnWasd/BtnFav`，点击处理在
  `Esc_ApplyGui` 的 `GUI_ITEM_UP_BTNS` 分支里自己写 INI（不走 `s_Toggle`）。
  段数常量 `ESC_UPBTN_N`，段名直接取 `s_GuiLabel` 里连着的那四个 item
  （`GUI_ITEM_UP_OFF + i`），与天气行同做法。
- “箭头指示”三档（不显示 / 边缘 / 中心）走 `s_Choice` 表，取值 = `Fav_ArrowMode()`
  （0/1/2），落盘 `[Favorite] ArrowMode = none|edge|center`。
- “云层”一行是**三段互斥**（`EG_CLOUD`）：原子表 = [云影][雾气][关闭]，整行就是
  云层的三态，没有独立开关控件 —— “关闭”即第三段，从“关”切到“雾气”是一步。
  一段就是一个取值（`Weather_CloudMode` 0/1/2），段序（显示顺序）与取值不同序，
  映射表 `kCldSegMode`；当前段由 `Esc_CloudSeg()` 反查，绘制与命中共用。
  点击只写一个键：`Esc_QueueValue(... CloudMode, kCldSegMode[atom])`。`s_Toggle` 里
  那条 `WTH_CLOUD` 保留，但 `section/key` 已置 NULL（键由这一行的点击处理自己写），
  它只供设置项快照用，画面上不再画胶囊。
- “放置建筑时显示预览”一行是五段互斥 `关/25/50/75/100`（首段文案 `GuiPlacePrevOff`，
  其余是数字）：值即 `[PlacementPreview] Opacity` 百分比，0 = 关闭，100 = 不透明。
  展开虚影没有自己的开关（`[DeployPreview]` 已无 `Ghost` 键），跟这一档走，
  两处虚影同一来源。
  所有行类型都按同一套控件流定位 (`Esc_RowFlow`)，所以折行、命中、悬停自动一致；
  点击处理里要自己判点在哪个原子上 —— `Esc_ApplyGui` 在这行先于 s_Toggle 的通用
  取反分支。
- 分段选择行（含语言切换、天气行、排序行）每段底下都画横线：选中段金色、
  未选中段灰色，横条贴行底，与文本视觉间距 2px（字形底部自带 ~3px 空隙，
  代码上线顶伸进文本框 1px 抵消）。
- 鼠标悬停的内容行画一层暗紫色高亮（组标题/分隔条不参与），光标在右面板
  行区内移动或进出该区时触发重绘；五页（GUI 功能 / 文本显示调整 / 显示设置 /
  快捷键 / 游戏控制）都生效。
- **混合格行**（`EG_MIX` / `s_GuiCells`）把几个"小标题 + 控件"并排放一行：
  格 item 落在 `s_Toggle` 画胶囊开关，落在 `s_Num` 画可编辑数值框，小标题统一
  取 `s_GuiLabel`。目前两处：小窗口`尺寸`（宽 / 高，原先是两条拖动条）与
  `显示单位范围 (Ctrl)`（对空高度 + 启用开关）。几何走控件流（`Esc_RowAtoms`），
  命中/编辑/绘制三处共用 `Esc_MixCellAt` + `Esc_MixCellBox`（数值格 = [小标题][缝]
  [数值框]，开关格只有胶囊）；`Esc_NumBoxFor` 也要认这几种格，否则点框定位光标
  会落到最右的步进框上。
  **胶囊格不带小标题**（与普通开关行的胶囊一样自证状态），所以胶囊要写在 cells
  最后一个：原子流整串右对齐到 `rowL+rowW-ESC_CTL_RIGHT`，胶囊放最后才会与普通
  开关行的胶囊落在同一列；放前面就成了"行中间一个胶囊"，与其它行错位。
- "游戏速度"为 滑条 + `[- 数值 +]`：点按钮一次 ±5（与滑条吸附档一致，到边界
  按钮置灰），点数字框可直接输入任意值（含非 5 倍数），点轨道跳值并进入拖动；
  数值超过上限显示 MAX。
- 调色比例与矿石光照是"端点行"[左段][轨道][右段][数值]：数值只显示
  不能输入，两端段是 0 / 满值 的快捷跳值（调色比例 = 默认/触发，矿石光照 =
  默认/环境），样式与分段选择行同（选中段金色下划线；行标题与段文字同基线，
  都是文字底贴横线上缘；行标题让位按实测宽度算，别用固定值）。
  点轨道跳值并进入拖动：
  调色比例一次提交要重建全部约 3.7 万格的地格光照，拖动期间只显示待生效值、
  松开鼠标才提交一次；矿石光照值轻，拖动中直接生效。
- 数值框的键盘输入只有光照参数用计算器式移位（上限三位数，换数频繁，
  位数打满后再按数字丢最左一位、新数字接在末尾），其余数值框保持满位拒绝。
  光标点击定位按前缀字符宽度累加（等宽数字下与整串渲染一致）。
- 滚轮悬停在数值控件上时微调数值、不滚页：显示行数 ±1、游戏速度 ±5、
  调色比例 ±1、矿石光照 ±1、光照参数 ±1、文本页数值行按各自步长；
  光标不在数值控件上时滚当前页。
- “云影风格”8 个预设固定两行、每行四个（`ESC_STYLE_PER`），两条都右对齐：
  团状积云 / 大块稀疏 / 细碎云 / 平滑层云 / 阴天 / 疏密不均 / 条状云带 / 卷云
  （强扭曲拉丝 + 最扁压缩的高空薄纱）。落盘 `[Environment] CloudStyle` 写序号。
- 左侧第二项为“文本显示调整”，右侧按组显示（组标题 + 组间距分隔，**组间不画线**）：
  `作战文本信息`（字体、开关、行数、背景透明度、显示位置、水平对齐、边距
  （左/顶/底 三格）、宽度、打字机显示、滚动条）、
  `聊天文本框`（自定义输入框、单机名字前缀、预设文本）、
  `操作与战斗提示`（开关、字体、背景透明度、文本颜色、显示行数、宽度、滚动条、
  显示雷达事件）、`超武倒计时面板`（开关、字体、样式 统一/原本、背景透明度）、
  `其它文本`（= `[VecText]`：字体、资金字号与偏移、底部电力 关闭/余量/负载·电力
  + 边距 左/底、启用简报重排、简报宽度、简报边框）。
  **"样式"两段与 `SWPanel` 的 `Classic` 同向**（0=统一面板 1=原本样式），
  表项 `invert` 必须为 0。页首还有两行灰字说明（`EG_NOTE`：常用字体文件名、
  常用字号），不可点。
  该页内容较高，超出面板时用鼠标滚轮滚动（滚动量吸附到行顶，右侧画滑块，
  每页都有自己的一份滚动量）。滚轮作用域是整个对话框客户区，无论光标在左面板还是右面板
  都滚当前选中的页；光标位置取 `GetCursorPos`（滚轮消息可能由引擎转发，
  `lp` 的坐标系不保证是屏幕坐标，不能直接拿来换算命中）。滑块 thumb 高度 =
  `可视高²/内容总高`，与实际可滚比例一致。
  行绘制用 `s_ClipTop/s_ClipBot` 裁到行区，三个绘制入口都要读它：`Esc_LockRect`
  （所有 fill）、`Esc_Text`（文字裁上边要跳过覆盖图前 `Y0` 行，光缩 `h` 会错行）、
  `Esc_Capsule`（开关的胶囊不经 `Esc_Fill`，漏读就会在面板外漏出半个开关）。
  滚动位置**只在中间位置吸附行顶**（避免整行被上下切一半）；越界一律直接贴底，
  不吸附 —— 底部位置一般不是行高整数倍，折行行（两行高）的那一截正好等于一整行
  时，“距底不足一行”判不出来，最后那一两行就永远露不出来。贴底判据的行高取
  `Esc_PageWalk` 输出的**本页最高行**（`maxRowH`），不是 `ESC_ROW_H`；溢出不出一行
  （`max < maxRowH`）时退化成逐像素滚。
  行高/组距不要为了“刚好放得下”去压 —— 换分辨率总会遇到只差几十像素的情况，
  交给上面的滚动。
- “边距”排在“显示位置 / 水平对齐”**之后**：两个坐标是否起效由前两行决定，
  灰显跟着上面改起来更直观（顺序只由 `s_TextLayout` 的表序决定）。
- “边距”一行是文本页的多格行 (`EG_CELLS`)：`[行标题][小标题][数值框]` ×3，
  三格都是独立 item（`TX_CAMP_MARGIN_L/T/B`，值/上下限仍在 `s_TextNum` 里，
  编辑态按 item 记，与光照参数行同做法）。**灰显 = 这个坐标现在不起作用**：
  左（`PosX`）只在“左对齐”下生效，顶（`PosY`）只在“顶部”显示下生效，
  底（`CampBottomMargin`）只在“底部”显示下生效；判据在 `Esc_TextCellActive`。
  灰显只压小标题与数值框颜色（`Esc_DrawEditBox(..., dim)`），不拦编辑 ——
  先填值再切显示位置是常见用法。加新的多格行只需往 `s_TCells` 加一行。
- **说明行 `EG_NOTE`**：整段灰字，按行宽折行（`Esc_NoteLines` 数行 → `Esc_RowFlow`
  填 `f.lines` → `Esc_RowH` 得整数倍行高，`Esc_DrawNoteRow` 用同一算法绘制）。
  不参与命中，文案键 `TextFontNote1/2`。
- **字体行 `EG_FONT`**：一行四件 `[字体名框][字号][字号框][粗体]`，几何只在
  `Esc_FontGeom` 算一次，整块右对齐到与其它行同一条右界。名字框封顶
  （`ESC_FONT_FACE_MAX=150`），否则宽面板上"字号"两个字会被挤没。
  `s_TFonts[]` 记四个目标（战役框 / 提示 / 超武面板 / `[VecText]`）的行 item、
  字号 item、`FT_*`、模块选项号与落盘键；字号 item 无独立文案（`kTextKey` 写
  NULL），行标题共用 `TextFont`。点框 = 编辑字名（`Esc_FaceBegin` 绑 `s_FaceBuf`，
  `LiEdBind` 不清字，光标落在点处），点"字号"或数字 = 编辑字号，点"粗体" =
  切粗体并立刻落盘；滚轮只认字号框那一段（`mx` 在 `[px, bx)`）。
  "粗体"与分段/开关共用 `Esc_SegTextAt`，只是文字基线取行内居中 —— 贴底基线
  会比同一行的框低 3px。
- **分割线只用在页内分节**：组与组之间不画（组标题 + `pg->gap` 已足够分隔），
  `TX_SEP5/6/7` 只在"其它文本"组内。线 2px、`0x4E/0x4C/0x4E` + alpha 150。
  分割线行**占满一行高（`ESC_ROW_H`）**，线画在行中间 —— 行高与普通行一致，
  上下行距才不会因为插线而变窄。
- **各页内间距共用一套常量**（`ESC_GROUP_H=26` / `ESC_BLOCK_GAP=6`，都从
  `EscPage.groupH/gap` 读）：所有页面的行距必须一致，加新页也别单独定。
- **模块归属 `OPM_*` + `Esc_OptGet/Set`**：同一套 `TX_*` 行分属四个模块
  （`MsgText` / `SWPanel` / `VecText` / `PowerDisplay`），表项带 `mod`，
  读写与落盘节名都由它推（`Esc_OptSection`：`OPM_PWR` 的键住在 `[VecText]`）。
  加新行别直接调某个模块的 getter，否则 `mod` 一列形同虚设。
- **字符串值（字体名）走 `Esc_QueueStr`**：`Esc_QueueValue` 只搬 int。同一个
  `(节,键)` 只留最后一次（打字是逐字符的，不合并会写几百次）。
- **"底部显示电力信息"一个下拉管两个键**（`PowerBottom` 开关 + `PowerMode`）：
  `EscTextChoice` 的 `get/set` 字段就是给它用的，落盘时两个键一起排。
  换 `[VecText]` 字体后必须 `PowerDisplay_Invalidate()`：底电覆盖位图的缓存键里
  没有字面（字号/粗体/模式在键里）；切粗体要 `VecFlushGlyphs()`，VecText 的
  字形缓存键里同样没有粗体（合成粗只改像素、不改前进宽）。
- `[-]`/`[+]` 是 `Esc_StepButton`：`ESC_STEP_BTN_W = ESC_STEP_BTN_H = 20`（正方形），
  边框走 `Esc_Outline` 四边全描。数值框/文本条同高，行高 26 时上下各留 3px。
- “预设文本”是文本页的 `EG_TEXT` 块：**标题一行 + 每条一行**（行高
  `ESC_ROW_H*(1+条数)`），屏幕上不出现字面的 `\n`。几何只在 `Esc_QuickGeom`
  算一次（框铺到按钮列左侧，右端 `[-][+]`），绘制/命中/行高都读它 ——
  行高由 `Esc_RowFlow` 填 `f.lines`；`Esc_CtlHit` 用整块矩形判命中，
  `Esc_TextApply` 再按同一份几何细判是哪条框 / 哪个按钮。
  `[-]` 删该条、`[+]` 在该条**之后**插一条空条目并进入编辑（`Esc_QuickAdd`）。
- 数据是**条目表** `s_QItem[24][161]`（与 ChatEdit 的 `CE_PRESET_N/CE_BUF_N` 对齐），
  ini 落盘时才拼成一行字面 `\n` 串（`Esc_QuickJoin`），读回按同一规则拆
  （`Esc_QuickLoad`，空条目跳过、两端空白去掉）。`Esc_QuickUtf8Len` 挡住超长
  （聊天框只读 `ESC_QUICK_RAW` 字节，写出去读不回来等于静默丢条目）。
- 点框即进编辑，**光标落在点击处**：`Esc_QuickGeom` 给出框位，点击 x 减去框左内边距
  得到文本坐标（已滚动时补 `Esc_QuickView` 的起始像素），`Esc_QuickCurAt` 逐字符
  取最近的缝（与数字框 `Esc_EditCurAt` 同口径）。`Esc_QuickBegin(hDlg, idx, curAt)`
  第三参传 -1 才是末尾（`+` 新增空条目走这条）。
  **换算**：滚动起点 `start` 的像素偏移 = `宽(整串) − 宽(s+start)`，不是
  `宽(s+start)`（后者会平白多算一整串宽）。
- 编辑是**就地**的：`LiEdit` 直接绑在该条缓冲上（`s_QEdit` 记哪条，用 `LiEdBind`
  —— `LiEdInit` 会先 `buf[0]=0`，绑已有内容会把这条清空，别用），敲一个字就
  `Esc_QuickSync(1)`（拼串→排落盘→`ChatEdit_SetQuickText`），所以聊天框里立刻可查，
  不用等回车。编辑会话与数字框互斥：开一个先收另一个。
- **任意字符只能从对话框消息来** —— `Esc_DlgProc` 截 `WM_CHAR`/`WM_IME_CHAR` 写
  缓冲（数字框只认 0-9，走按键轮询），回车/Esc/退格这类控制字在轮询里处理
  （`Esc_QuickPoll`），对话框那份直接吞掉，免得漏给引擎触发菜单快捷行为。
  光标横向滚动用二分找起点（每次测量都要过一遍 GDI，不能逐字试）。
- 文本框的**输入法**与聊天框同一套（`Esc_ImeCompRefresh/Esc_ImePin`）：系统组合窗
  在 `Esc_DlgProc` 的 `WM_IME_SETCONTEXT` 里去掉 `ISC_SHOWUICOMPOSITIONWINDOW`、
  `WM_IME_STARTCOMPOSITION` 直接返回 0，组字串画在插入点（黄字 + 下划线，
  `Esc_DrawLineBox`），候选窗贴框底。上屏字符仍走原过程转出的 `WM_IME_CHAR`，
  `WM_IME_COMPOSITION/ENDCOMPOSITION` 只旁观（读组字串后照旧交下去），否则上屏
  就断了。组字串每 16ms（`Esc_QuickPoll`）重读一次：有的输入法不给对话框发
  组字消息，只靠消息会漏画。组字中 ASCII 由输入法收编，`Esc_QuickChar` 丢掉
  （与聊天框同判据），否则会留拼音残渣。`Esc_ImePin` 传的是**持有输入上下文那个
  窗口**的客户区，不是对话框客户区，跨窗时先 `ClientToScreen` 再 `ScreenToClient`。
- 取消（点“取消”/`Esc_RestoreSettings`）整表退回打开菜单时的快照，回灌聊天框但
  **不排落盘**（`Esc_QuickSync(0)`）。
- 文本页落盘的节名由 `Esc_OptSection(mod)` 推（`[MsgText]` / `[SWPanel]` /
  `[VecText]`），键名分散在各表：`s_TextNum[].key`、`s_TChoice[].key`、
  `s_TToggle[].key`、`s_TFonts[].faceKey/boldKey`。改动经 `MsgText_*` /
  `SWPanel_*` / `VecText_*` / `PowerDisplay_*` 立即生效；取消时四个模块各自的
  `*_RestoreOptions()` 一起回退（`Esc_SaveSettings` 里统一快照）。
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
- 左栏**底部**自上而下：`ESC_LINK_N` 行链接（bilibili、GitHub，**末行**右侧是版本号），
  然后一条分隔线，再是 `ZH` / `EN` / `AUTO` 语言分段。`Esc_LangY` / `Esc_FootY`
  给出语言行与链接区整块的顶，`Esc_LinkY(p, i)` 再按 `ESC_FOOT_H` 分行；页名列表
  到链接区上面那条线为止。列表装不下就少显示几页（见 `Esc_Panel` 的 break），
  链接区不缩。多加一行要动四处按行排列的东西：`s_LinkText` / `s_LinkUrl` 的初值、
  `kLinkGlyph`、`kLinkRgb`，以及 `Esc_LoadAllText` 里那两行键名。
- 链接：文字与网址来自语言文件（`GuiGithub` / `GuiBilibili` 及两者的 `*Url`，
  缺键保留内置初值），换语言就换链接，**面板里不显示网址**；点击经 `Esc_OpenLink`
  调 `ShellExecuteW` 开浏览器（shell32 不在链接库里，`LoadLibrary` 现取一次）。
  默认白字、悬停转金（`s_LinkHot`，由 `WM_MOUSEMOVE` 的 `Esc_FootHover` 触发重画，
  面板本身不逐帧重绘），**不画下划线**。
- 图标源是 `icons/icon_a_*.png`（22x22，图案压在纯底色上，alpha 不可用）。
  `tools/ghicon.py` 把底色当透明、最亮通道当满覆盖，转成 22x22 覆盖率遮罩
  `kIconGithub` / `kIconBili`（每格 `0`~`f`），运行期不读图片。用最亮通道而不是
  亮度：品牌蓝 `#00A1D6` 按亮度算只剩一半。画法是 `Esc_Capsule` 一个 28px 暗圆盘
  + 一次 `Esc_LockRect` 内逐格混该行颜色（alpha = 格值 * 17，逐格调 `Esc_Fill`
  会锁/解锁近五百次）。GitHub 白、bilibili 品牌蓝，颜色在 `kLinkRgb`。
- 版本号取 `MOE_VERSION`（`Esc_BuildVersion` 转宽字符，不另写字面量）。
- 语言切换那一条固定显示 `ZH` / `EN` / `AUTO` 三个分段，点击某段立即切换语言
  （仅本次运行有效，不写 INI）。切换后菜单文案、缺失提示与雷达文案缓存同步重载。
- 开关文字在左、胶囊开关和开/关状态在右；开启轨道使用紫色主题。胶囊与圆点
  使用 8x8 超采样抗锯齿，圆角横纵半径一致，圆点在轨道内垂直居中。只有点击
  胶囊开关本身才会切换，点击整行文字不生效。排序行右侧同时显示“按阵营/填充”，
  点击整行切换；行数行右侧为 `- N +`，点减/加按钮调整，点中间文本框可直接输入
  （1~40，提交时夹取范围）。文本页所有数值行同样支持直接输入。
- “小窗口”一组（`GUI_ITEM_PIP_*`，`[PiP] Enable/Operate/AutoJump/Width/Height/
  Scale`）：开关三行走 `s_Toggle`，宽高走 `s_Num`，倍率走 `s_Choice`（段名固定
  `1x`/`2x`，不进语言文件）。宽高/倍率会重建 PiP 离屏面，所以这两行的滑条
  必须"松开才提交"——见 `Esc_SlideDefer`（调色比例也在这个名单里）。
  细节见 `notes/画中画.md` §交互。
- “启用单位面板”（`GUI_ITEM_UP_ENABLE`，`[UnitPanel] Enable`）关掉 = 整块不画、
  不吞点击，并当场收掉键盘移动状态（菜单开着时主循环停着，keyboard gate 可能
  还带着“有可动单位”继续吞 W/A/S/D）。
- “收藏星标”（`GUI_ITEM_FAV_STAR`，`[Favorite] StarEnable`）只管单位头顶那颗星，
  收藏按钮与跳转不受影响。
- “折线集结点”开关（`GUI_ITEM_RALLY`，`[RallyPath] Enable`）：关掉立即
  `RallyPath_SetEnabled(0)` → 清本机折线表、钩子门返回 0（引擎恢复原版单点聚集点）；
  开回来只恢复模块行为，不清任何引擎数据。Z 只是"追加尾巴"的修饰键，开关本身
  不碰输入。
- 设置项回写主 INI 的 `[BottomBar] ScaleEnable/PauseEnable/GridEnable`、
  `[SWSidebar] Enable/FillMode/MaxRows/Side`、`[Sidebar] ScrollEnable/
  HideEnable/Mode`、`[Features] BuildQueue` 与 `[PlaceScatter] Mode`。
  `Side` 按字符串写 `right`/`left`，其余按整数。回写按字节定位 `key=value`，
  只替换值，保留行尾注释、空行和文件编码。
- “玩家颜色”只保留界面布局/绘制/命中测试；颜色枚举、当前色、切换与
  刷新逻辑在 `src/gui/PlayerColor.cpp`，详见 `notes/玩家颜色切换.md`。
- 联机（`GameMode 3/4`）：
  - 面板照常显示，**只做置灰**，不加任何“联机不可用”的提示条 —— 灰掉的项
    就是不可用，多一条说明反而挤掉正文首行。
  - `Esc_MpLocked(page, item)` 是唯一判据，绘制压暗层、左键点击、滚轮微调三处
    共用。命中被锁行时**吞掉**这次点击、不往下走 —— 否则会漏给引擎那些被面板
    盖住的隐藏控件。只锁非文本页（`page != EDIT_PAGE_TEXT`），文本页的 `TX_*`
    item 与 `GUI_ITEM_*` 编号重叠。
  - 只读清单（会与其它玩家分叉，或模块本身联机就不接管）：游戏速度开关与滑条
    （GameSpeed 在 3/4 直接放行引擎）、设置页帧率上限（FrameCap 联机放行）、
    暂停与帧进（PauseView 联机不接管）、存档缩略图（联机没有存档界面）。
  - 不锁：侧栏 / 单位面板 / 小窗口 / 文本 / 光照 / 天气 / 热键页与各种下令辅助
    —— 它们是本机渲染，或走标准事件的输入辅助。
  - 阵营 UI 皮肤与玩家颜色也不锁：两者都只写本机渲染数据（侧栏 SHP/调色板与
    布局、House 的 `ColorSchemeIndex` 与 `LightConvert` 调色板），不发命令、
    不碰模拟，联机各端各改各的显示。判据是"是否进入模拟"，不是"其它端能否看到"。
- 原 WndProc 存在窗口属性 `MOE_EscOldProc`；经 `LineEdit.h` 的 `LiCallOld`
  转发（内部按 `IsWindowUnicode` 选 A/W）。

## 5. 多语言

- 每个语言一个文件：`MOExperienceResources\<locale>.ini`，节名固定为
  `[EscMenu]` 和 `[MsgRadar]`。
- 默认取系统 UI 语言；存在同名的 `<locale>.ini` 就优先使用。
  zh 系列无对应文件用 `zh-CN.ini`，其它语言无对应文件用 `en-US.ini`。
- 左侧标题栏的语言分段可运行时覆盖为 `ZH` 或 `EN`，`AUTO` 恢复跟随系统。
  覆盖只改内存选择（不写 INI），重启游戏后回到 AUTO。
- 菜单标题和占位项读取 `[EscMenu]` 的 `LeftTitle`、`LeftItem1/2/3/4`、
   `RightTitle`、`RightItem1/2`；GUI 设置读取 `GuiTitle`、`DispTitle`、
   `KeyTitle`、
   `GuiGroupAppear`、`GuiGroupBottom`、
   `GuiScale`、`GuiPause`、`GuiGrid`、`GuiGroupSws`、`GuiSws`、`GuiSort`、
   `GuiRows`、`GuiBuildQueue`、`GuiScatter`、`GuiScatterOff/OnPlace/Auto`、
   `GuiPlacePrev`、`GuiRally`、
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

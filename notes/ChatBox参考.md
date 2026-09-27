# ChatBox 参考（最小入库）

上游：`https://github.com/moon-bamboo/ChatBox/tree/master`
版本：`0.3.8`；目标引擎：`gamemd.exe` 尤里复仇 `1.001`。
只记录钩点与调用，不入库第三方源码全文。

钩子：

- `0x5D3BA0`（6 字节）：`MessageListClass::AddMessage` 入口，捕获全部消息。
- `0x5D4A94`（7 字节）：`MessageListClass::Draw` 内部画消息列表处；接管返回 `0x5D4A9B`。
- `0x777998`（6 字节）：滚轮方向。
- `0x533F50`（5 字节）：悬停消息框时跳过侧栏滚动。
- `0x692419`（7 字节）：命中消息框时吞掉穿透点击。

单人聊天框（Phobos `AllowChatBoxInSinglePlayer` 同原理，已在 `MOExperience.cpp`
用 `ChatBoxSP` 实现，`0`=走原版）：
对话框函数按 `[SessionClass 0xA8B238]==0/5` 判单人并跳过聊天框四处逻辑，
开启时跳过 session 检查（其余旗标检查保留）：

- `0x55E477`（5 字节，`mov eax,[sess]`）→ `0x55E48D`。
- `0x55E62F`（6 字节，`cmp [sess],ebx`）→ `0x55E637`。
- `0x55E693`（6 字节，`cmp [sess],ebx`）→ `0x55E69B`。
- `0x55E746`（5 字节，`mov eax,[sess]`）→ `0x55E77B`。

`AddMessage` 入口栈布局（`ESP` 指向返回地址）：

- `+0x00` 返回地址；`+0x04` 来源名；`+0x08` ID；`+0x0C` 正文。
- `+0x10` 颜色方案索引；`+0x14` 样式；`+0x18` 超时；`+0x1C` 静默标志。

字体相关：

- `BitFont::Instance = 0x89C4D0`，`+0x1C` 为字高。
- 文本绘制 `0x4A61C0`（`Fancy_Text_Print_Wide` 的 `ColorScheme*` 重载，`__cdecl`，8 参数）。
- 折行 `0x433F50`（`thiscall`，ECX 传 `BitFont*`）。
- 画布 `DSurface::Composite = 0x88731C`；可视区 `0x886FA0`。
- 颜色方案数组 `0xB054D0`（`Items@+4`，`Count@+0x10`）；`BaseColor@0x308`。
- 当前玩家 `0xA83D4C`；玩家颜色方案索引偏移 `0x16054`。

本地使用前，必须用反汇编核对上述地址是否适用于 `G:\motest\gamemd.exe`。


# 战役框显隐（淡入淡出 + 悬停展开）

调用链：`MOExperience_MessageDrawHook` 每帧算悬停/活消息 → `camp.fade`（0–100）
→ `if (showCamp) DrawBox`；`DrawBox` 内底板/阴影/文字/滚动条透明度等比缩放，
`fade=100` 与原来逐像素一致。

## 状态机（0.2.8 定案，0.2.44 补两条）

- 目标三档：活消息=100；区内驻留（静止 300ms，±2 像素防抖）=100；
  区内移动=托底（`HoverMinOpacity`，找区用）；离开=0。
- 活消息即时满亮（0.2.44，治时差）：`liveCamp` 直接 `s_fade=100`，
  新消息与引擎提示音同帧出现（原版首字同帧），不再爬 500ms 斜坡；
  淡出与悬停淡入仍走斜坡。
- 底板两态（0.2.44，Phobos hover 态）：平时逐行包裹已揭示文本
  （黑底只包墨水，左右 3/上下 2，与整块同边距）；悬停/展开铺整块
  （`camp.fullBoard=s_expand`）。透明度都走 `BoardOpacity`。
- 打字机（0.2.44）：揭示位=`ClockNow-born`（1 字/tick≈16ms，原版速率），
  首帧至少 1 字；行偏移 `g_LineOff`，未揭示行留空占位；`CampAnim=0` 关。
  提示框保持整行即时（原版 sp=1 无动画同）。
- 全显锁存：已到 100 且鼠标还在区上，区内移动不再压暗，离开才淡出。
- 展开锁存（滚动条 + 全部历史）：悬停即展开；淡出中保持上帧（hover 离开后
  历史可见淡出，否则淡出的是空盒、看起来像瞬灭）；`fade` 到 0 才收回；
  活消息独占时仍只看活的。锁存只保持不新设（0.2.45）：末条过期时上帧是
  收拢态，保持收拢直接消失（原版行为）；之前新设展开会导致闪出整块历史
  再整体消失。
- 斜坡：全程 500ms，步进按流逝毫秒算（`dt*100/500`），与帧率无关；
  长暂停（`dt>500`）直接追上目标。

## 配置（[MOExperience]，改后重启）

- `HoverMinOpacity=5`：悬停最低透明度 0~100（找触发区用）；0=沿用渐隐。
  启动横幅 `HovMinOp=` 回显。

## 教训

- 体感时间一律现实时钟（`GetTickCount`）：游戏 tick 是帧计数，
  拿它做 500ms 防抖等于焊死大门（0.2.1 的坑）。
- 淡出的是内容不是透明度：`ignoreTimeout` 切"只看活消息"后，
  无活消息时 `DrawBox` 返回 0——斜坡走满画的也是空盒。淡出必须锁存展开。
- 展开无需防抖：透明度斜坡已保证无硬闪，历史随底板一起淡入；
  多余的门反而挡了找区（0.2.4→0.2.5 的回退）。

## 清理（0.2.9）

- 删周期 `dbg:` 遥测块（12 字段，每 300 tick 一行）。
  日志只剩：transition（`camp: show/hide`、`hint:`）+ `stat:`（1800 tick
  一行健康计数）+ `vbox:`/`green:` 一次性 + `focus:`/`clock:` 罕见事件。
  `camp:` 翻转行自带鼠标坐标，定位够用。
- 删无用尝试：`src/Probe.cpp` + `构建探针.bat`（阶段 1 探针，
  其构建脚本自述"探针阶段已结束"；正式构建从未编它，git 历史可恢复）。

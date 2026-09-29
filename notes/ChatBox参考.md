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

## 自定义输入框的落位与原生行隐藏（0.2.149f 定案）

`MessageListClass::Draw`（`0x5D49A0`，`this` 恒为 `0xA8BC60`，探针实测）：

```
0x5D49A0  sub esp,0x24 / push esi / mov esi,ecx
0x5D49B4  mov al,[esi+0x19]     ; IsEdit
0x5D49B9  je  0x5D4A8D           ; 不在编辑 → 整段跳过
0x5D49BF..0x5D4A8C              ; Lock 临时面 / label->v[0x6C](1) /
                                ;   0x4A60D0 + 0x433ED0 测量 /
                                ;   压 8 参数 call 0x4A61C0 画原生行 / Unlock
0x5D4A8D  mov ecx,[esi] / pop esi / 逐条 v[0x2C](1) 显示消息 label
0x5D4A94  ; MessageDrawHook (0x5D4A94) 在此之后
0x5D4A9B  add esp,0x24 / ret
```

**隐藏原生行**：钩 `0x5D49B4`（`8A 46 19`，3 字节无 rel）=
`ChatEdit_HideNative`，判 `[this+0x19]` 为真时返回 `0x5D4A8D`。跳过的
`0x5D49B4..0x5D4A8C` 里 push 与 pop/`add esp` 全部成对（`push edi`..`pop edi`、
Lock..Unlock、8 压栈..`add esp,0x20`），**栈零失衡**；落点就是引擎自己
"不在编辑"分支的落点（引擎绝大部分时间走它）。不改任何原生控制流。

**踩过的坑（0.2.149b~e）**：钩装在 `0x5D4A1E`（2 字节）和 `0x5D4A1B`
（3 字节）两版，日志实测 **0 次被调**，而同函数 `0x5D49A0`/`0x5D49A94` 的
钩都调得到、探针还确认 `this=0xA8BC60` 且 `IsEdit=1` —— 即分支根本没走、
是**那两个地址的钩没装上**。所以钩只放在 `0x5D49B4` 这一个验证过的位置。
教训：`.inj` 加钩后必须用一次性日志确认它真被调到，不能靠"代码看着对"。

**原生行的位置**：`EditPos`（`+0x1C/+0x20`）= **(3,0)**，`fieldW`（`+0x2C4`）
= **整屏宽**。`AddEdit`（`0x5D4210`，`0x5D42FF` 起）把 EditPos 拷进编辑
label 的 X/Y（`+0x24` 的 `+0x0C/+0x10`），Draw 只读 label 的 X/Y。
实测**写 label X/Y 搬不动原生行**，所以别再试那套。

**落位**（`ChatAnchor_CampBelow`，`MOExperience.cpp`）：

- 底部模式（`CampBottom=1`）→ 贴战役框下沿（`surfH-CampBottomMargin+gap`），
  落在留出的底部间距里，位置不随消息条数变。
- 顶部模式 → 返回 0，盒子留在原生行原位（不挪）。此时 `ce_AtNative=1`，
  **禁止**钳屏内位移，一挪就和原生位分家。
- 底板只按画出的内容宽。**不要**按 `fieldW` 铺满：那是整屏宽，铺出来就是
  屏幕顶上留一条黑杠（试过，很难看）。

**叠序**：自绘盒子**不能**挂在 `0x5D4A8D`（`ChatEdit_DrawTail`，已撤）——
那里在 `MessageDrawHook` 之前，战役框会画在输入框之上。盒子改在
`MOExperience_MessageDrawHook` 尾部调 `ChatEdit_Frame` 时画：调用点在
战役框/提示框/超武面板/橡皮线之后，输入框压在最上层。



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

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

## MessageListClass 字段布局（`0xA8BC60`，`sizeof==0x149C` 与 YRpp 头文件逐字段吻合）

```
0x0000  MessageList*     0x0004  MessagePos (X,Y)   0x000C  MaxMessageCount
0x0010  MaxCharacters    0x0014  Height             0x0018  EnableOverflow(bool)
0x0019  IsEdit (bool)    0x001A  AdjustEdit (bool)   0x001C  EditPos (X,Y)
0x0024  EditLabel*       0x0028  EditBuffer[162]    0x016C  OverflowBuffer[162]
0x02B0  EditCurrentPos   0x02B4  EditInitialPos     0x02B8  CursorCharacter(wchar)
0x02BC  OverflowStart    0x02C0  OverflowEnd        0x02C4  Width
0x02C8  MessageBuffers[14][162]                   0x1480  BufferAvail[14]
```

`+0x2B8` 是 **`CursorCharacter`（光标字符）**，不是"要不要画"的开关。
引擎自己的 `AddEdit` 调用点（`0x55E5A7 push edi`）在单机下传 0，
所以 `0x5D49FA` 那道 `je` 会跳走。**我曾把它当成标志位、写它想"打开"
显示，连崩三次**——见下文"越界钩连崩三次"。

## 自定义输入框的落位与原生行隐藏（定案）

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

**原生行总闸**：钩 `0x5D49B4`（3 字节）= `ChatEdit_HideNative`。覆盖的是
`0x5D49B4..0x5D49B6`（`mov al,[esi+0x19]`），落点只有两个，**都是引擎原生走法**：

| 条件 | 落点 | 效果 |
| --- | --- | --- |
| 不在编辑 | `0x5D4A8D` | 引擎原生"跳过"路径（该处 push/pop 全成对，栈零失衡） |
| 在编辑 + `ChatEdit=0` | `0x5D49BF` | 引擎原生"进入"路径，**照常画原生行** |
| 在编辑 + `ChatEdit=1` | `0x5D4A8D` | 跳过，自绘盒子顶上 |

**这个钩永远不返回 0**，这一点是要害，见下面踩坑记录。

**原生行的位置**：`EditPos`（`+0x1C/+0x20`）= **(3,0)**，`fieldW`（`+0x2C4`）
= **整屏宽**。`AddEdit`（`0x5D4210`，`0x5D42FF` 起）把 EditPos 拷进编辑
label 的 X/Y（`+0x24` 的 `+0x0C/+0x10`），Draw 只读 label 的 X/Y。
实测**写 label X/Y 搬不动原生行**，所以别再试那套。

**绝对不要碰 `0x5D4A22` 的打印段**：单机下 `[+0x2B8] CursorCharacter==0`，
引擎在 `0x5D49FA cmp word [esi+0x2B8],0 / je 0x5D4A81` 就跳走了，
**那段永远不执行**。`0x5D4A22` 要用 `[0x887314]` 那个面和 `0x4A61C0` 的
8 个参数，单机路径上从没初始化过，强行进入必崩。

**落位**（`MsgText_ChatAnchor`，`MsgText.cpp`）：

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

### 教训：3 字节钩不能用 `return 0` 回放

`0x5D49B4` 处只有 3 字节 (`mov al,[esi+0x19]`)，盖不下 Syringe 的 5 字节跳转 ⇒
`return 0`（回放被覆盖的原字节）根本不成立：回放落回 `0x5D49B7` 时那里已是跳转
指令的位移字节，歪跳 `0x5D4A8D` ⇒ 原生输入框永远不显示。

之所以"钩能用"结论一路绿灯：`ChatEdit=1` 走显式落点，从不执行回放，只有
`ChatEdit=0` 才第一次走回放路径。
⇒ **永不 `return 0`，自己读 `[this+0x19]` 判 `IsEdit`，两向都给显式落点。**
⇒ 通用教训：**先做"摘钩对照实验"确认基线，再动手改**。

### 教训：越界钩连崩三次

| 钩 | 越到 | 崩因 |
| --- | --- | --- |
| `0x5D49FA` 6B | `0x5D4A00` | 连带跳过 `0x5D49F8 mov edi,eax`，`0x5D4A37 add edx,edi` 拿野值当 X 偏移 |
| `0x5D4A1B` 3B | `0x5D4A22` | 进了单机永不执行的打印段，`[0x887314]` 等参数未初始化 |
| 写 `[+0x2B8]` | — | 同上，等于把控制流顶进打印段 |

后两个同一根因：单机 `CursorCharacter==0`，引擎在 `0x5D49FA` 就跳走，
`0x5D4A22` 那段本不该被执行。
⇒ **改控制流前先确认目标路径在目标场景下本来会不会被走到**；引擎自己都不走的路
= 上下文未初始化 = 硬闯必崩。

### 教训：`.inj` 脱节

`dist/MOExperience.dll.inj` 曾只能靠脚本增删行，反复增删导致同符号两行钩子
(`0x5D49B4 =…, 3` 与 `0x5D49B7 =…, 8`) 区间互相踩 ⇒ 崩。
⇒ 已纳入 git（GBK，`-text` 防行尾转换）；改钩后跑**重叠检查**（同区间内两两比对首尾）。
⇒ `build-fast.bat` 每次构建都用 `dist` 覆盖游戏目录那份 ⇒ 实验改 `dist`，以构建后
的游戏目录为准。

### 导航键：不用全局键盘钩

### 导航键：不用全局键盘钩

曾用 `WH_KEYBOARD_LL` 消息级吞 `Delete`/方向键防滚图。**副作用**：钩子回调
只在安装线程抽消息时才跑，游戏在后台时主线程不抽消息，按键就卡在系统里
等回调 → 别的程序拿不到 `Delete`/方向键（用户表现为在外部编辑器里删不动
字、光标移不动），且**关掉聊天框后就正常**（= 卸钩 = 系统不再等）。

**已整体删除** `CE_LLProc`/`CE_LLOn`/`CE_LLOff`/`CE_KeyScope` 及三个计数状态。
这六个键本来就有完整的按帧轮询实现（边缘立刻一步、按住 400ms 后 33ms 连发），
现在它是唯一来源，门用 `CE_GameForeground()`（`GetAsyncKeyState` 是全局状态，
不加门就会在用户切走的程序背后照删）。

**代价**：方向键不再被吞，打字时镜头可能跟着动一下。拿可见小瑕疵换掉
"抢全系统键盘"。要彻底消掉得再钩游戏自己的方向键处理。



# 战役框显隐（淡入淡出 + 悬停展开）

调用链：`MOExperience_MessageDrawHook` 每帧算悬停/活消息 → `camp.fade`（0–100）
→ `if (showCamp) DrawBox`；`DrawBox` 内底板/阴影/文字/滚动条透明度等比缩放，
`fade=100` 与原来逐像素一致。

## 状态机

- 目标三档：活消息=100；区内驻留（静止 300ms，±2 像素防抖）=100；
  区内移动=托底（`HoverMinOpacity`，找区用）；离开=0。
- 活消息即时满亮（治时差）：`liveCamp` 直接 `s_fade=100`，
  新消息与引擎提示音同帧出现（原版首字同帧），不再爬 500ms 斜坡；
  淡出与悬停淡入仍走斜坡。
- 底板两态（Phobos hover 态）：平时逐行包裹已揭示文本
  （黑底只包墨水，左右 3/上下 2，与整块同边距）；悬停/展开铺整块
  （`camp.fullBoard=s_expand`）。透明度都走 `BoardOpacity`。
- 打字机：揭示位=`ClockNow-born`（1 字/tick≈16ms，原版速率），
  首帧至少 1 字；行偏移 `g_LineOff`，未揭示行留空占位；`CampAnim=0` 关。
  提示框保持整行即时（原版 sp=1 无动画同）。
- 全显锁存：已到 100 且鼠标还在区上，区内移动不再压暗，离开才淡出。
- 展开锁存（滚动条 + 全部历史）：悬停即展开；淡出中保持上帧（hover 离开后
  历史可见淡出，否则淡出的是空盒、看起来像瞬灭）；`fade` 到 0 才收回；
  活消息独占时仍只看活的。锁存只保持不新设：末条过期时上帧是
  收拢态，保持收拢直接消失（原版行为）；之前新设展开会导致闪出整块历史
  再整体消失。
- 斜坡：全程 500ms，步进按流逝毫秒算（`dt*100/500`），与帧率无关；
  长暂停（`dt>500`）直接追上目标。

## 配置（[ChatEdit] Enable/FontFace/Bold 与 [MsgText] ChatBoxSP，改后重启）

- `HoverMinOpacity=5`：悬停最低透明度 0~100（找触发区用）；0=沿用渐隐。
  启动横幅 `HovMinOp=` 回显。

## 教训

- 体感时间一律现实时钟（`GetTickCount`）：游戏 tick 是帧计数，
  拿它做 500ms 防抖等于焊死大门。
- 淡出的是内容不是透明度：`ignoreTimeout` 切"只看活消息"后，
  无活消息时 `DrawBox` 返回 0——斜坡走满画的也是空盒。淡出必须锁存展开。
- 展开无需防抖：透明度斜坡已保证无硬闪，历史随底板一起淡入；
  多余的门反而挡了找区。

## 清理

- 删周期 `dbg:` 遥测块（12 字段，每 300 tick 一行）。
  日志只剩：transition（`camp: show/hide`、`hint:`）+ `stat:`（1800 tick
  一行健康计数）+ `vbox:`/`green:` 一次性 + `focus:`/`clock:` 罕见事件。
  `camp:` 翻转行自带鼠标坐标，定位够用。
- 删无用尝试：`src/Probe.cpp` + `构建探针.bat`（阶段 1 探针，
  其构建脚本自述"探针阶段已结束"；正式构建从未编它，git 历史可恢复）。

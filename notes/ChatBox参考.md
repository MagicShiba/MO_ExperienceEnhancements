# ChatBox 参考（最小入库）

上游：`https://github.com/moon-bamboo/ChatBox/tree/master`（`0.3.8`，`gamemd.exe` 尤里复仇 `1.001`）。
只记录钩点与调用，不入库第三方源码全文。用前用反汇编核对地址是否适用 `G:\motest\gamemd.exe`。

钩子：

- `0x5D3BA0`（6B）：`MessageListClass::AddMessage` 入口，捕获全部消息。
- `0x5D4A94`（7B）：`MessageListClass::Draw` 内画消息列表处；接管返回 `0x5D4A9B`。
- `0x777998`（6B）：滚轮方向。
- `0x533F50`（5B）：悬停消息框时跳过侧栏滚动。
- `0x692419`（7B）：命中消息框时吞掉穿透点击。

单人聊天框（Phobos `AllowChatBoxInSinglePlayer` 同原理，`ChatBoxSP`，`0`=走原版）：
对话框函数按 `[SessionClass 0xA8B238]==0/5` 判单人并跳过聊天框四处逻辑，开启时跳过 session 检查（其余旗标保留）：

- `0x55E477`（5B）→ `0x55E48D`；`0x55E62F`（6B）→ `0x55E637`；
  `0x55E693`（6B）→ `0x55E69B`；`0x55E746`（5B）→ `0x55E77B`。

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

## MessageListClass 字段布局（`0xA8BC60`，`sizeof==0x149C`，与 YRpp 头文件逐字段吻合）

```
0x0000  MessageList*     0x0004  MessagePos (X,Y)   0x000C  MaxMessageCount
0x0010  MaxCharacters    0x0014  Height             0x0018  EnableOverflow(bool)
0x0019  IsEdit (bool)    0x001A  AdjustEdit (bool)   0x001C  EditPos (X,Y)
0x0024  EditLabel*       0x0028  EditBuffer[162]    0x016C  OverflowBuffer[162]
0x02B0  EditCurrentPos   0x02B4  EditInitialPos     0x02B8  CursorCharacter(wchar)
0x02BC  OverflowStart    0x02C0  OverflowEnd        0x02C4  Width
0x02C8  MessageBuffers[14][162]                   0x1480  BufferAvail[14]
```

`+0x2B8` 是 `CursorCharacter`（光标字符），不是显隐开关；单机 `AddEdit` 传 0。

## 自定义输入框落位与原生行隐藏（定案）

`MessageListClass::Draw`（`0x5D49A0`，`this` 恒为 `0xA8BC60`）：

```
0x5D49A0  sub esp,0x24 / push esi / mov esi,ecx
0x5D49B4  mov al,[esi+0x19]     ; IsEdit
0x5D49B9  je  0x5D4A8D           ; 不在编辑 → 整段跳过
0x5D49BF..0x5D4A8C              ; Lock 临时面 / 显示 label / 测量 /
                                ;   压 8 参数 call 0x4A61C0 画原生行 / Unlock
0x5D4A8D  mov ecx,[esi] / pop esi / 逐条显示消息 label
0x5D4A94  ; MessageDrawHook 在此之后
0x5D4A9B  add esp,0x24 / ret
```

**原生行隐藏**：钩 `0x5D49B4`（3B）= `ChatEdit_HideNative`，落点只给两处引擎原生走法：

| 条件 | 落点 | 效果 |
| --- | --- | --- |
| 不在编辑 | `0x5D4A8D` | 引擎原生跳过路径（栈零失衡） |
| 在编辑 + `ChatEdit=0` | `0x5D49BF` | 引擎原生进入路径，照常画原生行 |
| 在编辑 + `ChatEdit=1` | `0x5D4A8D` | 跳过，自绘盒子顶上 |

**永不返回 0**：3B 装不下 Syringe 5B 跳转，回放落到跳转位移字节上歪跳，原生行整段不画。自己读 `[this+0x19]` 判 `IsEdit`，两向显式落点。

**禁区**：单机 `CursorCharacter==0`，引擎在 `0x5D49FA` 即跳走，`0x5D4A22` 打印段上下文未初始化——勿钩它、勿写 `[+0x2B8]`。改控制流前先确认目标路径在目标场景下本来会被走到；先做摘钩对照实验确认基线。

**原生行位置**：`EditPos`（`+0x1C/+0x20`）= (3,0)，`fieldW`（`+0x2C4`）= 整屏宽；`AddEdit`（`0x5D4210`）把 EditPos 拷进 label X/Y，Draw 只读 label。写 label X/Y 搬不动原生行。

**落位**（`MsgText_ChatAnchor`）：底部模式贴战役框下沿（位置不随消息条数变）；顶部模式盒子留原生行原位（`ce_AtNative=1`，禁止钳屏内位移）；底板只包画出内容宽，不按 `fieldW` 铺满（整屏宽，铺出来是顶上黑杠）。

**叠序**：盒子在 `MOExperience_MessageDrawHook` 尾部画（战役框/提示框/超武面板之后），压在最上层；勿挂 `0x5D4A8D`（在战役框之前，会被压住）。

## 输入法（定案）

- 组合窗 `CFS_FORCE_POSITION` 指盒底上一行（隐藏了，只做堆叠锚点）+ 候选窗 `CFS_CANDIDATEPOS` 指盒底（surface→**呈现区**换算，见下“IME 坐标基准”）；组合窗用 `CFS_FORCE_POSITION`（`CFS_POINT` 允许 IME 调整位置）。组字前候选听 `CANDIDATEPOS`（贴底）；组字后 IME 按“组合窗下再空出一行”自己重排（覆盖候选窗设置），组合窗抬过一行后正好落回盒底。`CFS_EXCLUDE` 试过：组字开始后按不住，被 IME 重排覆盖，不用。组合行高读 `ImmGetCompositionFont` 实测（失败回退自家字高换算，一局内缓存）。
- 系统组合窗由主窗子类化隐藏：截 `WM_IME_SETCONTEXT` 清 `ISC_SHOWUICOMPOSITIONWINDOW`，吃掉 `WM_IME_STARTCOMPOSITION`（不转交）；候选窗（`WM_IME_NOTIFY`）与输入（`COMPOSITION/CHAR`）原样转交。
- 结论：压组合字高 / `CFS_HIDDEN(0x2000)` 依赖输入法配合，不可靠，不用。
- 组字串游戏内自画（黄色+下划线），`Imm` 轮询 `GCS_COMPSTR`。

### IME 坐标基准：呈现区 ≠ 客户区（`CE_MeasureView`）

- IMM 位置是主窗客户区坐标，盒子画在渲染面上，按呈现区/渲染面比例换算；全屏上采样时呈现区=显示器，窗口化=客户区。
- 模式只能问包装层 `DDIsWindowed`（1=窗口化/0=全屏），每次开框重查；窗口几何、样式、引擎旗标区分不出两种模式。查不到时按窗口化。
- 分母：窗口化=客户区；全屏=所在显示器（要求客户区原点在屏幕原点，否则仍按客户区）。
- 仍偏则用 `[MsgText] ImeFullscreen` 手动指定（0=客户区，1=显示器；旧版 cnc-ddraw 无此导出时用 1）。
- 基准变化才作废位置缓存并记一行 `chatedit: view=WxH client=WxH mon|win`；量不到沿用上次。`CE_CompLineH` 回退换算共用同一呈现区高。

## 导航键

不用全局键盘钩（`WH_KEYBOARD_LL` 回调只在安装线程抽消息时跑，后台按键会卡住别的程序取键）。按帧 `GetAsyncKeyState` 轮询（边缘一步，按住 400ms 后 33ms 连发），门用前台判定；代价是打字时方向键镜头可能动一下。

- 输入历史（纯输入框本地，与联机无关）：回车分支记 + 关框补记（单机对话框提交不走 `Input`，`Esc` 取消不记），两处双记由“相邻重复不记”合并，32 行；开框时上/下翻（`Up` 更旧/`Down` 更新，到底恢复翻前行）；改字（`Insert/Delete`）即离翻，光标移动不离翻。

## 战役框显隐（聊天内容主显示区，淡入淡出 + 悬停展开）

调用链：`MOExperience_MessageDrawHook` 每帧算悬停/活消息 → `camp.fade`（0–100）→ `if (showCamp) DrawBox`；底板/阴影/文字/滚动条透明度等比缩放，`fade=100` 与原来逐像素一致。

状态机：

- 目标三档：活消息=100；区内驻留（静止 300ms，±2 像素防抖）=100；区内移动=托底（`HoverMinOpacity`，找区用）；离开=0。
- 活消息即时满亮：`liveCamp` 直接满，新消息与引擎提示音同帧出现；淡出与悬停淡入走 500ms 斜坡（步进按流逝毫秒，与帧率无关；`dt>500` 直接追上）。
- 底板两态：平时逐行包裹已揭示文本（黑底只包墨水）；悬停/展开铺整块。透明度都走 `BoardOpacity`。
- 打字机：揭示位按现实时钟推进（原版速率，首帧至少 1 字），未揭示行留空占位；`CampAnim=0` 关；提示框保持整行即时。
- 锁存：全显后区内移动不再压暗，离开才淡出；悬停即展开，淡出中保持上帧（否则淡出的是空盒），`fade` 到 0 才收回；锁存只保持不新设（末条过期时上帧收拢则直接消失）。

结论：体感时间一律现实时钟（游戏 tick 是帧计数）；淡出的是内容不是透明度（无活消息时 `DrawBox` 返回 0，须锁存展开）；展开无需防抖（斜坡已保证无硬闪）。

配置（`[MsgText]`，改后重启）：`Enable/FontFace/FontHeight/Bold`（输入框同套字），`ChatEdit/ChatBoxSP/ChatNameSP/ImeFullscreen`（输入法定位基准，见上），`CampBottom/CampBottomMargin`（输入框落位），`HoverMinOpacity=5`（悬停最低透明度，0=沿用渐隐）。

## 性能 (开输入框掉帧)

位置/状态不变不重设; 每帧建 GDI/IMM 对象先问是否与上次相同。

1. `CE_CompLineH` 回退值也缓存 (键=行高/客户区高/游戏高)。
2. `CE_PositionIME` 缓存 (cx, cy, 组合窗 y), 不变不碰 IMM。
3. HIMC 一帧一个 (`ce_FrameImc`), 帧末释放; 候选窗行高须用真 HIMC 量。
4. 逐字宽度走 `GtCharAdv` 记忆表, 不逐字问 GDI。

缓存变量名避开 Windows 短宏 (如 `s_lh` 会展开, 编译报错无指向)。

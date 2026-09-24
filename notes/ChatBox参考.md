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

单人聊天框（Phobos `AllowChatBoxInSinglePlayer` 同原理，已在 `MsgFont.cpp`
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

# VecText 全局文本矢量接管（结论）

单 DLL 多文件中的一份（与 MOExperience 聊天框同 DLL）。只替换字形光栅，
度量 / 换行 / 对齐 / 版式全部走引擎原版。

## Enable 的边界

`[VecText] Enable` 只门控文本五钩：Blit 0x434120 / DrawText 0x434CD0 /
Print 0x434B90 / Simple 0x4A5EB0 / GetDim 0x433CF0。

`VecText_Install()` 同时装全 DLL 共用钩（缩放/底电/鼠标等），与文本无关，
不跟 Enable 一起关。钩归属与 ini 开关分开。

## 性能

`Enable=1` 从 300 帧掉到 210 帧（约 +1.4ms/帧），主项与修法：

1. **一槽一字号**。`CreditPx` 与 `FontPx` 混槽会逐字互拆重建。`VecGdiSlot` 分槽 2/8；`GDI_SLOTS` 扩到 9。
2. **只清所需 DIB**。`ClearDibRect` 替代整幅清。
3. **行光栅缓存**（`ROWC`）：键=槽+串+代号，命中不调 GDI；容量 64。
4. **混合查表**。`g_ExpR/G/B` + 头文件内联 `Blend16`；`a==255` 短路；纯黑走 `BlendDark`。压位按比例 `(v*N+127)/255`，不是移位。
5. **ABC 记忆表**（`GtCharAdv`）。槽重建键值一起清。
6. **引擎度量记忆**。键 (t, idata, wch, 行高)；行高须进键。
7. **整串记忆**（`g_DimC`）。键含 `maxW`；只存接管成功的。
8. **字形缓存 2047 槽** + 逐行墨水区间。
9. **字体键存指针**，不逐字跑字符串比较。
10. **阴影单遍合成**（`BlitCoverageShadow`）：各遍保留系数连乘，每像素混一次。

性能账（`Perf=1`）：每 2s 记 `perf: ms= glyph= miss= gdHit= gdMiss= gdUs= gpus=`。逐字只数不计时。`miss` 超 `glyph` 5% 查字形缓存；`gdMiss` 大查串记忆。

## 接管点（gamemd.exe，部署前核对）

- BitFont::Blit = 0x434120，头 7B，ret 0x10，advance = 返回值 - X。
  JMP 钩 + 跳板，异常回落原版。
- BitFont::DrawText = 0x434CD0，thiscall + 10 栈参，ret 0x28。
  参数：（p1, p2, str, X, Y, W, H, flags, maxRows, maxH），字符串在第 3 槽。
- BitFont::Print = 0x434B90，thiscall + 7 栈参，ret 0x1c，字符串也在第 3 槽。
- GetTextDimension = 0x433CF0（只读度量，不钩）。
- BitFont 对象布局：+0x04 InternalData，+0x0C 锁定像素缓冲，
  +0x10 pitch（WORD 数），+0x24 默认色，+0x30..+0x3C 裁剪 LTRB。
- InternalData + 0x0C = Lines（当前 fnt 为 19）。

- 摆位：整体居中 dy = y + (h - 窗高)/2；字间相对位置保留 GDI，不逐字居中。
- 横向全墨水不压缩，溢出由面界收尾；只认面界不认引擎裁剪；步进按 GDI 步进只加宽不收窄（无则墨水右缘），简报盒内保持原笔位（数字除外）。
- FontPx > 格高必溢出裁剪；字号按格高选（0=自动）。
- 键 = (idata, 字体名, 粗体)。

## 超武列表整体接管（Simple_ 入口隐藏 + [SWPanel] 重绘）

- Simple_ 入口（0x4A5EB0）：EDX=文本，a1=画布，a2/a3 和=落笔，a4=前景色。命中即收表并跳过；空串只收表。Print 层无色，只能在此取。
- 命中：主画布 + 横向带 + CJK 首字/前 8 字冒号；150ms 过期。
- 归并键 (y,kind)，x 跟随漂移。
- 版式见 [SWPanel] Classic；DrawText 计时器走原版；Print 只收冒号后备。

## 菜单按钮整体替换（MenuTake=1）

- DrawText caller=0x621144，flags=5，123x24/121x20。入口只收串，首个 Blit 居中起笔；原字形不画，失败回原版。
- 横向只认面界，纵向与常规一致；巨墨水回原版。

## 悬浮提示背景盒（已移除）

半透明取样失真 + 自盲 + 误收列表，重开须同解三题。取证：悬浮经 Print (caller=0x4A5FF2)；前 16 字含 '=' 非列表串。

## GetDim 宽度接管（0x433CF0）

只动单行适配宽：先走原版取 h，w 用 GDI 步进和。控制串/超长/触 maxW 走原版；重入守卫；Render 关不接管。

## 顶栏资金数字

同一函数会画多处文本，函数粒度分不开，只认 y<32 条带：条带数字吃 CreditPx/CreditDY。优先级 CreditPx > FontPx > 自动。发起者须过 call 校验。

## 简报正文（自绘，不改引擎）

- 正文 = 一次 DrawText (0x621144 → 0x434CD0)，盒 (652,313,453,432)。引擎排版绑死 fnt，只能整块自绘：行高跟 BriefLine，字距跟 BriefTrack，换行按墨水宽。
- 引擎照跑（绑面/Lock/裁剪），Blit 抢首字整块画掉并抑制后续逐字。面须借引擎的（进钩时 font+0x0C 尚空）。
- 逐字格宽 = 字体前进宽 + 字距；摆位 = abcA (`g_VecNat`)。
- 裁剪四边临时放开、画完还原；盒宽按 BriefWidth，行数只受屏高限。
- `BriefMark`：深灰双线 + 内缘 20px；裁剪比框外放 4px。
- 颜色 font+0x24；不可靠返回 -1 走引擎。

## 配置（[VecText]，改后重启）

- Enable=1，FontFace=文件名原样匹配（先游戏目录后系统目录），FontPx=0（自动），Bold=0，VShift=0。
- CreditPx/CreditDY（顶栏）；BriefOwn/BriefLine/BriefTrack/BriefWidth/BriefAdv/BriefMark（简报）；LogDetail/Render；电力四键见下。

## 已排除（勿重查）

- DrawText 改 t+0x1C 无效：t 是画布，字体是 a1。
- IsPlausiblePtr 要求 4 字节对齐，字节级探测先对齐。
- 正文无体外循环，都经 DrawText。
- 已删键勿加回：`LogAdv`、`FixLineH`、`BriefBox`/`BriefClip`。

## 返回值哨兵

成功返回新笔位（含合法负值），真失败返回 `VC_FAIL=-1000000`，调用方判 `r != VC_FAIL`。先查是否误伤负值，再查失败点。

## 日志

`vtext:` 钩子成败、`brief: dt/own draw/w/skip why=`、`perf:`、`gdi:` (LogDetail=1)。探针定案即删。

## 右侧提示与超武区分

调用链：`Simple_` → `Simple_+0x142` 调 `Print` → 逐字 `Blit`；建筑提示另经 `DrawText 0x479041` 小矩形。

门全按路径/发起者：`Simple_` 收 `0x4A6345`（Fancy 内部调 Simple_ 的返回点，所有 Fancy 文本都经此，名单另靠横向带+形状过滤）；`Print` 只收冒号计时器；`Blit` 只认 DrawText 路径并豁免提示子树；`VecRender` 做坐标归一与空裁剪回退。

Ares FPS 计数器（`"FPS: N Avg: F"`，0x4F4583 定死 x=0、y=底-32，Render 尾最后画）含冒号但永不收表（`VecSWIsFps` 前缀排除；超武计时器从不以 `FPS:` 开头）。它到 `Simple_` 有两条路（Fancy 内转返回点 0x4A6345 / 新版 YRpp 直调返回点在 Ares.dll 内），收串只认内容不认调用者。

FPS 钉住（任何档位 1x、位置固定）：`Simple_` 入口收串并跳过原绘制 → Render 尾新钩（0x4F4589，Ares 之后 5B）以固定 16px（GDI 9 号槽）重画到固定位（x=0，底上 32，与 Ares 同锚点）。Ares 的黑底盒走度量（0x4A59E0 经 GetDim），计数器串宽度报 0 直接消失。只画窗口面，大面跳过；`Render=0` 或钉住钩没装上时不收，看原版。

## 电力提示盒

- 两处一律要求 caller == 0x4A5FF2。
- 出界填不透黑，不做映射填充；边框色粘存沿用。
- TipBox 只服务电串。

## 底部电量

- 侧栏穹顶“闪电+负载/电力”（PowerMode=1 为余量数字），y 锚底边；侧栏面与主面双份同像素。
- 闪电 10x16 二值直写；数值取 CurrentPlayer+0x53A4/0x53A8；颜色 d*4<o*3 绿，d<=o 黄，否则红。
- 字号/字体与资金同源；值变才重光栅。
- 四路同画：Message::Draw、prepare 0x4F42F0、BlitSidebar 0x6A70E0、UpdatePrimarySurface 0x4F4780。naked 只许 "m" 操作数。
- 上屏唯 BlitSidebar 内 CopyFromPart；禁每帧调 BlitSidebar（光标拖影）。

# 目标与路线：消息队列矢量字体

范围：只替换 ChatBox 消息框里的文字光栅化；消息捕获、过滤、滚动、点击照旧。

选定路线：引擎内 GDI 混合。

1. 沿用绘制接管点 `0x5D4A94`，跳过原版消息列表绘制。
2. 可见行用 `Microsoft YaHei` 经 GDI 光栅化到内存 32 位 DIB。
3. 用目标色（来自 `ColorScheme`）按 alpha 混入 `DSurface::Composite`。
4. 折行宽度改用 GDI 度量，避免与点阵字宽对不齐。

动工前必须先探针验证：

- 本地 `gamemd.exe` 是否与 ChatBox 目标版本一致（地址是否可用）。
- `Composite` 的 `BytesPerPixel`、`Pitch`、`Lock/Unlock` 行为。
- `ColorScheme` 取色结果（`BaseColor@0x308` 或 `0x517440`）。

风险：全屏独占下覆盖窗口不可靠，所以不选覆盖窗口；GDI 抗锯齿在透明混合时用灰度 alpha，不要用 ClearType。


# VecText 全局文本矢量接管（结论）

单 DLL 多文件中的一份（与 MOExperience 聊天框同 DLL）。只替换字形光栅，
度量 / 换行 / 对齐 / 版式全部走引擎原版。

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

## 字号校准

- 不测字体文件（改版字体墨水盒本身可能是错的，照抄等于抄错）。
  原版哲学：每个字在格内垂直居中，与字体具体多高无关。
- 摆位公式（整体居中）：dy = y + (h - 窗高)/2，全字统一。
  逐字居中是错的：贴底标点会被拽到格中央飞出格子（边缘显示不全），
  且破坏字间相对位置。字间排版（基线/贴底/贴顶）原样保留 GDI 的。
- 横向：全墨水不压缩（紧墨水盒原样端上，溢出格子由面界收尾，与 GDI 自然
  overhang 一致；裁边 / 重采样 / 滤波已全部删除）。
  横向只认面界（0..pitchW），不认引擎裁剪；纵向摆位 / 裁剪不变；
  笔位 / adv 走引擎，但步进按 GDI 自然步进（ABC 全宽）加宽（只加宽不收窄，
  无则按墨水右缘 fullW）：引擎窄格配宽墨水时按原 adv 会前后重叠。
  例外：简报正文盒内（DrawText 宽≥400 且高≥100，引擎按自有宽度换行，
  加宽会撑出盒子）保持原笔位，数字除外。
  数字步进另吃 [VecText] DigitTrack 微调（负=收紧，范围-8~8，只影响 0-9）。
  菜单整行自有版式时步进同理（GDI 自然步进优先），字间自带字体边距。
- FontPx 大于格高必然溢出裁剪（物理限制）：如 19px 墨水进 16 格，
  居中也只能对称裁。字号请按格高选（自动=跟格）。
- 简报是唯一例外：步进由 BriefLine 单独放开（见“简报正文”节），
  盒子与裁剪底同步放高，长文本装得下。
- 真寻址（Blit 反汇编实证，备用知识）：tbl=idata+0x18（指针），
  no=u16[tbl+wch*2]，rec=*(idata+0x1C)+(no-1)*(*(idata+0x14))。
- 键 =（idata， 字体名， 粗体），字体重载自动重算。
- 当前 game.fnt 为 19 高改版（原版 16 高下无简报问题）。
- FontPx > 0 为全局固定字号：墨水与字宽等比缩放（advance 跟字号走），
  简报行步进同样跟字号；行距 / 换行仍是引擎的，偏离原版太多可能溢出盒子。
  默认 0 = 自动校准。
- 高度不压缩：输出自然高度 outH 行、与 DIB 1:1，降部超出格高如实画、
  由表面裁剪收尾。

## 超武列表整体接管（Simple_ 入口隐藏 + [SWPanel] 整块重绘）

- 收集点在 Simple_Text_Print_Wide 入口（0x4A5EB0，Print 唯一调用者）：
  背景黑条是 Simple_ 内部按标志（ah&4，名行 flags=0x4618）铺的不透明底，
  与字形同一调用，入口跳过即连底+字一起消失。
- 槽位（fastcall：ECX+EDX+7 栈参，ret 0x1c）：
  ECX=pRetVal，EDX=文本，a1=画布，a2/a3=矩形+坐标（和=落笔 XY），
  a4=前景色（名 0x07FF/计时器白），a5=背景（黑），a6=标志，a7=1。
  Print 层无颜色（a6=a7=0），颜色只能在此取。
- 命中（主画布 + 和坐标落 1300~1752/32~1080 + CJK 首字/前 8 字冒号）即收表
  （串，坐标，原前景色）并跳过（返 ECX，与原版同口径）；空串只收表不跳过。
  面板用原前景色画全部新鲜行；150ms 未刷新即过期，引擎闪烁原样透出。
- 收集表同 y + x 容差 ±6 归并：引擎同行落笔 x 逐帧抖动约 ±3，
  精确匹配会涨满 32 项导致丢弃+过期闪烁；
  名/计时器同 y 差 27 天然分开。跟随漂移更新 x。
- 版式（[SWPanel] Classic）：0=统一面板（右下锚定单个底，名左计时器右）；
  1=原版式（名右对齐到计时器列，计时器右对齐到右缘；每行高=字高+上下 PadY，
  条四面留边（横向 PadX）无缝衔接，字永不出条；整块底部锚定 BottomMargin，
  引擎 y 只定上下顺序；Opacity 同管）。
- 不碰：DrawText 计时器（侧栏图标叠字，非列表，走原版）。
  Blit 层 SuperPx 接管已移除（游戏内置 0 对比无变化）：右侧区文字走通用 Render。
- 日志：config: SWPanel（加载）。
- Print 入口只收冒号计时器做后备；CJK 名串不在 Print 收（见“右侧提示战役总览”）。

## 菜单按钮整体替换（[VecText] MenuTake=1）

- 路径：DrawText caller=0x00621144
  （与简报正文同一调用者，再用标志+尺寸区分），flags=5，
  123x24（正文，如 1773,231 游戏控制/256 载入/281 储存/306 删除/331 放弃）
  / 121x20（阴影，+2,+2 偏移）。
- 入口时表面尚未锁定（t+0x0C 无效），故入口只收串+矩形；
  首个 Blit 建全串字形、算总宽、矩形内居中起笔，之后按实际墨水宽度
  走笔（空格按引擎 adv，只加宽不收窄），引擎 x 不用。
  串序对不上回落引擎笔位；原字形一次不画，双双失败才走原版。
- 绘制：全墨水（不挤压不断笔画，溢出格子由面界收尾，与 GDI 自然
  overhang 一致），横向只认面界（0..pitchW），纵向摆位/裁剪与常规一致；
  颜色走引擎。巨墨水（>32）与失败回落原版（保底可读）。
- 同调用者的简报（盒 652,313 等，flags≠5）与其它尺寸一律走原版。

## 悬浮提示背景盒自适应（已移除：代码整机删除，取证保留）

- 关闭原因三条：侧栏生产项行底半透明（取样填充变死黑底，任何取样填充不许碰
  半透明 UI）；后帧取样撞自己上一帧的字（自盲）；提示飘进右侧带被当列表收走。
  重开须同时解三题，不另立项不碰。
- 取证：世界悬浮框经 Print（caller=0x4A5FF2 内层），outer=0x00707744；
  超武名 / 计时器从不含 '='（前 16 字含 '=' 即非列表串，两路收集同规则）。
- 若重开：只包墨水（生产项串带前导空格，行首起算盖图标）；起笔附近 4 点全同
  且暗（=引擎实色底）才拓盒（左 4/右 4/上下 2，取样色填充）；电串走 Tip 路
  （LastTipX/TipTot/Sticky），建筑名走 Tool 路；盒右界=a6+2，Simple 三重门
  （电力串+a4/a5/a7 签名+a6 自校验）+GetDim 宽度转发，盒引擎自画。

## GetDim 宽度接管（VecGetDimEntry，0x433CF0，6 字节钩）

- 引擎所有算字宽处（盒子 / 居中 / 步进 / 换行）全走 GetTextDimension，
  一处替换处处生效，不再逐个位置打补丁。
- 只动单行适配宽度：先走原版取 h，w 用 GDI 自然步进和
 （ABC 全宽 + 数字 DigitTrack，与绘制笔位同口径）。
- 控制串（\t \n 等）、超长（>255 字）、触 maxW（换行逻辑）一律走原版。
- 重入守卫 + 未开 Render 不接管；VecMeasure 走原版（笔位基准不动）。
- 头字节 gamemd 静态对过（83 EC 1C 8B 41 04），跳板回 0x433CF6。

## 顶栏资金数字

- 同一串数字至少 5 个发起者反复画，且坐标相同（x=63..96, y=2）：
  0x4D002E 所在函数 [0x4CFE80,0x4D0170]（按 House 遍历），
  0x4BADAC/0x4BAE49 所在函数 [0x4BAD80,0x4BAEB9]（数字格式化），
  另有 0x7BC1BF / 0x411648 / 0x437E23。
  函数粒度分不开（顶栏函数也会画别的文本，如按 House 遍历连超武列表一起画），
  故不用函数白名单，条带是唯一判据。
- 最终规则（Blit 层，与发起者无关）：
  y<32 条带上的数字统一吃 CreditPx/CreditDY（CreditPx=0 且 CreditDY=0 时不碰）。
- 字号优先级：条带 CreditPx（仅条带数字）> 全局 FontPx > 自动校准。
- 数字行 y=2，CreditDY 建议 -2（贴顶；用户现值 0；-5 会顶到 y=-3 切掉顶部）。
- 发起者判定须过 call 指令校验（栈上整数常落在代码段内，如 0x6D005C 是误报）。

## 简报正文（自绘，不改引擎）

- 正文 = 一次 DrawText，经 0x621144 → 0x434CD0，盒 (652,313,453,432)。
- 引擎那套排版三处度量全绑死在 fnt 上，互相牵制（这是不能只打补丁的原因）：
  - 行高 = 字体对象 +0x1C：`0x434EC2 mov eax,[ecx+0x1C] / add edx,eax`。
  - 字宽 = idata+0x1C：`0x4346C0 add eax,[ecx+0x1C]`
    （完整式 `idata->0x14*(宽表[ch]-1) + idata->0x1C`，逐字不同）。
  - 换行 = 盒宽内按字宽累加；切行 = 行高比盒高。
  - 症状：墨水按字号出（msyh 18px 墨水 ≈18px），行高按格高钉死（17px）
    → 行间零留白、上下行横切；字宽 16 而墨水 18 → 汉字首尾相接。
  - 临时改 a1+0x1C 行高（画完还原）治不了行距：字宽一动换行就跟着
    错，想加字距必然撑出 453 宽的面板盒子被切掉。
- 现在整个自绘（VecBriefBody），但**不跳过引擎**：引擎照跑（它负责绑面 /
  Lock / 裁剪 / 逐字调 Blit），在 Blit 钩里抢第一个字的位置把整块画掉，
  之后这一次的逐字绘制全抑制（`g_BriefState` 1=已接管 2=不可用不抑制）。
  - 关键坑：`0x4348F0`（装面，填 font+0x0C/+0x10/+0x30）在 DrawText
    **内部第 0x28 字节**处才调，进钩那一刻 font+0x0C 还是 NULL。自己绑面
    就得自己解，而面是 Lock 着的 ⇒ 借引擎自己的面，别自己来。
- 排版（与 GDI DrawText / DirectWrite / 浏览器一致）：
  - 行高 = 字号 × `BriefLine`%（130 → 18×1.3 = 23px），下限 fnt 格高。
  - 字距 = `BriefTrack` 加在字体前进宽**之后**（标准 letter-spacing）。
  - **逐字格宽 = 字体自己的前进宽**（`GtCharAdv` = GetCharABCWidths 的
    A+B+C）+ 字距。18px 雅黑：汉字/CJK 标点 18px、拉丁按字形比例、空格窄。
    统一格宽或引擎 fnt 宽度表都会让混排忽宽忽窄（fnt 那张表是 16×16
    点阵时代的产物，逐字不同）。
  - 摆位 = 字体自己的左边距 abcA（`g_VecNat`）。旧的"墨水不足格宽 2/3 就
    居中否则左对齐"靠 inkW 猜宽窄，同一行会忽左忽中。
- 面板子面的裁剪正好卡在盒子角 (652,313) 与右缘，框只放底边 → 灰框只有右边
  和下边。四边都临时放开、画完原样还原。
- 盒子：位置用引擎给的 x/y/w，宽度按 `BriefWidth`（0=沿用 453）；行数不再
  受盒高 432 限制（原版就是靠面板裁剪漏掉末行），只受屏高限制。
- 边界框 `BriefMark`：深灰 0x40 双线，外 2px + 隔 2px + 内 1px；框内缘离
  文字 20px（BRIEF_PAD）。裁剪比框再外放 4px，否则框自己被自己开的裁剪
  切掉（右边框曾因此只剩一半）。
- 颜色 font+0x24（`0x434D13 mov si,[edx+0x24]`，Blit 层同源）。
- 任何一步不可靠返回 −1，调用方照旧走引擎，绝不画半截、不叠字。
- 别加 BriefBox / BriefClip：这两个配置连带改盒高、放底边裁剪、临时改 a1+0x1C 行高、`0x478D78/0x478D94` 的步进立即数补丁。
  ⚠️ 后者反汇编看是 GUI 控件的 Y 位移（ret 0x4、改 [esi+4]），跟文字排版无关（易被误标成"行步进"）。
- 日志：`brief: dt <调用者> <盒> <字数> own/eng`、`brief: own draw box=…`、
  `brief: w U+xxxx gdi= cell= full=`（前 8 个不同的字）、`brief: own skip why=N`。

## 配置（[VecText]，改后重启）

- Enable=1，FontFace=simsun.ttc（用户现值；文件名原样匹配，先游戏目录后系统目录），
  FontPx=0（自动校准），Bold=0，VShift=0。
- CreditPx=16（用户现值；>0 覆盖顶栏字号，0=跟随），CreditDY=0。
- 简报五键：BriefOwn=1（自绘，0=交回引擎）、BriefLine=130（行距）、
  BriefTrack=2（字距）、BriefWidth=620（盒宽）、BriefAdv=1（用字体前进宽）、
  BriefMark=1（深灰双线框标边界）。
  BriefTrack=2（字距像素）。BriefBox=1 / BriefClip=1 只在
  BriefOwn=0 时生效。
- LogDetail=0（1=开 gdi 逐字光栅日志），Render=1（矢量替换总开关）。
- 电力四键（PowerBottom/PowerX/PowerPad/PowerMode）见“底部电量”节。

## 已排除（勿重查）

- DrawText 钩里改 t+0x1C 调行距：t 是 ECX = 画布，不是字体，恒为 0。
  字体是第 1 栈参 a1（见“简报正文”节）。
- FontFace 行内注释：GetPrivateProfileString 不吃行内 ";"，
  `FontFace=msyh.ttc ; 说明` 会把注释当文件名致 found=none 静默回落。
  已在 ResolveFontFace 遇 ";" 截断（整数值不受影响）；日志 file= 值带 "; ..." 即此病。
  注：此旧方案已过时，见 notes/智能中键.md（OU_ReadString 加载层统一清洗）。
- IsPlausiblePtr 要求 4 字节对齐：u8/u16 级寻址一律判非法，
  字节级探测用 VecCanRead（对齐到 4 字节边界再验页）。
- 正文无体外循环：管线外 Blit 直调零命中，都经 DrawText。
- 已不存在的键（别再加回来）：`LogAdv`（无门挂载）、`FixLineH`（几何只动简报步进与正文盒）。

## 提示贴左出屏：返回值哨兵

- 根因：`VecRender` 成功返回新笔位 `x+step`；字左移出屏后 x 为负，
  调用方把负返回一律当失败，回落引擎窄字形 → 整串压短左移（盒不动）。
- 修法：真失败才返回 `VC_FAIL=-1000000`，成功（含负笔位）照常返回；
  调用方判 `r != VC_FAIL`。
- 排查顺序：疑似渲染失败先查判据是否误伤合法负值（`x<0` 出屏是常态，
  符号位不当错误位），再找真正的失败点。

## 日志（定位用，探针定案即删）

- `vtext:` 安装横幅与钩子成败（`dtail=` 表 LogDetail）、`config:` 各节配置、
  `vswfail`（接管区渲染失败，上限 8）、`vfb`（`VecRender` 失败+`why` 码，
  上限 40，`g_VecWhy` 只服务它）、`gdi:`（`[VecText] LogDetail=1` 才开）、
  `vstep`（简报步进补丁，装时一次）。
- 电力：`mpwr`（sb 几何+d/o+mode+rgb）、`mpwrp`（pri/vx/xy）、各 hook ON。
- 规则：探针一旦定性立即删，结论进笔记；重现成本远低于背日志坟场。

## 右侧提示战役总览（定案）

调用链：`Simple_`（外，画底）→ `Simple_+0x142（=0x4A5FF2）` 调 `Print`
画字 → `Print` 逐字调 `Blit`；建筑提示另经 `DrawText（0x00479041
小矩形）` 包办。超武名出自侧栏 `0x004A6345` 经 `Simple_`。

- 四道门（全部按“路径/发起者”，不按坐标/内容）：
  1. `Simple_` 主门：只收发起者 `0x004A6345`（名 x=1722 fore=0x07FF，
     计时器 x=1749 fore=0x07FF/0xFFFF，同行 y 差 19；提示 c=0x73AC616A
     fore=0xA92A，一刀切）。
  2. `Print`：只收冒号计时器（该层名与提示不可区分，CJK 不收）。
  3. `Blit` 超武门：只认 `DrawText` 路径（`g_DTAllDepth>0`），再豁免
     提示子树（`g_TipDrawDepth`，`0x00479041` 小矩形 w≤400/h≤60）与
     `inTip/inWt`。`VecToolMatch` 带内排除保留——漏网名单不被
     ToolBox 劫持的最后保险。
  4. `VecRender`：坐标归一 `VecNormC`（`(int)(short)v`，引擎 16 位语义，
     如曾见 tx+2000 高位脏；符号扩展保留真负坐标）；空裁剪哨兵
     `(0,0,-1,-1)` 且 `pp==g_SWCompW` 时退主面边界 `(0,0,pp-1,compH-1)`；
     真剔除（非负倒置）仍失败走原版。
  5. 返回值：成功返回新笔位（含合法负值），真失败返回
     `VC_FAIL=-1000000`；调用方判 `r != VC_FAIL`。
- 位置带 [1300,1752] 不动（对齐面板调过，相对化丢名单行）。
- 日志见“日志”节；配置只留活键（见“配置”节）。
- 教训总汇：门下在能区分的那一层 → 读数先验域 → 失败分层修 →
  返回值与坐标分域。

## 电力提示盒三修（定案）

- 存档名含“电”也画框：电字/子串分支缺调用者门。现两处一律要求
  caller == 0x4A5FF2（世界悬浮 Print 内层，与 VecToolMatch 同门）。
- 出界后底变半透明：删映射填充（出界采样失真，漏出引擎半透明底），
  改填不透黑 [L0..R0]x[yT+1..yB-1]，字随后画上。
- 出界后边框变红：顶行采样（玩家色）出界只剩黑，致回落纯红。
  加 g_TipBcCache/g_TipBcOK 粘存（成功即更新，采不到沿用；玩家色一局不变）。
- TipBox 只服务电串（建筑名走 Tool 路），黑底改动不影响建筑名。

## 底部电量（定案）

- 形态：侧栏穹顶“闪电+负载/电力”（PowerMode=1 则“闪电+余量纯数字”，
  够用正数/不够负数，颜色照走），居中（PowerX>0 则侧栏绝对 x），y 锚底边
  （y0=H-PowerPad-adv）；sb=168x1080 侧栏坐标 + Primary=1920x1080 屏坐标
  （穹顶屏 x=ViewBounds 右沿 1752+偏移）双份，同像素。
- 闪电 10x16（emoji 剪影阈值二值，不透明直写，幂等不闪；与文本同 fr/fg/fb）。
  两个雷：手写 col16 绿通道错位会偏青（改调 `Blend16(a=255)` 逐位同色）；软边混合在持久面上叠加会粗细闪（退回二值，底板 + PowerPlate 都不要）。
- 数值：CurrentPlayer(0xA83D4C)+0x53A4=PowerOutput/+0x53A8=PowerDrain
  （UpdatePower 0x508C30 反汇编实证）；非法值跳过。
- 颜色（Phobos PowerDelta 0.75/1.0）：d*4<o*3 绿，d<=o 黄，否则红；0/0 判黄。
- 字号=资金真字号 CreditPx（用户 16；CreditPx=0 则退原字行高，无+4），
  字体/粗体与资金相同（VecText_FontFace/Bold，不再用战役面）；
  GDI 只在值/字号变时光栅进 PwrCache，落面每路必画。
- 电力文本无阴影（用户要求）；总开关 [VecText] PowerBottom=0 即关
  （电力四键 PowerBottom/PowerX/PowerPad/PowerMode 全归 [VecText]，
  挨着 CreditPx/CreditDY；代码经 VecText.h 的 VT_SECTION 读取）。
- 四路同画（同一像素）：(1) 消息帧 Message::Draw 内（0x4F455D 唯一调用点）；
  (2) prepare 0x4F42F0（每循环，render 门控 a8e378，静置/滚完 render 跳过，
  屏冻结在被盖帧，故钩在此）；(3) BlitSidebar 入口（每次条带呈现前先恢复，
  呈出的帧必带字）；(4) UpdatePrimarySurface 入口（非条带呈现路，只在
  Hidden/Primary 白名单面上画，Composite 自动跳过）。
- 钩表（入口钩+跳板，BytesSame 验字节，失败留原版；安装日志回显）：
  - prepare 0x4F42F0，首 5B=A1 24 73 88 00，跳回 0x4F42F5；
  - BlitSidebar 0x6A70E0（thiscall ecx=Sidebar=0x87F7E8，栈 force，ret 4），
    首 6B=83 EC 38 53 55 56，跳回 0x6A70E6；
  - UpdatePrimarySurface 0x4F4780（__fastcall ecx=mouseCaptured,edx=surface），
    首 7B=83 EC 5C 8B 44 24 60（注意无 push esi，在后），跳回 0x4F4787。
  - Entry 皆 naked：pushal/pushfl→调 C→pop→尾跳跳板（非 call，保栈形；
    prepare/BlitSidebar 原 ret 4，Update 原 add esp,0xC0+ret）。
- 铁律：naked 操作数只许 "m"，禁止 "r"（编译器会在 pushal 前用 ecx/edx
  传址，盖掉入口活寄存器 → 启动崩；以反汇编验成品：pushal 起手 + FF15/FF25）。
- 上屏链实证：真上屏唯 BlitSidebar 内 Primary.CopyFromPart(Sidebar)
  （虚表+0x8；0x6A722C/7312/739D/742E/747E）；帧函数 0x4F452D 的
  WWMouse+0x40(Sidebar,1)+B0B519 只是侧栏光标（BlitSidebar 尾统一置旗），
  置旗≠上屏；每帧调 BlitSidebar 会带光标副作用致鼠标拖影（禁调）。
- 帧函数 0x4F4480（GScreen::Render）：Tactical×3→光标块→消息 Draw→tooltip→
  光标→ret，上屏在调用者。
- 加固：绘制先过玩家/数值门再碰面虚表；vt/lockFn验 IsPlausiblePtr
  （loading 半成品面）。
- 日志见“日志”节。
- 残留（到此为止）：极轻微颜色变暗闪烁（疑穹顶 art 边缘重画），不再跟。

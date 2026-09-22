# VecText 全局文本矢量接管（结论）

单 DLL 多文件中的一份（与 MsgFont 聊天框同 DLL）。只替换字形光栅，
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
- 横向：墨水宽超出字宽时，≤4px 对称裁边距（保笔画），>4px 才重采样。
  诊断 vsqz: wch/adv/inkW/m（0=裁边，1=重采样）。
- FontPx 大于格高必然溢出裁剪（物理限制）：如 19px 墨水进 16 格，
  居中也只能对称裁。字号请按格高选（自动=跟格）。
- 真寻址（Blit 反汇编实证，备用知识）：tbl=idata+0x18（指针），
  no=u16[tbl+wch*2]，rec=*(idata+0x1C)+(no-1)*(*(idata+0x14))。
- 键 =（idata， 字体名， 粗体），字体重载自动重算。
- 当前 game.fnt 为 19 高改版（原版 16 高下无简报问题）。
- FontPx > 0 为全局固定字号：墨水与字宽等比缩放（advance 跟字号走），
  简报行步进同样跟字号；行距 / 换行仍是引擎的，偏离原版太多可能溢出盒子。
  默认 0 = 自动校准。
- 高度不压缩：基准窗（永+g+j 墨水并集）纵向曾按 (y*refH)/h 压进 19 格，
  已删除。现输出自然高度 outH 行、与 DIB 1:1，降部超出格高如实画、
  由表面裁剪收尾。压高使墨水不满格，是超武列表闪烁（帧间残留 / 错位）的原因。

## 超武列表接管（SuperPx）

- 整体区域接管，不分路径：名（Print）与计时器（DrawText）都落在右侧
  面板区，Blit 层按区域一刀切——命中即隐藏原字形、按 SuperPx 当场重绘
  （同锁同裁剪，笔位/背景/后续全是引擎的）。
- 区域 = 主面（pitch 1024~4096）且 x≥0.83 屏宽且 y≥32。顶栏（y<32）、
  简报（x~652）、菜单（居中）天然排除。SuperPx=0（默认）全不碰。
- 教训：调用链判决（入口计数+首字栈掃） Root cause 是脆弱的——中间层
  0x4BAE49 挡住名发起者 0x4BB7EB（相等判必 miss）、计时器子面 pitch 小
  被 zone 误杀、ASCII miss 不记日志导致盲区；且全 Print 计数开销大。
  区域判定无状态、零栈掃、零误杀面。
- VecRender 失败也不回落原版（只走笔位，记 vswfail，上限 8），否则原字
  会冒出来破坏隐藏。
- 行距引擎实测 19px（17 字），SuperPx 别超太多否则溢出面板被切。

## 顶栏资金数字

- 同一串数字至少 5 个发起者反复画，且坐标相同（x=63..96, y=2）：
  0x4D002E 所在函数 [0x4CFE80,0x4D0170]（按 House 遍历），
  0x4BADAC/0x4BAE49 所在函数 [0x4BAD80,0x4BAEB9]（数字格式化），
  另有 0x7BC1BF / 0x411648 / 0x437E23。
- 按函数白名单追不完，且函数粒度分不开：顶栏函数也会画别的文本
  （如按 House 遍历连超武列表一起画），CreditPx 经函数上下文漏过去
  造成列表大小闪烁。函数白名单已删，条带是唯一判据。
- 最终规则（Blit 层，与发起者无关）：
  y<32 条带上的数字统一吃 CreditPx/CreditDY（CreditPx=0 且 CreditDY=0 时不碰）。
- 字号优先级：条带 CreditPx（仅条带数字）> 全局 FontPx > 自动校准。
- 数字行 y=2，CreditDY 建议 -2（贴顶；-5 会顶到 y=-3 切掉顶部）。
- 发起者判定须过 call 指令校验（栈上整数常落在代码段内，如 0x6D005C 是误报）。

## 简报正文

- 正文 = 一次 DrawText（经 0x621144），盒（652,313,453,432），
  约 442 字 23 行 x 19 = 437px。
- 末行被 Lock 进 BitFont+0x30 的面板裁剪切掉（底约 745，盒底 745）。
  原版矮墨水蹭过去，满格墨水脚被吃。
- 修法（BriefBox/BriefClip，默认开）：
  盒 H 432->470；裁剪底同步到 783（只放底边，pitch<1024 不碰，防小面越界）。
  盒底 783 < 1080，下面是黑边。
- 宽保持 453：文字画在 453 宽面板子面上，加宽文字不加宽面板会被切丢字。
  真加宽须改面板控件，另立项。
- 简报布局步进 0x478D78 / 0x478D94 跟格高 Lines（不是 GDI 字号）：
  原版二进制是 16，改版 19 高漏改，按 Lines 补。

## 配置（[VecText]，改后重启）

- Enable=1，FontFace=simhei.ttf（文件名原样匹配，先游戏目录后系统目录），
  FontPx=0（自动校准），Bold=0，VShift=0。
- CreditPx=0（跟随；>0 覆盖顶栏字号），CreditDY=-2（顶栏上移）。
- BriefBox=1，BriefClip=1。
- LogAdv=1（诊断：vcall/vbrief/vlen/vrows/vdigit/vclip/vctx/vpx/vref），
  Render=1（矢量替换总开关）。

## 已排除（勿重查）

- FontFace 行内注释：GetPrivateProfileString 不吃行内 ";"，
- IsPlausiblePtr 要求 4 字节对齐（(u&3)==0），u8/u16 级寻址
  （奇数码位表项、55 字节记录）一律判非法。字节级探测用 VecCanRead
 （对齐到 4 字节边界再验页）。曾因此 vorig: fallback（count/recsz 都对）。
  `FontFace=msyh.ttc ; 说明` 会把注释当文件名导致 found=none 静默回落。
  已在 ResolveFontFace 遇 ";" 截断（整数值不受影响）。日志 file= 值带
  "; ..." 即此病；useHex= 记解析出的真名 U+ 值备查。

- 正文无体外循环：管线外 Blit 直调零命中，都经 DrawText。
- FixLineH 已删除（几何只动简报步进与正文盒，其余保持原版）。

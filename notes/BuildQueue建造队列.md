# BuildQueue 参考（最小入库）

设计：本地队列 + 复刻点击，不碰模拟（Phobos 改 8 处模拟行为，
此处只拦输入、代下标准单）。只记录钩点与调用，不入库第三方源码全文。
右键（本钩结构性解决“复活”问题）：
- 第一次暂停走原生（cameo state==1 置 2 发 SUSPEND=`0xF`，到不了钩子）；
- 取消分支入口 `0x6AB191`（6 字节 `mov cl,[edi+0xcce]`，无 rel，
  return 0 安全）→ `BQ_CancelPeel`：Shift+右键清空本家本型本地后跑原指令
  （原生 AbandonAll=`0x2E`/Abandon=`0x10` 清模拟）；本地有本型则弹掉最后
  一个本地后跳原生出口 `0x6AB28F`（=IsQueued==0 的退出，EDI=类型、
  ESP=入口值，其余覆写后用，逐寄存器一致）；本地无本型跑原指令。
- E8 事件构造点（`0x6AB1B0/0x6AB231/0x6AB1FF/0x6AA7E7/0x6AAFDC`）一律不碰：
  仿调用造畸形事件致执行侧崩是教训。
- R1 镜像清已删：放置完成与取消在 `Object==null` 上不可区分，
  R1 会在放置瞬间清掉整个队列；“复活”问题已由本钩结构解决——模拟侧取消
  只可能在本地无本型时发出（有本地则被本钩弹出并压住原生），无需镜像。
钩子共九个（`0x6AA7BC` 左键切换门/`0x6AA884` 切换清产/`0x6AB689` 忙时记入/
`0x6AB6CE` 闲时Shift记入/`0x6AB0C1` 暂停检查/`0x6AB0C7` 暂停块剥离/`0x6AB197`
右键剥离/`0x6A9C86` 图标数字/`0x6A9C6C` 空工厂绘制）：Jcc 点永远跳转
不 return 0，跳转只跳前向原生目标。
Ares 同流钩子（查 `Ares.dll.inj` 实证，同函数内）：`0x6AB0B0` LoadCameo2
及一串 FixOffset（`0x6AB49D/4E8/577/620/741/802/825/920/92F` 等）；
`0x6AB0B0` 在 H0 上游，可能直跳暂停块/取消分支绕过 H0，故暂停块入口
另埋剥离钩（H0P），两条路都堵（`pa` 常驻 trace 看实际路由）。其余无 rel（return 0 安全），
跳转只跳前向原生目标。
右键规则（用户定稿，Shift 在此忽略）：
总数 N>1 时每次右键只减数不动在产（`0x6AB0C1` 处 state==1 且本地有本型
则弹一跳出口，不暂停；state!=1 由 `0x6AB197` 弹一跳出口）；N==1 时走
原生暂停取消。由来：日志实证用户排队时 Shift 常驻手上，Shift+右键清
全部会把“减一”变成“清零+杀在产”，故本地有本型时永远只弹一（c=shift
只记录不参与决策），只有本地无本型才放行原生。
左键切换清产压制（反汇编实证 `0x6AA7B6-0x6AA931`）：
左键点建筑图标，原生先按 `[[esp+0x2c]]` 发 ABANDON（`0x6AA7E7`，`0x10`）
+AbandonAll（`0x6AA8CD`，`0x2E`）切掉在产（进度作废即“从头开始”），再进
`0x6ABxxx` 下新单。单位式排队必须压住这两处，E8 点一律不碰：
H1 只跳原生地址并摆出与原生一致的寄存器（EDI=0/ESI=`[esp+0x10]`），
H2 只在开关开时跳原生跳过处；Shift+右键清模拟仍走 `0x6ABxxx` 原生分支。

钩子（`dist/MOExperience.dll.inj`，永远跳转，**禁止在含 rel 字节上 return 0**）：
铁律（Syringe `HandleException` 源码实证：跳板原样拷贝被偷字节，
`E8/Jcc` 的 rel 不修正；Phobos 全库同样从不在 rel 字节上 return 0）：
偷了 rel 的钩子永远跳转；E8 构造调用一律亲调 `EventCtor`（返回值回填
EAX）后跳构造后下一条；条件跳转按 EAX 真值仿判跳原生目标，永不跳回自身。
R_ESP 即原始 ESP（Syringe 补丁是 JMP 不压栈，`0x6A9C86` 的 `[ESP+0x34]`
取数实证）：E8 点五个 push 之后 `[ESP+0]`=houseIdx、`+4`=evt、`+8`=rtti、
`+0xC`=heap、`+0x10`=naval；缓冲取 ECX（前一条 lea 已跑，四处皆如此）。
本地匹配不用栈，只用寄存器类型（EDI/EBX）+ WhatAmI 守卫。

- `0x6AB689`（5 字节，`push -1 + or edx,-1`，无 rel）→ 记入本地队列后跳
  `0x6AB95A`（与原版拒绝同尾，多路到达 esp 一致，只跳过播音）。
  寄存器：EBP=abs（原版 `cmp ebp,7` 紧邻在前），EDI=图标类型
  （`0x6AB602 mov edi,[esp+0x28]`，`0x6AB6CE` 处 `[edi+0xCCE]`），
  ESI=工厂（`0x6AB602` 处 `GetPrimaryFactory` 结果，
  `0x6AB61E mov esi,eax`）。
- （历史钩点，已全撤，见上）
- `0x6AB6CE`（10 字节，`mov edx,[esp+0x24] + mov al,[edi+0xcce]`，无 rel）
  → 工厂闲时 Shift+左键顺手记 4 个（原版即时下单第 1 个，合计 5 个；
  ESI 可空时按主工厂回退），跑原指令。regs 同点击钩子。
- `0x6AB197`（8 字节，`push eax + push ecx + mov ecx,[0xa83d4c]`，无 rel）
  → 右键取消分支入口，见上（Shift 清本地后直通 / 有本地弹一跳 `0x6AB28F`
  / 无本地直通）。regs：EBP=abs，EDI=图标类型。到此的都是右键。
  （`0x6AB191` 6 字节，含义相同，换 8 字节避开生僻长度。）
- （历史：`0x6AB1B5` 的 8 字节 Jcc 仿判思路由 `0x6AB191` 继承，
  但不再读 IsQueued/EAX、不做条件仿判，只跳无条件出口。）
- 右键语义（原版反汇编实证，`0x6AB0AC-0x6AB0A7`）：cameo state==1 则置 2、
  发 SUSPEND、跳 `0x6AB28f`（暂停）；state!=1 才进 `0x6AB18d` 取消分支
  （IsQueued→`0x2E`/`0x10`）。匹配用户规格：单个（第一次暂停第二次取消）、
  多个（第一次暂停第二次减一、无数字时取消）、非当前直接减数、
  Shift+右键清空本型。
- Shift 检测：`0x53EC70`（ecx=`0x10`，`test ah,0x80`；与 `0x54F25B` 原版
  用法逐字节一致，含 `GetKeyState` 后备）。另有 32 帧闩锁（Poll 每帧采样）：
  左键记数用“当前或近期按下”（防单帧竞态/提前松开），右键清空只认当前。
- 建筑页/防御页天然分开：条目按 house+heap 匹配（heap=ArrayIndex 类型唯一，
  同型必同 BuildCat/同工厂），清空只清本家本型。
- 诊断 trace（`MOExperience.log`，点击级）：`bq: qd/qf/qn`（左键记入），
  `bq: h0`（右键到暂停检查，a=state b=heap），`bq: h1`（到切换门，
  a=flag b=ebxHeap），`bq: cp`（右键，a=heap b=剩本地 c=shift
  d=1弹一/2清空/3暂停剥离/0走原生），`bq: sw`（压住切换清产），
  `bqueue: queued/fire/unqueued`（记入/开工/移除），
  `bqueue: modbase=`（对照崩溃 EIP 归因模块）。
  `h0/h1/cp-d0` 常驻：每次点击必留一行，无刷屏。
- 绘制空工厂钩：`0x6A9C6C`（6 字节 test+mov，无 rel）。
  绘制按 buildcat=0 取槽工厂，建筑页没下过单则槽空，原生直接跳过计数，
  防御页连带没数字。工厂空但本地有本型→EAX/EBX=本地数跳绘制块；
  工厂空且本地无→跳原生跳过处。`[esp+0x18]` 在两目标后无人读
  （`0x6A9CC9-0x6AA01C` 全段实证），绘制跳跃无需处理。
- 待发事件窗守卫：Poll 挂渲染可快于逻辑帧，连发两单则第二单
  在执行侧被拒（`0x4FA520` 首部 busy+building 直接返 3），静默丢单；
  故发单后同线 45 轮内不再发（执行后 Object 非空本来就会停）。
- 确认制：发单只 mark flying 不移除，见模拟认领
  （`CountTotal>0`，`bqueue: onboard`）才移除；执行拒收则条目留着数字
  留着，pend 窗后重发。发而不删，永不哑单丢数。
- `0x6A9C86`（5 字节，`mov ebx,eax + cmp ebx,1`，CountTotal 取数点）→ 本地
  有排队或原生数为 0 时改用全厂求和（`sim+n`，`sim` 为本家各工厂
  `CountTotal` 之和）后跳 `0x6A9CC9` 借用原版绘制数字，否则跑原指令。
  跨页漏计：绘制只认单槽工厂，防御页在产的会被漏（5 显示 4），全扫才对；
  建筑页单个在产原生数>0 时仍跑原生，零影响。`[ESP+0x34]`=图标类型。

事件复刻（与点击路径 `0x6AB741-0x6AB7C7` 逐字节同义）：

- `EventClass` ctor `0x4C6970`（thiscall，111 字节栈缓冲，
  houseIndex/`0xE`(Produce)/rtti/heap/naval）。
- `OutList`：计数 `0xA802C8`（满 `0x80` 下帧重试），111 字节拷入
  `0xA802D4+next*111`，无参回调 `call [0x7E1530]`（全二进制数十处同调，
  必忽略寄存器），并行数组 `0xA83A54[next]`=调用后 EAX（全二进制只写不读），
  `0xA802D0=(next+1)&0x7F`，计数+1。
- 参数：rtti=7；heap=`[type+0xDF8]`（BuildingType.ArrayIndex）；
  naval=`[type+0xCCE]`（TechnoType.Naval）；houseIndex=`[house+0x30]`
  （House.ArrayIndex）。`Factory.Object=+0x58`，`Owner=+0x6C`；
  `FactoryClass::Array` 基 `0xA83E30`，Items=+4，Count=+0x10；
  `CurrentPlayer=*(0xA83D4C)`；`EventType::Produce=0xE/Abandon=0x10`。
  （MSVC+Yrpp 头 `offsetof` 探针求值，原版反汇编交叉验证。）

轮询（`BuildQueue_Poll`，挂 `PowerDrawBoth` 每循环，无对账无丢单）：
条目不存工厂指针（只存 house/type/naval/cat，cat 取自点击帧 `[esp+0x24]`
与原生 GPF 入参同值），每轮按 `(house,naval,cat)` 现解析槽位工厂
（槽位即权威，无 stale）；工厂不存在/非本家→留着等（卖了重建/具现后
自动续上）；`Object==null` 才下单；同一工厂每轮只喂一项（事件下帧才
执行）。教训：存工厂指针+R0 存活校验，开工后日志实证整
队列被 drop（`fire` 后连串 `drop` 到 depth=0），遂删除整个 R0/drop path。
菜单/读档/换 CurrentPlayer 即清空（未下单=未扣费）。上限 16。

结论：

- 下的全是标准事件，单机联机通用，混装原版端也只看到普通订单；
  存档丢队列（经济一致）。
- `Ares.dll.inj` 在以上地址段零钩子，无重叠。
- 开关关时钩子全跑原指令、轮询直接返回。

本地使用前，必须用反汇编核对上述地址是否适用于 `G:\motest\gamemd.exe`。

- 真分发( 右键先看[esp+0xB0]&0x10, 再看[esp+0x18]: null 走 0x6AB0AC 明路(旧钩); 非 null 且在产未暂停走 0x6AAFE1 暗暂停(SUSPEND 经 0x6AB02A, ec 调用者 0x6AB02F); 否则按 shift 走 0x6AAE4A(0x2E)/0x6AAFBD(0x10, 经 jmp 0x6AB1FF 共享尾, ec 调用者恒 0x6AB204, 故旧 ex 哨兵全被绕过)。剥离搬到暗路三钩: hd 哨兵(暂停不动)+AFBD/AE4A 弹尾减数(N==1/模拟<=1 走原生)。

- 崩溃定位: DarkCancelPeel(0x6AAFBD,10B) 非零返回后跳 0x6AB28F, 落点 EIP=0x0693002D(C0000005, EBP=7), 疑 9/10B 钩非零返回触 Syringe 短桩问题(8B/6B 同模式无事, FreeShift-10B 仅 return 0 无事)。1暗路合并为单 6B-Jcc 钩 0x6AAE44(分发点, 与 H0 同构): 弹尾/拦下跳出口, 放行跳回原生去向(AFBD/AE4A)。

- 清理: 撤 H1/H2/H0P/ex×2/ec(6 钩, 全版本零触发, 日志为证)+对应代码/defines/inj 行; H0-state==1 改一律原生暂停(d=3 退役, 与暂停优先模型一致); DarkPeel 去 kind/shift 参数(d=6/7); 头注释重整(删“忽略 Shift”等过期规则, legend 去 h1/pa/sw/ex/ec/d=3/d=4/d=9)。现 8 钩: QueueClick/FreeShift/H0/CancelPeel/DarkDispatch/DarkPauseSeen/ShowCount/DrawNullFac。

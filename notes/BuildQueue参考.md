# BuildQueue 参考（最小入库）

设计：本地队列 + 复刻点击，不碰模拟（Phobos 改 8 处模拟行为，
此处只拦输入、代下标准单）。只记录钩点与调用，不入库第三方源码全文。
右键（0.2.92d 起，本钩结构性解决“复活”问题）：
- 第一次暂停走原生（cameo state==1 置 2 发 SUSPEND=`0xF`，到不了钩子）；
- 取消分支入口 `0x6AB191`（6 字节 `mov cl,[edi+0xcce]`，无 rel，
  return 0 安全）→ `BQ_CancelPeel`：Shift+右键清空本家本型本地后跑原指令
  （原生 AbandonAll=`0x2E`/Abandon=`0x10` 清模拟）；本地有本型则弹掉最后
  一个本地后跳原生出口 `0x6AB28F`（=IsQueued==0 的退出，EDI=类型、
  ESP=入口值，其余覆写后用，逐寄存器一致）；本地无本型跑原指令。
- E8 事件构造点（`0x6AB1B0/0x6AB231/0x6AB1FF/0x6AA7E7/0x6AAFDC`）一律不碰：
  0.2.90d 前仿调用造畸形事件致执行侧崩是教训。
- R1 镜像清（0.2.91d）已删：放置完成与取消在 `Object==null` 上不可区分，
  R1 会在放置瞬间清掉整个队列；“复活”问题已由本钩结构解决——模拟侧取消
  只可能在本地无本型时发出（有本地则被本钩弹出并压住原生），无需镜像。
钩子共七个（`0x6AA7BC` 左键切换门/`0x6AA884` 切换清产/`0x6AB689` 忙时记入/
`0x6AB6CE` 闲时Shift记入/`0x6AB0C1` 暂停检查/`0x6AB197` 右键剥离/`0x6A9C86`
图标数字）：Jcc 点永远跳转不 return 0，其余无 rel（return 0 安全），
跳转只跳前向原生目标。
右键规则（用户定稿，0.2.96d 起）：总数 N>1 时每次右键只减数不动在产
（`0x6AB0C1` 处 state==1 且本地有本型则弹一跳出口，不暂停；state!=1 由
`0x6AB197` 弹一跳出口）；N==1 时走原生暂停取消。
左键切换清产压制（0.2.94d 起，反汇编实证 `0x6AA7B6-0x6AA931`）：
左键点建筑图标，原生先按 `[[esp+0x2c]]` 发 ABANDON（`0x6AA7E7`，`0x10`）
+AbandonAll（`0x6AA8CD`，`0x2E`）切掉在产（进度作废即“从头开始”），再进
`0x6ABxxx` 下新单。单位式排队必须压住这两处，E8 点一律不碰：
H1 只跳原生地址并摆出与原生一致的寄存器（EDI=0/ESI=`[esp+0x10]`），
H2 只在开关开时跳原生跳过处；Shift+右键清模拟仍走 `0x6ABxxx` 原生分支。

钩子（`dist/MsgFont.dll.inj`，永远跳转，**禁止在含 rel 字节上 return 0**）：
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
  （0.2.92d 曾用 `0x6AB191` 6 字节，含义相同，换 8 字节避开生僻长度。）
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
- 诊断 trace（`MsgFont.log`，点击级）：`bq: qd/qf`（a=heap b=want c=rawShift
  d=实际用），`bq: cp`（a=heap b=剩余/清掉数 c=shift d=1弹一/2清空），
  `bqueue: queued/fire/unqueued/drop`（记入/开工/移除/丢弃），
  `bqueue: modbase=`（对照崩溃 EIP 归因模块）。
- `0x6A9C86`（5 字节，`mov ebx,eax + cmp ebx,1`，CountTotal 取数点）→ 本地
  有排队则 EAX/EBX=原版数+本地数后跳 `0x6A9CC9` 借用原版绘制数字，
  否则跑原指令。`[ESP+0x34]`=图标类型（调用前后 esp 无净变化）。

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

轮询（`BuildQueue_Poll`，挂 `PowerDrawBoth` 每循环，0.2.95d 起无对账无丢单）：
条目不存工厂指针（只存 house/type/naval/cat，cat 取自点击帧 `[esp+0x24]`
与原生 GPF 入参同值），每轮按 `(house,naval,cat)` 现解析槽位工厂
（槽位即权威，无 stale）；工厂不存在/非本家→留着等（卖了重建/具现后
自动续上）；`Object==null` 才下单；同一工厂每轮只喂一项（事件下帧才
执行）。教训：0.2.93d-94d 存工厂指针+R0 存活校验，开工后日志实证整
队列被 drop（`fire` 后连串 `drop` 到 depth=0），遂删除整个 R0/drop path。
菜单/读档/换 CurrentPlayer 即清空（未下单=未扣费）。上限 16。

结论：

- 下的全是标准事件，单机联机通用，混装原版端也只看到普通订单；
  存档丢队列（经济一致）。
- `Ares.dll.inj` 在以上地址段零钩子，无重叠。
- 开关关时钩子全跑原指令、轮询直接返回。

本地使用前，必须用反汇编核对上述地址是否适用于 `G:\motest\gamemd.exe`。

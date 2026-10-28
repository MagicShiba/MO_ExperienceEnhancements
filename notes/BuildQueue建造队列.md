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
- E8 事件构造点（`0x6AB1B0/0x6AB231/0x6AB1FF/0x6AA7E7/0x6AAFDC`）一律不碰，
  不仿造事件构造。
- 无需镜像清理：模拟侧取消只可能在本地无本型时发出
  （有本地则被本钩弹出并压住原生）。
钩子共八个（`0x6AB689` 忙时记入/`0x6AB6CE` 闲时Shift记入/`0x6AB0C1` 暂停检查/
`0x6AB197` 右键剥离/`0x6AAE44` 暗分发/`0x6AAFE1` 暗暂停哨兵/
`0x6A9C86` 图标数字/`0x6A9C6C` 空工厂绘制）：Jcc 点永远跳转
不 return 0，跳转只跳前向原生目标。
Ares 同流钩子（查 `Ares.dll.inj`）：`0x6AB0B0` LoadCameo2
在 H0 上游，可能直跳暂停块/取消分支绕过 H0，故暂停块入口
另埋剥离钩，两条路都堵。
右键规则（用户定稿，Shift 在此忽略）：
总数 N>1 时每次右键只减数不动在产；N==1 时走
原生暂停取消。本地有本型时永远只弹一，
只有本地无本型才放行原生。
左键切换清产压制（`0x6AA7B6-0x6AA931`）：
左键点建筑图标，原生先按 `[[esp+0x2c]]` 发 ABANDON（`0x6AA7E7`，`0x10`）
+AbandonAll（`0x6AA8CD`，`0x2E`）切掉在产，再进
`0x6ABxxx` 下新单。单位式排队必须压住这两处，E8 点一律不碰。
H1 只跳原生地址并摆出与原生一致的寄存器（EDI=0/ESI=`[esp+0x10]`），
H2 只在开关开时跳原生跳过处；Shift+右键清模拟仍走 `0x6ABxxx` 原生分支。

钩子（`dist/MOExperience.dll.inj`，永远跳转，**禁止在含 rel 字节上 return 0**）：
铁律：偷了 rel 的钩子永远跳转；条件跳转永不跳回自身。
R_ESP 即原始 ESP（Syringe 补丁是 JMP 不压栈）：E8 点五个 push 之后
`[ESP+0]`=houseIdx、`+4`=evt、`+8`=rtti、`+0xC`=heap、`+0x10`=naval；
缓冲取 ECX。本地匹配不用栈，只用寄存器类型（EDI/EBX）+ WhatAmI 守卫。

- `0x6AB689`（5 字节，`push -1 + or edx,-1`，无 rel）→ 记入本地队列后跳
  `0x6AB95A`（与原版拒绝同尾，多路到达 esp 一致，只跳过播音）。
  寄存器：EBP=abs（原版 `cmp ebp,7` 紧邻在前），EDI=图标类型
  （`0x6AB602 mov edi,[esp+0x28]`，`0x6AB6CE` 处 `[edi+0xCCE]`），
  ESI=工厂（`0x6AB602` 处 `GetPrimaryFactory` 结果，
  `0x6AB61E mov esi,eax`）。
- `0x6AB6CE`（10 字节，`mov edx,[esp+0x24] + mov al,[edi+0xcce]`，无 rel）
  → 工厂闲时 Shift+左键顺手记 4 个（原版即时下单第 1 个，合计 5 个；
  ESI 可空时按主工厂回退），跑原指令。regs 同点击钩子。
- `0x6AB197`（8 字节，`push eax + push ecx + mov ecx,[0xa83d4c]`，无 rel）
  → 右键取消分支入口，见上（Shift 清本地后直通 / 有本地弹一跳 `0x6AB28F`
  / 无本地直通）。regs：EBP=abs，EDI=图标类型。到此的都是右键。
- 右键语义（`0x6AB0AC-0x6AB0A7`）：cameo state==1 则置 2、
  发 SUSPEND、跳 `0x6AB28f`（暂停）；state!=1 才进 `0x6AB18d` 取消分支
  （IsQueued→`0x2E`/`0x10`）。
- Shift 检测：`0x53EC70`（ecx=`0x10`，`test ah,0x80`，含 `GetKeyState` 后备）。另有 2 逻辑帧闩锁：
  Poll 记下最近按下的逻辑帧号，左键记数用“当前或刚按过”（防单帧竞态/提前松开），
  右键清空只认当前。窗口按逻辑帧而非轮询次数：Poll 挂渲染 prepare，按次数算会随帧率
  变成几百毫秒，那段时间的普通点击会被当成 Shift+点击（一次记 5 个）。
- 建筑页/防御页天然分开：条目按 house+heap 匹配（heap=ArrayIndex 类型唯一，
  同型必同 BuildCat/同工厂），清空只清本家本型。
- 日志：仅启动两行（`bqueue: local building queue ON` + `bqueue: modbase=`）。
- 绘制空工厂钩：`0x6A9C6C`（6 字节 test+mov，无 rel）。
  绘制按 buildcat=0 取槽工厂，建筑页没下过单则槽空，原生直接跳过计数，
  防御页连带没数字。工厂空但本地有本型→EAX/EBX=本地数跳绘制块；
  工厂空且本地无→跳原生跳过处。
- 待发事件窗守卫：发单后同线 2 逻辑帧内不再发（执行后 Object 非空本来就会停，
  被拒则隔一帧自愈）。
- 确认制：发单只 mark flying 不移除，等模拟认领才移除。认领判据＝本家任一工厂
  `Object` 的类型就是本条类型（`Object->vtbl+0x84`），扫 `FactoryClass::Array`，
  不依赖主工厂缓存槽。Object 为空不算认领，放置后变空才喂下一项。执行拒收则条目
  留着，重发窗口过后重试。发而不删，永不哑单丢数。
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

轮询（`BuildQueue_Poll`，挂 `PowerDrawBoth` 每循环）：
条目不存工厂指针（只存 house/type/naval/cat，cat 取自点击帧 `[esp+0x24]`，
与原生 GPF 入参同值），每轮按 `(house,naval,cat)` 现解析工厂，`Object==null`
才下单，同线按逻辑帧限速。菜单/读档/换 CurrentPlayer 即清空（未下单=未扣费），
上限 50。Poll 由 `PrepDue` 限速（约 60 次/秒，与帧率无关），时间判据一律用
逻辑帧 `ADDR_FRAME`。

主工厂槽（`GetPrimaryFactory 0x500510`）不是“找工厂”，只读 house 的缓存槽
（absID=6/7 → `[house+0x53BC]`，`buildcat==5` → `[house+0x53CC]`）。槽由引擎
`BeginProduction` 写：空则新建 `FactoryClass` 并写回，**拒单时清空**
（0x4FA7EC 一族）。所以槽空只表示“现在查不到”，不等于不能下单 —— 槽空照发
PRODUCE 事件（引擎执行侧会按需建厂写回），重发间隔 `BQ_BLIND_FRAMES`=15 帧，
防“产线真忙→拒单→清槽→再发”刷事件；单被接受后条目立刻被认领，不拖慢队列。

限造（`rules.ini BuildLimit`，本 mod 大量建筑限 1/2/3）：

- 达上限原版拒收，本地须按同口径算余量。忙工厂路径原生不查上限，须在钩子里自己查。
- 口径（只读）：`BuildLimit=[type+0x3B8]`（正=总数上限，负=历史上限，0=不限）；
  正限判 `OwnedBuildingTypes=[house+0x5500][ArrayIndex]`（含在产），
  负限判 `FactoryProducedBuildingTypes=[house+0x55A0][同]`；
  在排=各厂 `CountTotal` 之和再扣掉正在造本型的厂数（在产已计入 Owned）；
  计数器取值走 `0x49FAE0`，在产判定走工厂 `Object` 的 `vtable+0x84`。
- 用法 `BQ_RoomLeft(house,type,others)` = 还能再排几条（`others` 是同型其余本地数）：
  记入时只记余量内的条数，闲时 Shift 给原生那 1 个留位；每轮喂单前排不进的直接丢（=原版“点不下”，队列自愈）。
- Poll 认领删除条目后须 `continue`，否则会用上一条的工厂喂下一条。

权限复查（失去建造资格，如前置被卖/毁）：

- 原版 cameo 校验（`0x6AA781`）调 `HouseClass::CanBuild`(`0x4F7870`)：
  `==0` 永久不可造，`>0`（`1` 可造 / `-1` 临时灰化，如工厂断电）放行。
  本地复刻事件绕过了它，条目失去资格后永不认领，重发还会压住整条产线。
- 故 Poll 每条目喂单前调 `FN_CANBUILD(house,type,0,1)`，`==0` 则
  `BQ_RemoveAt`+`continue`，`>0` 不动。只读，联机安全。

结论：

- 下的全是标准事件，单机联机通用，混装原版端也只看到普通订单；
  存档丢队列（经济一致）。
- `Ares.dll.inj` 在以上地址段零钩子，无重叠。
- 开关关时钩子全跑原指令、轮询直接返回。

本地使用前，必须用反汇编核对上述地址是否适用于 `G:\motest\gamemd.exe`。

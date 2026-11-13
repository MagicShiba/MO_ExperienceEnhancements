# SmartArea 组合中键范围下令

触发 `[SmartArea] Hotkey`（默认 Shift+Middle，可配单键如 X1），点按/划线，
松开即执行。点按 = 单块，划线 = 沿轨迹各节点逐块环扫（去重，松开格点中排首，
上限 64 节点）。只有攻击/进入/进驻/占领才下单，移动/未知跳过。

## 进建筑（电厂 / 坦克碉堡 / 隧道口）

进建筑只有一种令：`Mission::Enter(7)` + Destination=**建筑对象**，步兵载具都一样。
依据是 `UnitClass::Mission_Enter`（0x741970，载具虚表 `0x7F5C70 + 0x480`）：

```
0x741983  [unitType+0xD6A] 非 0 才走这条（部署/进入型单位）
0x741C5D  cmp [unit.Mission], 7     只认 Enter，其余直接结束
0x741C6F  unit 有乘客 → 结束（先卸货）
0x741C7E  call 0x40DD70(目标ID)     目标 → 对象，给格必得 NULL
0x741CE5  [bldType+0x16AE] UnitAbsorb → 收编（0x70C610）
0x741D37  [bldType+0x16AB] Bunker     → 收编 + SetMission(3)
0x741DCC  建筑空 → [objVtbl+0x278](0xE, 建筑) = 把车/人加进建筑载员链
```

`Action::Enter(3)` 在 `FootClass::ObjectClickedAction`（0x4D74E0，字节表
0x4D7D04 + 跳转表 0x4D7CD0）里只发 `SetMission(7, 0, 目标, 0)`，建筑也一样；
`Sabotage(17)` 只对应 `Action::Sabotage(16)`（0x4D7788）。

| 目标 | 光标 | 下的令 |
|---|---|---|
| 可进驻楼 / 民用楼 | Enter(3) | `Enter(7)`，Destination=建筑 |
| 自家 InfantryAbsorb（YAPOWR/YAPOWRB/NAPRIS） | Enter(3)；Select(7)/Move(1) 由 `SA_BldFix` 补 | `Enter(7)` + 建筑，步兵按余位均分 |
| 自家 Bunker=yes（NATBNK/NATBNKK） | Ares 钩（挂 0x6FFEC0）给 SelfDeploy(4)，gamemd `0x740244` 又改成 Move(1) | `Enter(7)` + 建筑，按目标限 1 辆 |
| 自家 UnitAbsorb（NATUNE/NATUNH 隧道口） | 同上 | `Enter(7)` + 建筑，按目标限 1 辆 |
| 工程师 + 自家掉血楼 | GRepair(29) | `Capture(8)` 进驻即修 |

两条硬约束：

- **Destination 只能是建筑对象，不能给格。** `0x741C7E` 用 `0x40DD70` 解目标 ID，
  格解出来是 NULL，后面全跳过 —— 表现是"车开到楼旁边就停"。发 `Move(2)` /
  `Unload(16)` 也进不了收编分支。
- **光标只是提示，不能照抄。** 载具光标链是 `0x73FD50 → 0x4DDED0`；Bunker 建筑上
  给的 SelfDeploy(4) / Move(1) 都不是要下的令。

吸收型楼必须按余位均分：引擎对 InfantryAbsorb 楼"来者不拒"地 append 进载员链
（`0x4733A0`），超出 `Passengers` 的人不报错、只堆在楼外站着。余位由
`SA_GarrRoomPrep` 算：`Passengers`（类型 `+0x5E0`，电厂 5 / 隧道口 6）
− 已占（调 `0x473460`，`ecx = 建筑 +0x114`，引擎自己累加乘客 `Size`），本快照
已派的数由 `s_GarrRoom` 边派边减体现。**不要读 `[建筑+0x114]`**，那是乘客个数，
与 `Passengers` 不同量纲。

民房（`CanBeOccupied=yes`）与步兵碉堡（`NABNKR`）读不到容量，按不限：超额的人
引擎自己挡在楼外。

实现集中在 `SA_BldFix`（主判定之后、fallback 之前）：只按 `BuildingTypeClass` 上的
标记（`+0x16AB` Bunker / `+0x16AE` UnitAbsorb / `+0x16AF` InfantryAbsorb，读法见
`OU_BldFlag`）补令，只对自家建筑，工程师不走步兵那条。载具进碉堡/隧道口走
`s_SnapEnterTaken` 按目标限 1（`NumberOfDocks=1`）。

## 光标

走 `OU_MouseOverObjectNative(ignoreForce=1)`。本功能热键含 Shift，用 0 会被物理
Shift 污染成 ToggleSelect(8)，主判定全废。仍有待命机制（`SA_DirtyHeld`）：分不到
活的单位留旗，脏键（配置修饰/附加键或物理 Shift）松开后重判。

光标无令时走 fallback 规则表（`SA_Fallback`）：本家只允许步兵进自家运输机，其余
跳过；非本家载具/飞机/步兵对非建筑强攻；步兵对建筑仅工程师占领。

## 分组与限数

分配三段（`SA_Distribute`，终判与预览同一套）：

1. **均分轮**（`SA_GarrEvenPass`）：士兵进驻/占领建筑（民房、自家碉堡、电厂、
   隧道口）一人一楼轮转，余位大的楼自然多拿，余位 0 的楼退出。收口时把参与过
   的楼余位清零，后面的轮次不再往进驻类目标加人。
2. **首轮**：每目标取 `SquadSize` 个（默认 4），满则下一个目标。
3. **次轮**（`SA_SpreadLeft`）：剩下的人从轮转游标起找下一个能接的目标，每人一个，
   目标远少于人时自然均分。

进楼单按归属与类别限数：

| 目标 | 规则 |
|---|---|
| 自家楼 | 工程师修掉血楼只派 1 人（CAPTURE 进驻即修，人消耗），满血跳过；士兵进驻走均分轮（吸收型楼按余位，民房/碉堡不限） |
| 非自家楼 | 工程师占无主/中立科技（TL≠-1）按楼限 1；工程师对中立民房（TL=-1，无归属变更）只做维修（掉血 1 人/满血跳过）；士兵进驻中立民房走均分轮；敌楼强占：**不损伤模式按楼限 1，损伤模式按 `SA_ENG_SQUAD`（固定 4）**（见下节）；拆除按 Squad |
| 非自家载具 | `Enter`（占空车）按目标限 1；偷车 `Capture` 不分归属限 1 |
| 自家运输机 | 装人不限 |
| 自家碉堡/隧道口 | `Enter(7)` 开进去按目标限 1 辆（NumberOfDocks=1，与"占车限 1"共用标记） |
| 自家吸收型楼（电厂） | 按余位均分：余位 = `Passengers` − 已占，轮转一人一楼；余位 0（楼满）不派 |

## 工程师占敌楼派几个（Ares MultiEngineer）

工程师点敌方楼的光标由 Ares 决定（`InfantryExt::GetEngineerEnterEnemyBuildingAction`，
钩 `0x51E5E1`）：血高于 `EngineerCaptureLevel` 时给 `Damage(28)` 光标并在 `0x519D9C`
里直接削血，够低了才给 `Capture`。**战役例外**：Ares 硬编码 `gameMode == Campaign`
时不做损伤，一个工程师直接占领整栋楼。

所以"一个楼分几个工程师"跟着这个开关走（`OU_EngDamageOn`，纯只读）：

| 场合 | 判定 | 分配 |
|---|---|---|
| 战役（GameMode 0） | 恒关 | 按楼限 1 |
| 遭遇战（5） | `GameModeOptionsClass.MultiEngineer`（0xA8B26C） | 关 → 限 1；开 → `SA_ENG_SQUAD` |
| LAN / Internet（3/4） | 恒开 | `SA_ENG_SQUAD` |

`SA_ENG_SQUAD` 固定 4：前两人爆破、第三人占领、第四人维修。**不按血量算** —— 楼可能
被对方工程师修回满血，派出去的人也可能半路损失，按血量算的 2 人两种情况都不够；
残血楼多出的两人当补损/维修。第五个之后进门只是维修消耗，所以仍然封顶。

不开损伤的场合用 `s_SnapRepairSent`（与修楼共用的"按目标单派"标记）限 1：第一个
进楼后楼已属自家，多派无益。

工程师判定: `InfantryType+0xEC3`（Engineer=yes 本体）或 GRepair/Repair(29/11)
光标；`Capture(9)` 不能自证（士兵对空房同样拿 9，占领与进驻按单位类型区分）。

中立判定: `House->Type->MultiplayPassive`（+0x34/+0x1A6，只读）。
民房判定: `TechnoType->TechLevel`（+0x634，TL=-1，只读）。两者皆有启动自检，
失败即停用（回退到按敌方/按楼限 1）。

## 事件与预览

只下标准 `0x04` MegaMission（攻击 Target=对象；进入/进驻/占领都是
Destination=对象）；物理 Z 按住时 `IsPlanningEvent=1` 排队追加，否则直接下令。
OutList 满跨帧续发。

预览 = 快照 + 分配，与终判同一套（双 sink，终判松手重扫一次）。预览截断
64 单位 / 96 目标 / 128 线，换格或 400ms 刷新，80ms 节流，松手即消。

## 仲裁

须无 Ctrl（归模糊下令）；Z 不禁。开关关或热键坏时不认领，回落裸中键。
默认组合恰为 Shift+Middle，与 DeployPreview 的 Shift 锁轴排直线重合；选中含
可展开单位时 DeployPreview 优先（见 `SmartArea_ShiftMiddleOnly`）。

联机: 只发标准事件，本地只读不写模拟内存。日志仅启动行。

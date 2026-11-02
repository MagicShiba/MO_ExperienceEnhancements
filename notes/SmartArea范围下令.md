# SmartArea 组合中键范围下令

触发: `[SmartArea] Hotkey`（默认 Shift+Middle，可配单键如 X1），点按/划线，
松开即执行。点按 = 单块，划线 = 沿轨迹各节点逐块环扫（去重，松开格点中排首，
上限 64 节点）。只有攻击/进入/进驻/占领才下单，移动/未知跳过。进建筑见下节。

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
| 自家 Bunker=yes（NATBNK/NATBNKK） | Ares 的 hook `UnitClass_GetActionOnObject_SelfDeployCursor_Bunker`（挂 0x6FFEC0）给 SelfDeploy(4)，gamemd 自己的 `0x740244` 又改成 Move(1) | `Enter(7)` + 建筑，按目标限 1 辆 |
| 自家 UnitAbsorb（NATUNE/NATUNH 隧道口） | 同上 | `Enter(7)` + 建筑，按目标限 1 辆 |
| 工程师 + 自家掉血楼 | GRepair(29) | `Capture(8)` 进驻即修（不变） |

两条不能改回去的：

- **Destination 只能是建筑对象，不能给格。** `0x741C7E` 用 `0x40DD70` 解目标 ID，
  格解出来是 NULL，后面全跳过 —— 表现是"车开到楼旁边就停"。发 `Move(2)` 或
  `Unload(16)` 也都进不了收编分支。
- **光标只是提示，不能照抄。** 载具光标链是 `0x73FD50 → 0x4DDED0`；Bunker 建筑上
  给的 SelfDeploy(4) / Move(1) 都不是要下的令。

吸收型楼必须按余位均分：引擎对 InfantryAbsorb 楼是"来者不拒"地 append 进载员链
（`0x4733A0`），超出 `Passengers` 的人不报错、只堆在楼外站着。配额由 `SA_GarrQuota`
先算：余位 = `Passengers`（类型 `+0x5E0`，电厂 5 / 隧道口 6）− 已占（调 `0x473460`，
`ecx = 建筑 +0x114`，引擎自己累加乘客 `Size`）− 本快照已派。轮转一人一楼地分
（楼大余位多拿），配额 0 = 楼满不派，读不到容量 = 不限。**不要读 `[建筑+0x114]`**，
那是乘客个数，与 `Passengers` 不同量纲。

实现集中在 `SA_BldFix`（主判定之后、fallback 之前）：只按 `BuildingTypeClass` 上的
标记（`+0x16AB` Bunker / `+0x16AE` UnitAbsorb / `+0x16AF` InfantryAbsorb，读法见
`OU_BldFlag`）补令，只对自家建筑，工程师不走步兵那条。载具进碉堡/隧道口走
`s_SnapEnterTaken` 按目标限 1（`NumberOfDocks=1`）。

## 光标

走 `OU_MouseOverObjectNative(ignoreForce=1)`。本功能热键含 Shift，用 0 会被
物理 Shift 污染成 ToggleSelect(8)，主判定全废。仍有待命机制（`SA_DirtyHeld`）：
分不到活的单位留旗，脏键（配置修饰/附加键或物理 Shift）松开后重判。

光标无令时走 fallback 规则表（`SA_Fallback`）：本家只允许步兵进自家运输机，
其余跳过；非本家载具/飞机/步兵对非建筑强攻；步兵对建筑仅工程师占领。

## 分组与限数

分组 `SquadSize`（默认 4）：每目标首轮 Squad 个，剩下找最近可处理目标加入。

进楼单按归属与类别限数：

| 目标 | 规则 |
|---|---|
| 自家楼 | 工程师修掉血楼只派 1 人（CAPTURE 进驻即修，人消耗），满血跳过；士兵进驻不限数不查血（吸收型楼见末两行） |
| 非自家楼 | 工程师占无主/中立科技（TL≠-1）按楼限 1；工程师对中立民房（TL=-1，无归属变更）只做维修（掉血 1 人/满血跳过）；士兵进驻中立民房轮转均分；敌楼强占与拆除按 Squad |
| 非自家载具 | `Enter`（占空车）按目标限 1；偷车 `Capture` 不分归属限 1 |
| 自家运输机 | 装人不限 |
| 自家碉堡/隧道口 | `Enter(7)` 开进去按目标限 1 辆（NumberOfDocks=1，与"占车限 1"共用标记） |
| 自家吸收型楼（电厂） | 按余位均分：配额 = `Passengers` − 已占 − 已派，轮转一人一楼；配额 0（楼满）不派 |

工程师判定: `InfantryType+0xEC3`（Engineer=yes 本体）或 GRepair/Repair(29/11)
光标；`Capture(9)` 不能自证（士兵对空房同样拿 9，占领与进驻按单位类型区分）。

中立判定: `House->Type->MultiplayPassive`（+0x34/+0x1A6，只读）。
民房判定: `TechnoType->TechLevel`（+0x634，TL=-1，只读）。两者皆有启动自检，
失败回退旧行为。

## 事件与预览

只下标准 `0x04` MegaMission（攻击 Target=对象；进入/进驻/占领都是
Destination=对象）；物理 Z 按住时 `IsPlanningEvent=1` 排队追加，
否则直接下令。OutList 满跨帧续发。

预览 = 快照 + 分配，与终判同一套（双 sink，终判松手重扫一次）。预览截断
64 单位 / 96 目标 / 128 线，换格或 400ms 刷新，80ms 节流，松手即消。

## 仲裁

须无 Ctrl（归模糊下令）；Z 不禁。开关关或热键坏时不认领，回落裸中键。
默认组合恰为 Shift+Middle，与 DeployPreview 的 Shift 锁轴排直线重合；选中含
可展开单位时 DeployPreview 优先（见 `SmartArea_ShiftMiddleOnly`）。

联机: 只发标准事件，本地只读不写模拟内存。日志仅启动行。

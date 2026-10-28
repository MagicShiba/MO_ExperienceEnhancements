# SmartArea 组合中键范围下令

触发: `[SmartArea] Hotkey`（默认 Shift+Middle，可配单键如 X1），点按/划线，
松开即执行。点按 = 单块，划线 = 沿轨迹各节点逐块环扫（去重，松开格点中排首，
上限 64 节点）。只有攻击/进入/进驻/占领才下单，移动/未知跳过（不下移动令）。

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
| 自家楼 | 工程师修掉血楼只派 1 人（CAPTURE 进驻即修，人消耗），满血跳过；士兵进驻不限数不查血 |
| 非自家楼 | 工程师占无主/中立科技（TL≠-1）按楼限 1；工程师对中立民房（TL=-1，无归属变更）只做维修（掉血 1 人/满血跳过）；士兵进驻中立民房轮转均分；敌楼强占与拆除按 Squad |
| 非自家载具 | `Enter`（占空车）按目标限 1；偷车 `Capture` 不分归属限 1 |
| 自家运输机 | 装人不限 |

工程师判定: `InfantryType+0xEC3`（Engineer=yes 本体）或 GRepair/Repair(29/11)
光标；`Capture(9)` 不能自证（士兵对空房同样拿 9，占领与进驻按单位类型区分）。

中立判定: `House->Type->MultiplayPassive`（+0x34/+0x1A6，只读）。
民房判定: `TechnoType->TechLevel`（+0x634，TL=-1，只读）。两者皆有启动自检，
失败回退旧行为。

## 事件与预览

只下标准 `0x04` MegaMission（攻击 Target=对象，进入类 Destination=对象）；
物理 Z 按住时 `IsPlanningEvent=1` 排队追加，否则直接下令。OutList 满跨帧续发。

预览 = 快照 + 分配，与终判同一套（双 sink，终判松手重扫一次）。预览截断
64 单位 / 96 目标 / 128 线，换格或 400ms 刷新，80ms 节流，松手即消。

## 仲裁

须无 Ctrl（归模糊下令）；Z 不禁。开关关或热键坏时不认领，回落裸中键。
默认组合恰为 Shift+Middle，与 DeployPreview 的 Shift 锁轴排直线重合；选中含
可展开单位时 DeployPreview 优先（见 `SmartArea_ShiftMiddleOnly`）。

联机: 只发标准事件，本地只读不写模拟内存。日志仅启动行。

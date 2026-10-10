# PlaceScatter 侧栏放建筑驱离压住的自家单位

目的: 侧栏放建筑时, 把压在地基格里的自家载具/步兵向外驱离 (标准 MegaMission Move 事件, 与手点右键移动同形)。
平时晃幽影不碰任何单位 (Mode=1)。

## 行为 ([PlaceScatter])

* Mode: 0=禁用 (原版行为); 1=点下时驱离 (默认); 2=幽影跟随自动驱离 (另含点下那一次)。
* Interval: Mode=2 补扫间隔, 逻辑帧, 5~300, 默认 30。
* Batch: 每次触发最多发出的移动事件, 1~64, 默认 16。
* Dist: 出框后再向外推的格数, 1~4, 默认 2。
* 只处理侧栏放下的建筑; 基地车展开不管 (DeployPreview 已有自己的散开); 天上飞的不管。

## 流程 (每次触发跑同一套)

1. Poll (挂 PowerDrawBoth, 按帧号门控): 读幽影写缓存; Mode=2 到 Interval 扫一次; 哨兵扫 OutList。
2. 哨兵抓自家 Place 事件 → 用 5 帧内缓存的 footprint → ClearFoot。
3. ClearFoot: 包围盒定中心×2 → 扫 Unit/Infantry 数组, 框内自家活单位入队 → Drain。
4. Drain 逐个: 框外跳过 → 屏反方向 → BFS 找最近框外格 (反方向优先, 半径 10/节点 384, 对角禁切角) → 沿出框方向外推 Dist-1 格 (逐格验) → 下 Move 令。全灭跳过。

## 屏坐标 (菱形地图)

* 屏正上=格(-1,-1), 屏正右=格(+1,-1), 屏正下=格(+1,+1), 屏正左=格(-1,+1)。
* 反方向: ddx=格心×2-中心×2 (X), ddy=同 (Y); sx=ddx-ddy (屏横差), sy=ddx+ddy (屏纵差); 符号经查表得屏八方向 (N=0,NE=1,E=2,SE=3,S=4,SW=5,W=6,NW=7, 正压中心默认屏右)。
* 结论: 屏符号须经变换查表, 不可直接当格向量; 格 (0,-1) 不是正北, 整偏 45°。

## 挡路判定 (PS_CellBlocked, 只读)

* 水/岩 (LandType 2/3) 挡; 内容链 (First+0xE4/Alt+0xE8, 后继+0x30) 里有建筑/地形挡; 单位与飞机不挡。
* 覆盖物墙不在对象链里: 认 WallOwnerIndex (+0x50), 非 -1 即有墙。
* 格建筑位 (+0x12C & 0x2): 与对象链双保险。
* 对角步禁切角: 两正交邻格须都可走。

## 哨兵 (OutList, BuildQueue 同式)

* Type@0/HouseIndex@2/Place.Location X@0x13 Y@0x15; 大跳变 (>64, 如读档/菜单) 重同步, 单轮扫 64。
* 点下才驱离的两个门: 缓存须是 0~5 帧内的幽影 (下界防读档帧号回绕); 事件 Location 须落在缓存 footprint 内 (集合命中, 否则退到包围盒), 其他来源的自家 Place 不得驱离。
* 缓存不可删: 点下当帧引擎在同一点击处理内先写 Place 事件 (0x4ABB80) 再同步清幽影 (0x4ABC46 调 SetActiveFoundation(null)), 到 Poll 时活幽影已不可读。Mode=2 定时扫的是活幽影, 不经过缓存。
* 结论: 哨兵上限须大于 Batch (单次驱离塞 ≤Batch 条 Move 进 OutList, 上限过小会挤掉下一次的 Place)。

## 联机安全

* 不写任何模拟内存, 不直调改模拟的引擎函数。唯一模拟变更 = OutList 标准 MegaMission (FN_MEGA 0x4C6860 + TargetCell 0x6E6B20 + IsPlanningEvent=0), 与 DeployPreview/SmartOrder 同形。

## 只读偏移

* Display 0x87F7E8: +0x1174 CenterCell (鼠标格) / +0x1178 TopLeftOffset / +0x117C CurrentFoundation_Data (CellStruct 数组, 0x7FFF 终止, 上限 120); Base = Center + TopLeft。
* OutList: 计数 0xA802C8 / 写指针 0xA802D0 / 缓冲 0xA802D4 (槽 111B×128)。
* Unit 0x8B4108 / Infantry 0xA83DE8 (Items@+4/Count@+16); 对象头 Health 0x6C/InLimbo 0x81/IsAlive 0x90/Owner 0x21C; HouseIndex 0x30。
* 格子: FirstObject +0xE4 / AltObject +0xE8 / NextObject +0x30 / LandType +0xEC / WallOwnerIndex +0x50 / AltFlags +0x12C (0x2=内有建筑)。
* 坐标 OU_GetCoords (虚表+0x48); 帧号 0xA8ED84; YRpp AbstractType: Unit=1/Aircraft=2/Building=6/Infantry=15/Terrain=36。

## 结论 (证伪项, 代码里不留分支)

* 手按 X 的 Scatter 只踢到相邻格, 大地基送不出来 → 用定向 Move。
* 按点击次数搞差异化 (黑名单/冷却/换向) 让问题忽隐忽现 → 每次触发跑同一套。
* 第一圈直搜即 BFS 深度 1 (同展开序同结果) → 合并为一次 BFS。
* 局限: 已展开/瘫痪/卡死的单位令下不动是引擎行为, 须手动挪开。

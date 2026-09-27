# 快捷键写法 (Hotkey)

适用: `MOExperience.ini` 里 `[PatrolCircle]`、`[SmartOrder]`、`[TreeChop]`、
`[FuzzyAttack]`、`[MindSelect]` 的 `Hotkey` 项。

## 规则

- 格式: 修饰键 + 按键, 用 `+` 或空格连接。例: `Shift+P`、`Ctrl+Z+Middle`。
- 大小写不敏感: `ctrl+z` = `Ctrl+Z`。
- 空格/Tab 随便加, 自动忽略: `  ctrl + z  ` = `Ctrl+Z`。
- 行尾 `;` 后面是注释, 会被截掉: `Hotkey=Middle ; 散开` 只取 `Middle`。
- 多个按键时, **最后一个是主键, 前面的是必须按住的键**:
  `Ctrl+Z+Middle` = 按住 Ctrl 和 Z, 再按中键。
- `None` / `Off` / `Disabled` = 关掉该功能。
- 键缺失、空串、写错 → 用默认值, 不会炸。

## 修饰键

| 写法 | 说明 |
|---|---|
| `Ctrl` (`Control`) | Ctrl 键 |
| `Shift` | Shift 键 |
| `Alt` (`Menu`) | Alt 键 |
| `Win` (`Windows`) | Win 键 |

## 按键

单个字母/数字直接写: `A`~`Z`(字母键)、`0`~`9`(数字键)、`F1`~`F24`。

注意: 多位纯数字按老 `TriggerVK` 数值兼容, 如 `80`=P、`90`=Z、`4`=中键;
单个 `4` 则是数字键 4。要中键请写 `Middle`。

### 功能键

| 写法 | 按键 |
|---|---|
| `Space` | 空格 |
| `Tab` | Tab |
| `Enter` (`Return`) | 回车 |
| `Backspace` (`Back`) | 退格 |
| `Insert` (`Ins`) | Insert |
| `Delete` (`Del`) | Delete |
| `Home` | Home |
| `End` | End |
| `PageUp` (`PgUp`) | PageUp |
| `PageDown` (`PgDn`) | PageDown |
| `Up` / `Down` / `Left` / `Right` | 方向键 |
| `Pause` | Pause |
| `Num0`~`Num9` | 小键盘 0~9 |

### 鼠标

| 写法 | 按键 |
|---|---|
| `Middle` (`MMB`) | 鼠标中键 |
| `LMB` | 鼠标左键 |
| `RMB` | 鼠标右键 |
| `X1` | 鼠标侧键 1 (前进键) |
| `X2` | 鼠标侧键 2 (后退键) |

## 各功能默认

| 功能 | 默认 `Hotkey` | 说明 |
|---|---|---|
| 巡逻圈 `[PatrolCircle]` | `Z+Middle` | 按住 Z 划中键画圈; 范围智能组合认领时让路 |
| 智能下令 `[SmartOrder]` | `Middle` | 裸中键点按自适应(移动/攻击/进入/进驻)/划线散开; 按住 Z 让路巡逻, 按住 Ctrl 让路模糊, 组合中键让路区域自适应 |
| 砍树 `[TreeChop]` | `Ctrl+Z+Middle` | 三键齐按 |
| 模糊攻击 `[FuzzyAttack]` | `Ctrl+Middle` | 按住 Z 时让路巡逻/砍树; Ctrl+Shift+中键也归模糊; 按住时红色规划线连到分到的敌, 松手即消 |
| 范围智能 `[SmartArea]` | `Shift+Middle`（主键须是 `Middle`，可改单键如 `X1`） | 点按/划线扫块(轨迹各节点)挑目标, 按住时绿色细线连到分到的目标(松手即消), 按各自光标下对象令(不移动); 光标无令时按规则表: 非本家载具/飞机/步兵(非建筑)强攻、工程师对建筑占领(多楼轮转均分, 不管 SquadSize)、步兵对自家载具进入、本家其余跳过、非工程师步兵对建筑待命; 松开即执行; 待命等脏键全松后重判（配置的修饰/附加键，或物理 Shift——单 X1 时按住 Shift 点照样待命，松 Shift 即判）; 含 Ctrl 时归模糊，Z 不禁 |
| 心控收敛 `[MindSelect]` | `Shift+P` | 结尾只剩心控单位 |

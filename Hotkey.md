# 快捷键写法 (Hotkey)

适用: `MsgFont.ini` 里 `[PatrolCircle]`、`[AirSpread]`、`[TreeChop]`、
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
| 巡逻圈 `[PatrolCircle]` | `Z+Middle` | 按住 Z 划中键画圈 |
| 散开 `[AirSpread]` | `Middle` | 裸中键; 按住 Z 让路巡逻, 按住 Ctrl 让路模糊 |
| 砍树 `[TreeChop]` | `Ctrl+Z+Middle` | 三键齐按 |
| 模糊攻击 `[FuzzyAttack]` | `Ctrl+Middle` | 按住 Z 时让路巡逻/砍树 |
| 心控收敛 `[MindSelect]` | `Shift+P` | 结尾只剩心控单位 |

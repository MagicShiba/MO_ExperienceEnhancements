# 心灵终结体验优化拓展

首个目标：游戏内消息队列改用微软雅黑矢量渲染，先只动显示，不管捕获与交互。

目录：

- `src/`：参与构建的源码。
- `dist/`：唯一发布目录，只接受构建输出与 ini 样例。
- `notes/`：文档与最小外部参照。
- `test/`：一次性探针与实验，用完即删。
- `build/`：中间文件。

复用（不复制）：

- 构建链：`../AutoLoadDev/tools/mingw`
- 引用头：`../AutoLoadDev/references/YRpp`，必要时 `../AutoLoadDev/references/Phobos`
- 钩子参照：`notes/ChatBox参考.md`，只记地址与调用，不入库第三方全文。

当前状态：骨架与路线已定；下一步做 Composite 像素格式与取色探针。

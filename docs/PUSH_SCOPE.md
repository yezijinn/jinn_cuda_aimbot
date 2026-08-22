# GitHub 推送范围

本文档定义本项目推送 GitHub 时的内容边界。仓库根目录建议为 `<repo-root>`，不要把 `<project-root>` 整个目录作为 Git 根目录。

> 当前 `<repo-root>` 下未发现 `.git`。推送前需要先初始化仓库并配置远端，例如 `git init` + `git remote add origin ...`。

## 发布命名约定

- 软件名：`咔蚯`
- 软件版本号：与 Git tag 完全一致，例如 `v20260822`
- Git tag 强制格式：`vYYYYMMDD`，以北京时间为准
- Release title 与 tag 必须一致，例如 `v20260822`
- 发布资产文件名使用纯英文，例如 `kaiqiu-v20260822-win64.zip`

## 外部目录不推送

以下 `<project-root>` 外部目录不进入 `main` 仓库，不作为 GitHub 推送内容：

- `<project-root>\docs`：研究文档、项目修改记录、协作文档不推送
- `<project-root>\reference`：参考项目、官方 SDK 样例不推送
- `<project-root>\logs`、`<project-root>\temp`、`<project-root>\Codex定时任务内容.txt`：本地日志与临时任务文件不推送
- `../onnxruntime-win-x64-1.28.0`：本地 ONNX Runtime 运行库不推送

## 允许推送的核心内容

| 类别 | 内容 |
|---|---|
| 构建定义 | `CMakeLists.txt`、`CMakeSettings.json`、`build_current.ps1` |
| 源码入口 | `mybot.cpp`、`mybot.h`、`scr/` |
| 业务子模块 | `capture/`、`config/`、`detector/`、`keyboard/`、`mouse/`、`overlay/`、`runtime/`、`tensorrt/` 等 |
| 测试 | `tests/`（如有）、`benchmarks/provider_benchmark.cpp`、协议/smoke 测试源文件 |
| 文档 | `README.md`、`CONTRIBUTING.md`、`AGENTS.md`、`docs/` |
| 基础文件 | `LICENSE`、`.gitignore`、`.gitattributes` |

允许推送的 `docs/` 以实际维护者确认为准，只推送当前维护文档，不得把“本机路径、账号、密钥、聊天记录、调试日志”当成文档推入。

## 禁止推送的内容

| 类别 | 内容 |
|---|---|
| 构建产物 | `/build/`、`/build_cuda/`、`*.exe`、`*.dll`、`*.pdb`、`.ninja_*`、`CMakeCache.txt` |
| 本地 SDK/依赖树 | `/CUDA.TensorRT/`、`/modules/opencv/`、`/modules/_downloads/`、`/packages/`、D 盘父目录的 `onnxruntime-win-x64-1.28.0` |
| 运行时数据 | `/models/`、`/depth_models/`、`*.engine`、`*.onnx`、`config.ini`、截图 |
| 日志与临时内容 | `build_current.log`、`build_current_last.log`、`*.log`、`/temp/`、D 盘根目录的 `logs/` |
| 本地工具配置 | `.vs/`、`.idea/`、`.verify/`、`.claude/`、`.opencode/`、`tools/.bin/` |
| 敏感信息 | `.env`、`*.pem`、`*key*`、`*token*`、`*password*`、账号 Cookie、私人对话内容、设备序列号 |

## 分级决策

1. Git 根目录必须是 `main` 内部路径，避免把父目录的 ONNX Runtime、依赖 SDK、日志和定时任务文件一起纳入。
2. 只提交源码、文档和明确需要的构建脚本；不要把本地编译树、三方 SDK、模型文件、运行缓存提交上去。
3. 提交前检查暂存区：`git add <显式路径>`，不要用 `git add .` 全量添加。
4. 如果公开仓库，`build_current.ps1` 中的本机工具链路径、`<project-root>` 等应视为本地专用；保留脚本时可注明“环境专用模板”，不宜写成通用安装标准。
5. 日志和修改记录 `<project-root>\docs\aim_项目修改日志.md` 默认不进入 GitHub；如需同步，应转换为抽象化发布说明后再提交。

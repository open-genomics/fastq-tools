# Contributing to FastQTools

C++23 工程能力展示项目，欢迎贡献。

## 开发流程

```bash
git status --short --branch    # 检查工作树
./scripts/core/build --dev      # 构建
./scripts/core/lint check       # 格式检查
./scripts/core/test             # 测试
```

- 单人项目，默认直接在当前分支改动。
- 修改 C++ 源码后至少运行 `lint format` 和相关测试。
- 非平凡改动（行为/架构/契约/流程/测试策略/落盘格式）必带一篇 `.agents/notes/` 决策笔记，提交前跑 `./scripts/core/notes verify`。
- commit message 用 Conventional Commits：`feat|fix|docs|refactor|test|chore(scope): subject`。
- 注释优先中文。

## License

贡献内容按项目 LICENSE（MIT）授权。

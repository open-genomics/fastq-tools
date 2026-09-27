# Agent Note: 扩展为全能 QC 工具（双端、UMI、对比报告等）

Status: rejected — 产品面刻意收窄为 stat + filter + 库 API，不走 fastp 式全能路线

## Problem

定位问题：测序 QC 已有 fastp（全能 QC + 报告）、fastqc（报告）、seqkit（全能工具箱）等成熟选手，FastQTools 必须回答「为什么存在」——跟随做全能意味着在别人的主场作战。

## Proposal

把产品面扩展为全能 QC 工具：paired-end/interleaved 处理、UMI/去重、质控前后对比报告、HTML 可视化报告等。

## Alternatives considered

- **窄而深（采纳的方向）** — 只做 stat + filter + 最小 C++ 库 API，把零拷贝、对象池、流水线保序、CI 质量门这些用户感知不到的内核做到工程上限（`docs/architecture.md` 项目定位）。
- **跟随做全** — 最强论据：paired-end 是测序数据主流形态，用户习惯一站式报告体验，功能覆盖广更容易被采用。被否：全能路线与单人项目的维护现实直接冲突，且在定位上与 fastp 正面撞车——扩展面越大，越失去「聚焦内核」的存在理由。

## Risks

「做全」作为方向被否决，不等于永远排斥单点能力：若某个用户可见能力（如 paired-end）有真实需求驱动，可单独评估立项；但「补齐 fastp 功能面」式提案不应重提。

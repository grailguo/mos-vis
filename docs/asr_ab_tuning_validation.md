# A/B 参数调优与验证（唤醒后首句命令漏识别）

## 目标

解决“唤醒后第一句命令（如：开始校准）经常只识别到前缀/单字，需重复 2-3 次才命中”的问题。

## A/B 参数

## A（基线）

- `audio.channel_select_mode = fixed`
- `audio.track_switch_consecutive = 3`
- `vad2.threshold = 0.50`
- `vad2.start_frames = 3`
- `asr.preroll_ms = 400`
- `asr.tail_ms = 240`

## B（保守增强，已应用）

- `audio.channel_select_mode = auto_track`
- `audio.track_switch_consecutive = 2`
- `vad2.threshold = 0.42`
- `vad2.start_frames = 2`
- `asr.preroll_ms = 600`
- `asr.tail_ms = 320`

## 回退方式

把上述 6 个参数改回 A 值即可，其他配置保持不变。

## 验证日志点（必须采集）

## 1) 通道选择是否正确

关注：

- `capture ch_ema=[...] selected=<x> best=<y>`

判定：

- A 常见：`selected=2 best=5` 长时间不一致
- B 目标：`selected` 可跟随 `best`（或更快收敛一致）

## 2) 唤醒后首句是否及时进入识别

关注顺序：

1. `event=tts_done`
2. `Vad2Stage: speech start`
3. `event=asr_partial`
4. `event=asr_final`

判定：

- 从 `tts_done` 到 `speech start` 的延迟应明显下降
- 第一轮（`turn=2`）应尽量直接产出完整 `asr_final`

## 3) 空结果率是否下降

关注：

- `event=asr_final_empty`

判定：

- B 相比 A，`asr_final_empty` 次数降低

## 4) 语义命中是否提升

关注：

- `event=intent_route ... intent=device.control.calibrate`

判定：

- 唤醒后第一条“开始校准”在 1 次发话内命中比例提升

## 建议测试脚本（人工流程）

每轮执行 10 次：

1. 唤醒词 + ACK 播放结束
2. 立刻说“开始校准”
3. 记录是否 1 次命中

统计指标：

- 首次命中率（10 次中 1 次即命中的次数）
- 平均需重复次数
- `asr_final_empty` 次数

## 通过门槛（建议）

- 首次命中率 >= 80%
- 平均重复次数 <= 1.3
- `asr_final_empty` 相比 A 至少下降 30%

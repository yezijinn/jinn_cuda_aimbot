# Common Recipes

## CUDA/TensorRT Performance Test

```ini
backend = TRT
ai_model = your_model.engine
capture_method = duplication_api
capture_use_cuda = true
show_window = false
collect_data_while_playing = false
```

Then check `[CaptureDiag]` output.

## Provider Benchmark

Use this when you want repeatable provider timings without starting capture, overlay, input devices, or file logging:

```powershell
.\ai.exe --benchmark-providers
.\ai.exe --benchmark-providers cuda --bench-cuda-model models\your_model.engine
```

The benchmark prints one final CSV-style summary in seconds. Results append to `benchmark_results\provider_benchmark_cuda.csv`. Use `--bench-no-save` for a disposable run.

## KMBOX Net Control Test

```ini
input_method = KMBOX_NET
kmbox_net_ip = 192.168.2.188
kmbox_net_port = 8808
kmbox_net_uuid = 0E0A3CAB
```

## MAKCU Control Test

```ini
input_method = MAKCU
makcu_port = COM0
makcu_baudrate = 115200
```

Select the COM port and baud rate required by the connected device firmware.

Related docs:

- [Capture diagnostics](capture-diagnostics.md)
- [Input methods](input-methods.md)
- [Build（现行）](../README.md#现行编译规范2026-08-21-更新)：根目录 `build_current.ps1`

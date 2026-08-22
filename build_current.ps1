<#
    build_current.ps1 — 为「当前项目」编译 ai.exe 的可复用脚本
    验证环境：Windows + MSVC 19.51 (E:/DevTools/VisualStudio, 非标准安装)
              + CUDA 13.2 (nvcc) + TensorRT 10 (main/CUDA.TensorRT)
              + OpenCV CUDA (main/modules/opencv/build/cuda/install)
              + ONNX Runtime 1.28.0 (<aim>/onnxruntime-win-x64-1.28.0)
    生成物：<BuildDir>/Release/ai.exe

    关键陷阱说明：
      1. 本项目 build_cuda/CMakeCache.txt 写死指向旧副本 D:/aim/main，
         直接 cmake --build build_cuda 会编译「旧副本」而非当前项目。
      2. 官方 build_cuda.bat 依赖 vswhere，本环境 vswhere 找不到
         E:/DevTools/VisualStudio（非标准安装），会报“未找到 VS”。
      3. VsDevCmd.bat 在本环境有缺陷(ommon7)，不会写入 PATH，需手动补 bin。
      4. 源目录直接使用 ASCII 路径 D:/jinn_aim/main（不依赖软链，避免路径拼接错误）。

    注：2026-08-21 起已弃用 D:/aim_cur 软链，全程使用真实 ASCII 路径。
#>
$ErrorActionPreference = "Continue"

# ---- 1) 注入 VS2022 环境（直接调用 VsDevCmd，绕过 vswhere）----
$vsdev = "E:/DevTools/VisualStudio/Common7/Tools/VsDevCmd.bat"
$lines = cmd.exe /s /c "`"$vsdev`" -arch=x64 -host_arch=x64 && set" 2>$null
foreach ($l in $lines) {
    if ($l -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}

# ---- 3) 手动补工具 bin（VsDevCmd 在本环境未写入 PATH）----
$add = @(
    "E:/DevTools/VisualStudio/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x86"
    "E:/DevTools/VisualStudio/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
    "E:/DevTools/VisualStudio/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja"
    "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.2/bin"
)
$env:PATH = ($add -join ";") + ";" + $env:PATH

$cmake = "E:/DevTools/VisualStudio/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
$src   = "D:/jinn_aim/main"                       # 直接使用 ASCII 真实路径，不依赖软链
$trt   = "D:/jinn_aim/main/CUDA.TensorRT"
$build = "D:/jinn_aim/main/build_cuda"   # 使用官方 build_cuda 缓存，产物落 build_cuda/Release/ai.exe

# ---- 4) 配置 + 构建 ----
& $cmake --fresh -S $src -B $build -G "Ninja Multi-Config" `
    -DAIMBOT_USE_CUDA=ON `
    -DAIMBOT_TENSORRT_ROOT=$trt `
    -DCMAKE_CUDA_FLAGS=--allow-unsupported-compiler `
    -DCUDA_NVCC_FLAGS=--allow-unsupported-compiler

& $cmake --build $build --config Release --parallel

Write-Host "DONE. ai.exe -> $build/Release/ai.exe"
Test-Path "$build/Release/ai.exe" | ForEach-Object { Write-Host "ai_exe_exists=$_" }

# 打印退出码便于判断：cmake --build 的退出码即为编译成败
Write-Host "BUILD_EXIT=$LASTEXITCODE"
exit $LASTEXITCODE
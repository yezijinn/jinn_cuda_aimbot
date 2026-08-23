#ifndef ONNX_INSPECTOR_H
#define ONNX_INSPECTOR_H

#include <filesystem>
#include <string>

struct StartupOnnxReport
{
    bool success = false;
    std::string summary;
    std::string class_summary;
    std::string class_names;
    std::string text;
    int width = 0;  // 识别的模型输入宽，0 表示未知
    int height = 0; // 识别的模型输入高，0 表示未知
};

struct OnnxInspectionResult
{
    bool success = false;
    std::string text;
    std::string brief_text;
    std::string full_text;
};

OnnxInspectionResult inspectOnnxModel(const std::string& modelPath);
OnnxInspectionResult inspectOnnxModel(const std::filesystem::path& modelPath);

StartupOnnxReport inspectLoadedEngineOnnx(const std::string& modelPath, int runtimeResolution);
StartupOnnxReport inspectLoadedEngineOnnx(const std::filesystem::path& modelPath, int runtimeResolution);
const StartupOnnxReport& startupOnnxReport();
void publishStartupOnnxReport(StartupOnnxReport report);

#endif // ONNX_INSPECTOR_H

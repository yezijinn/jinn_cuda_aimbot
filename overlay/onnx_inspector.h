#ifndef ONNX_INSPECTOR_H
#define ONNX_INSPECTOR_H

#include <string>

struct StartupOnnxReport
{
    bool success = false;
    std::string summary;
    std::string class_summary;
    std::string class_names;
    std::string text;
};

struct OnnxInspectionResult
{
    bool success = false;
    std::string text;
    std::string brief_text;
    std::string full_text;
};

OnnxInspectionResult inspectOnnxModel(const std::string& modelPath);

StartupOnnxReport inspectLoadedEngineOnnx(const std::string& modelPath, int runtimeResolution);
const StartupOnnxReport& startupOnnxReport();
void publishStartupOnnxReport(StartupOnnxReport report);

#endif // ONNX_INSPECTOR_H

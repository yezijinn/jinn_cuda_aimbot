#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "onnx_inspector.h"

std::pair<int, int> reportModelSize(const std::string& text)
{
    const std::string marker = "导出尺寸：";
    const std::size_t markerPos = text.find(marker);
    if (markerPos == std::string::npos) return { 0, 0 };
    const std::size_t lineStart = markerPos + marker.size();
    const std::size_t lineEnd = text.find('\n', lineStart);
    const std::string line = text.substr(lineStart, lineEnd - lineStart);
    const std::size_t xPos = line.find('x');
    if (xPos == std::string::npos) return { 0, 0 };
    return { std::atoi(line.substr(0, xPos).c_str()), std::atoi(line.substr(xPos + 1).c_str()) };
}

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        std::cerr << "Usage: onnx_inspector_regression_test <model.onnx>\n";
        return 2;
    }

    const std::string modelPath = argv[1];
    auto task = std::async(std::launch::async, [&modelPath] {
        return inspectOnnxModel(modelPath);
    });

    try
    {
        if (task.wait_for(std::chrono::seconds(30)) != std::future_status::ready)
        {
            std::cerr << "Async inspection did not complete within 30 seconds.\n";
            return 1;
        }
        const OnnxInspectionResult result = task.get();
        if (!result.success)
        {
            std::cerr << "Inspection failed: " << result.text << "\n";
            return 1;
        }
        if (result.brief_text.empty() || result.full_text.empty())
        {
            std::cerr << "Inspection returned an incomplete report.\n";
            return 1;
        }
        if (result.text != result.full_text)
        {
            std::cerr << "Legacy report text did not retain the full report.\n";
            return 1;
        }
        if (result.full_text.find("========== ONNX MODEL INFO ==========") == std::string::npos ||
            result.full_text.find("========== YOLO ANALYSIS ==========") == std::string::npos ||
            result.brief_text.find("ONNX 模型自然语言分析报告") == std::string::npos)
        {
            std::cerr << "Inspection did not preserve the donor report sections.\n";
            return 1;
        }

        const std::string enginePath = modelPath.substr(0, modelPath.size() - 5) + ".engine";
        const StartupOnnxReport startup = inspectLoadedEngineOnnx(enginePath, 320);
        const std::pair<int, int> modelSize = reportModelSize(startup.text);
        if (!startup.success || modelSize.first <= 0 || modelSize.second <= 0)
        {
            std::cerr << "Startup ONNX report was not created for the matching engine path.\n";
            return 1;
        }
        const std::string expectedResolution =
            "智能推断，当前模型分辨率：" + std::to_string(modelSize.first) + "x" + std::to_string(modelSize.second);
        if (startup.summary.find(expectedResolution) == std::string::npos)
        {
            std::cerr << "Startup ONNX summary did not use the parsed model dimensions.\n";
            return 1;
        }
        if (startup.class_summary.find("智能推断，当前模型类别数量：") != 0)
        {
            std::cerr << "Startup ONNX report did not retain the model class summary.\n";
            return 1;
        }

        const StartupOnnxReport startupFromOnnx = inspectLoadedEngineOnnx(modelPath, 320);
        if (!startupFromOnnx.success || startupFromOnnx.text != startup.text)
        {
            std::cerr << "Startup ONNX report was not created when the ONNX path was selected directly.\n";
            return 1;
        }

        const std::string expectedPrefix =
            "========================================================\n"
            ".onnx模型信息输出\n" + startup.summary + "\n";
        if (startup.text.rfind(expectedPrefix, 0) != 0)
        {
            std::cerr << "Startup ONNX report did not retain the shared header and summary order.\n";
            return 1;
        }

        const std::vector<std::string> requiredFields = {
            "模型作者：",
            "导出时间：",
            "导出尺寸：",
            "构建版本：",
            "模型规模（Model Scale）：",
            "模型类别数量（Classes）：",
            "训练权重总数（Parameters）：",
            "模型张量计数（Tensor Count）：",
            "[0] ",
            "date = ",
            "author = ",
            "email = ",
            "imgsz = ",
            "nc = ",
            "names = ",
            "project = ",
            "========================================================"
        };
        for (const std::string& field : requiredFields)
        {
            if (startup.text.find(field) == std::string::npos)
            {
                std::cerr << "Startup ONNX report is missing required field: " << field << "\n";
                return 1;
            }
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "Async inspection threw: " << error.what() << "\n";
        return 1;
    }

    std::cout << "PASS: ONNX inspection completed in an async task.\n";
    return 0;
}

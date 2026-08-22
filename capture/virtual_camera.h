#ifndef VIRTUAL_CAMERA_H
#define VIRTUAL_CAMERA_H

#include <opencv2/opencv.hpp>

#include "capture.h"
#include "mybot.h"

class VirtualCameraCapture final : public IScreenCapture
{
public:
    VirtualCameraCapture(int width, int height, const std::string& cameraName, int captureFps, bool verbose);
    ~VirtualCameraCapture() override;

    cv::Mat GetNextFrameCpu() override;

    static std::vector<std::string> GetAvailableVirtualCameras(bool forceRescan = false);
    static void ClearCachedCameraList();

private:
    std::unique_ptr<cv::VideoCapture> cap_;
    int captureWidth{ 0 }, captureHeight{ 0 };
    int targetWidth_{ 0 };
    int targetHeight_{ 0 };
    std::string selectedCameraName_;
    int captureFps_{ 0 };
    bool verbose_{ false };

    int roiW_{ 0 }, roiH_{ 0 };

    // 第 22 轮：原有成员 cv::Mat frameCpu 仅被 GetNextFrameCpu 自身使用，
    // 是导致每帧多一次深拷贝的中转变量，已改为函数内局部对象，成员随之移除。
};

#endif // VIRTUAL_CAMERA_H

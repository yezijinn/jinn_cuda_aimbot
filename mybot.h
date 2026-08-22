#ifndef SUNONE_AIMBOT_2_H
#define SUNONE_AIMBOT_2_H

#include "config.h"
#include "trt_detector.h"
#include "mouse.h"
#include "MouseInput.h"
#include "detection_buffer.h"
#include "KmboxNetConnection.h"
#include "KmboxAConnection.h"
#include "Makcu.h"
#include <memory>
#include <mutex>

extern Config config;
extern TrtDetector* trt_detector;
extern DetectionBuffer detectionBuffer;
extern MouseThread* globalMouseThread;
extern KmboxNetConnection* kmboxNetSerial;
extern KmboxAConnection* kmboxASerial;
extern MakcuConnection* makcuSerial;
extern std::unique_ptr<IMouseInput> activeMouseInputOwner;
extern std::atomic<bool> input_method_changed;
extern std::atomic<bool> aiming;
extern std::atomic<bool> shooting;
extern std::atomic<bool> zooming;
extern std::atomic<int> active_mouse_hotkey_slot;
extern std::mutex configMutex;
extern std::mutex inputDevicesMutex;

#endif // SUNONE_AIMBOT_2_H

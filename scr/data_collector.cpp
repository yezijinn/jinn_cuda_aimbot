#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "scr/data_collector.h"
#include "other_tools.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>

// 全局配置锁，定义于 mybot.cpp。
// 仅用于 MaybeCollectDataSample 内部的配置快照阶段，详见该函数处的说明。
extern std::mutex configMutex;

namespace cvm {
namespace {

namespace fs = std::filesystem;

constexpr int64_t kCollectSaveCooldownNs = 500'000'000;

struct CollectRuntimeState
{
    std::uint64_t frame_counter = 0;
    std::uint64_t sample_counter = 0;
    std::uint64_t saved_image_count = 0;
    std::uint64_t saved_label_count = 0;
    int64_t last_collect_save_ns = 0;
    std::string last_output_dir;
    std::string last_status;
};

struct CollectConfigSnapshot
{
    bool enabled = false;
    bool only_when_aimbot_running = false;
    bool only_when_targets_present = false;
    int save_every_n_frames = 1;
    int jpeg_quality = 95;
    std::string output_dir;
    bool auto_label_data = false;
    float auto_label_min_conf = 0.25f;
    int auto_label_max_boxes = 20;
    std::string auto_label_record_classes;
};

struct CollectAttempt
{
    CollectConfigSnapshot cfg;
    std::uint64_t sample_id = 0;
};

CollectRuntimeState g_collectRuntimeState;
std::mutex g_collectRuntimeMutex;

std::string GetExecutableDir()
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return ".";

    return fs::path(exePath).parent_path().string();
}

std::string BuildCollectSampleStem(std::uint64_t sample_id)
{
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm local_tm{};
    localtime_s(&local_tm, &t);

    char time_buf[80] = {};
    std::snprintf(
        time_buf,
        sizeof(time_buf),
        "%04d%02d%02d_%02d%02d%02d_%03lld_s%06llu",
        local_tm.tm_year + 1900,
        local_tm.tm_mon + 1,
        local_tm.tm_mday,
        local_tm.tm_hour,
        local_tm.tm_min,
        local_tm.tm_sec,
        static_cast<long long>(ms.count()),
        static_cast<unsigned long long>(sample_id));
    return std::string(time_buf);
}

std::set<int> ParseRecordClasses(const char* s)
{
    std::set<int> ids;
    if (!s || !s[0])
        return ids;

    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        item = OtherTools::TrimAscii(item);
        if (item.empty())
            continue;

        try
        {
            ids.insert(std::stoi(item));
        }
        catch (...) {}
    }

    return ids;
}

cv::Mat PrepareFrameForSave(const cv::Mat& frame)
{
    if (frame.empty())
        return {};

    cv::Mat bgr;
    switch (frame.channels())
    {
    case 4:
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
        break;
    case 3:
        bgr = frame.clone();
        break;
    case 1:
        cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
        break;
    default:
        break;
    }

    return bgr;
}

std::string ModelNameToFolder(const char* model_name)
{
    if (!model_name || model_name[0] == '\0')
        return "default";

    std::string s = OtherTools::TrimAscii(model_name);
    if (s.empty())
        return "default";

    const fs::path p(s);
    const std::string stem = p.filename().stem().string();
    return stem.empty() ? "default" : stem;
}

CollectConfigSnapshot SnapshotCollectConfig(const Config& cfg)
{
    CollectConfigSnapshot snapshot;
    snapshot.enabled = cfg.collect_data_while_playing;
    snapshot.only_when_aimbot_running = cfg.collect_only_when_aimbot_running;
    snapshot.only_when_targets_present = cfg.collect_only_when_targets_present;
    snapshot.save_every_n_frames = std::max(1, cfg.collect_save_every_n_frames);
    snapshot.jpeg_quality = std::clamp(cfg.collect_jpeg_quality, 50, 100);
    snapshot.output_dir = cfg.collect_output_dir;
    snapshot.auto_label_data = cfg.auto_label_data;
    snapshot.auto_label_min_conf = std::clamp(cfg.auto_label_min_conf, 0.01f, 0.99f);
    snapshot.auto_label_max_boxes = std::max(1, cfg.auto_label_max_boxes);
    snapshot.auto_label_record_classes = cfg.auto_label_record_classes;
    return snapshot;
}

void UpdateRuntimeStatus(const std::string& output_dir, const std::string& status)
{
    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    if (!output_dir.empty())
        g_collectRuntimeState.last_output_dir = output_dir;
    g_collectRuntimeState.last_status = status;
}

std::string WriteYoloLabelFile(const fs::path& label_path,
                               const std::vector<cv::Rect>& boxes,
                               const std::vector<int>& classes,
                               const std::vector<float>& confidences,
                               int frame_width,
                               int frame_height,
                               float min_conf,
                               int max_boxes,
                               const std::set<int>* allowed_classes)
{
    std::ofstream out(label_path, std::ios::trunc);
    if (!out.is_open())
        return "label open failed";

    const float width = std::max(1.0f, static_cast<float>(frame_width));
    const float height = std::max(1.0f, static_cast<float>(frame_height));
    int written = 0;

    for (size_t i = 0; i < boxes.size(); ++i)
    {
        const int cls = (i < classes.size()) ? classes[i] : 0;
        const float conf = (i < confidences.size()) ? confidences[i] : 1.0f;
        if (conf < min_conf)
            continue;

        if (allowed_classes && !allowed_classes->empty() && allowed_classes->count(cls) == 0)
            continue;

        if (written >= std::max(1, max_boxes))
            break;

        const cv::Rect& box = boxes[i];
        const float x1 = std::clamp(static_cast<float>(box.x), 0.0f, width);
        const float y1 = std::clamp(static_cast<float>(box.y), 0.0f, height);
        const float x2 = std::clamp(static_cast<float>(box.x + box.width), 0.0f, width);
        const float y2 = std::clamp(static_cast<float>(box.y + box.height), 0.0f, height);

        const float box_w = std::max(0.0f, x2 - x1) / width;
        const float box_h = std::max(0.0f, y2 - y1) / height;
        if (box_w <= 0.0f || box_h <= 0.0f)
            continue;

        const float cx = std::clamp(((x1 + x2) * 0.5f) / width, 0.0f, 1.0f);
        const float cy = std::clamp(((y1 + y2) * 0.5f) / height, 0.0f, 1.0f);

        out << cls << " " << cx << " " << cy << " " << box_w << " " << box_h << "\n";
        ++written;
    }

    return std::to_string(written) + " label(s)";
}

std::pair<fs::path, fs::path> ResolveModelOutputDirs(const std::string& root_dir,
                                                     const char* model_name,
                                                     const CollectConfigSnapshot& cfg)
{
    const fs::path output_root = ResolveCollectOutputDir(root_dir, cfg.output_dir.c_str());
    const fs::path model_root = output_root / ModelNameToFolder(model_name);
    // YOLO 数据集约定：图片与标签同级放在 images/ 与 labels/ 下（ultralytics 直接可读）。
    // 原实现第一元素直接是 model_root 本身（缺 images/ 子目录），导致标注与图片同目录、
    // 无法被训练框架识别；同时使下方 model_root = images_dir.parent_path() 取到高一层路径。
    return { model_root / "images", model_root / "labels" };
}

bool BuildSaveFrame(const cv::Mat& frame, cv::Mat& save_frame)
{
    save_frame = PrepareFrameForSave(frame);
    return !save_frame.empty() && save_frame.cols > 0 && save_frame.rows > 0;
}

bool TryBeginCollectAttempt(const CollectConfigSnapshot& cfg,
                            const std::vector<cv::Rect>& boxes,
                            bool aimbot_enabled,
                            std::uint64_t& sample_id)
{
    if (!cfg.enabled)
        return false;

    if (cfg.only_when_aimbot_running && !aimbot_enabled)
        return false;

    if (cfg.only_when_targets_present && boxes.empty())
        return false;

    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    ++g_collectRuntimeState.frame_counter;
    if ((g_collectRuntimeState.frame_counter % static_cast<std::uint64_t>(cfg.save_every_n_frames)) != 0)
        return false;

    if (g_collectRuntimeState.last_collect_save_ns > 0 &&
        (now_ns - g_collectRuntimeState.last_collect_save_ns) < kCollectSaveCooldownNs)
    {
        return false;
    }

    g_collectRuntimeState.last_collect_save_ns = now_ns;
    sample_id = ++g_collectRuntimeState.sample_counter;
    return true;
}

void SaveCollectedFrame(const std::string& root_dir,
                        const char* model_name,
                        const cv::Mat& frame,
                        const std::vector<cv::Rect>& boxes,
                        const std::vector<int>& classes,
                        const std::vector<float>& confidences,
                        const CollectAttempt& attempt)
{
    cv::Mat save_frame;
    try
    {
        if (!BuildSaveFrame(frame, save_frame))
            return;

    const auto [images_dir, labels_dir] = ResolveModelOutputDirs(root_dir, model_name, attempt.cfg);
    const fs::path model_root = images_dir.parent_path();

    std::error_code ec;
    fs::create_directories(images_dir, ec);
    if (ec)
    {
        UpdateRuntimeStatus(model_root.u8string(), "Collect save failed: create images folder.");
        return;
    }

    ec.clear();
    fs::create_directories(labels_dir, ec);
    if (ec)
    {
        UpdateRuntimeStatus(model_root.u8string(), "Collect save failed: create labels folder.");
        return;
    }

    const std::string stem = BuildCollectSampleStem(attempt.sample_id);
    const fs::path image_path = images_dir / (stem + ".png");
    const fs::path label_path = labels_dir / (stem + ".txt");

    // PNG 无损保存。将配置项 jpeg_quality(50..100) 映射为 PNG 压缩级别：
    // 质量 100→级别 0（最快、无损）、50→级别 5，使 UI 滑块生效且输出仍为无损 PNG。
    const int png_level = std::clamp((100 - attempt.cfg.jpeg_quality) / 10, 0, 9);
    const std::vector<int> imwrite_params = {
        cv::IMWRITE_PNG_COMPRESSION, png_level
    };

    // 中文路径修复：不再使用 cv::imwrite(std::string, ...)。
    //
    // 原因：MSVC 下 std::filesystem::path::string() 产出的是 UTF-8 字节序列，而 OpenCV
    // imgcodecs 内部用窄字符 fopen 打开文件，会按进程 ANSI 代码页（简体中文机器为 CP936）
    // 去解释这串字节。本工程未配置 activeCodePage=UTF-8 的 manifest，两者不一致，
    // 于是只要输出目录里含有任何非 ASCII 字符（例如本项目路径 "…\鼠标移动算法\…"），
    // imwrite 就恒定返回 false，数据采集功能整体静默失效。
    // 更具迷惑性的是：同函数下面写标签用的是 std::ofstream(fs::path)，C++17 起走宽字符
    // 重载，反而能成功 —— 于是现场表现为"只生成了 .txt、没有 .png"，很难联想到编码问题。
    //
    // 修复方式：先 imencode 到内存，再用 std::ofstream 的 fs::path 重载（宽字符）落盘。
    // 编码参数、输出格式、返回语义均与原实现完全一致。
    bool image_ok = false;
    try
    {
        std::vector<uchar> encoded;
        if (cv::imencode(".png", save_frame, encoded, imwrite_params) && !encoded.empty())
        {
            std::ofstream image_file(image_path, std::ios::binary | std::ios::trunc);
            if (image_file.is_open())
            {
                image_file.write(reinterpret_cast<const char*>(encoded.data()),
                                 static_cast<std::streamsize>(encoded.size()));
                image_file.close();
                // 显式检查流状态：磁盘写满 / 无写权限时 write 不抛异常，只置 failbit，
                // close() 时的刷盘失败同样只反映在 failbit 上，必须在 close 之后判定
                image_ok = !image_file.fail();
            }
        }
    }
    catch (...)
    {
        image_ok = false;
    }

    if (!image_ok)
    {
        UpdateRuntimeStatus(model_root.u8string(), "Collect save failed: image write.");
        return;
    }

    bool label_ok = true;
    std::string label_result = "auto-label disabled";
    if (attempt.cfg.auto_label_data)
    {
        const std::set<int> allowed = ParseRecordClasses(attempt.cfg.auto_label_record_classes.c_str());
        const std::set<int>* allowed_ptr = allowed.empty() ? nullptr : &allowed;
        label_result = WriteYoloLabelFile(
            label_path,
            boxes,
            classes,
            confidences,
            save_frame.cols,
            save_frame.rows,
            attempt.cfg.auto_label_min_conf,
            attempt.cfg.auto_label_max_boxes,
            allowed_ptr);
        label_ok = (label_result != "label open failed");
    }

    {
        std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
        g_collectRuntimeState.saved_image_count += 1;
        if (attempt.cfg.auto_label_data && label_ok)
            g_collectRuntimeState.saved_label_count += 1;
        g_collectRuntimeState.last_output_dir = model_root.u8string();
        g_collectRuntimeState.last_status = attempt.cfg.auto_label_data
            ? ("Saved image + " + label_result)
            : "Saved image only";
    }
    }
    catch (...)
    {
        // 编码/目录创建/标签写入等环节可能抛 cv::Exception 或 bad_alloc；
        // 推理线程（尤其 DML 路径，其 catch 在循环外）若不加捕获会终止整个推理线程，
        // 故在此兜底：记录失败状态并安全返回，不影响实时推理主循环。
        UpdateRuntimeStatus(std::string(), "Collect save failed: exception.");
        return;
    }
}

}  // namespace

std::filesystem::path ResolveCollectOutputDir(const std::string& root_dir, const char* output_dir_raw)
{
    const std::string cleaned = OtherTools::TrimAscii(output_dir_raw ? std::string(output_dir_raw) : std::string());
    if (cleaned.empty())
    {
        const std::string base_dir = root_dir.empty() ? GetExecutableDir() : root_dir;
        return fs::path(base_dir) / "screenshots";
    }

    fs::path out(cleaned);
    if (out.is_absolute())
        return out;

    const std::string base_dir = root_dir.empty() ? GetExecutableDir() : root_dir;
    return fs::path(base_dir) / out;
}

bool IsDataCollectionEnabled(const Config& cfg)
{
    return cfg.collect_data_while_playing;
}

DataCollectionUiState GetDataCollectionUiState(const std::string& root_dir, const char* model_name, const Config& cfg)
{
    DataCollectionUiState ui;
    ui.enabled = IsDataCollectionEnabled(cfg);

    const CollectConfigSnapshot snapshot = SnapshotCollectConfig(cfg);
    const fs::path model_root = ResolveCollectOutputDir(root_dir, snapshot.output_dir.c_str()) / ModelNameToFolder(model_name);
    // 用 u8string() 输出 UTF-8，避免含中文的目录名经本地编码(.string())传给 ImGui 显示乱码
    ui.resolved_output_dir = model_root.u8string();

    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    ui.observed_frame_count = g_collectRuntimeState.frame_counter;
    ui.attempted_sample_count = g_collectRuntimeState.sample_counter;
    ui.saved_image_count = g_collectRuntimeState.saved_image_count;
    ui.saved_label_count = g_collectRuntimeState.saved_label_count;
    ui.status = g_collectRuntimeState.last_status;
    return ui;
}

void ResetDataCollectionRuntime()
{
    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    g_collectRuntimeState = {};
    g_collectRuntimeState.last_status = "Counters reset.";
}

void MaybeCollectDataSample(const std::string& root_dir,
                            const char* model_name,
                            const cv::Mat& frame,
                            const std::vector<cv::Rect>& boxes,
                            const std::vector<int>& classes,
                            const std::vector<float>& confidences,
                            bool aimbot_enabled,
                            const Config& cfg)
{
    if (frame.empty() || frame.cols <= 0 || frame.rows <= 0)
        return;

    // 在 configMutex 保护下完成配置快照，消除与 overlay 线程的数据竞争。
    //
    // 竞争说明：cfg.collect_output_dir / cfg.auto_label_record_classes 是 std::string，
    // 由 overlay 线程在 draw_debug.cpp 的 InputText 回调中改写（该路径在 overlay.cpp
    // 渲染 Tab 内容时已持有 configMutex）。而本函数运行在 TensorRT/DML 推理线程，
    // 原先完全无锁地拷贝这两个 string —— 一旦用户正在编辑"保存目录"输入框，
    // 推理线程就可能读到被撕裂的 SSO 缓冲/堆指针，造成堆越界读或 double-free 崩溃。
    // 该崩溃随机出现且栈回溯落在 std::string 内部，极难定位。
    //
    // 为什么不在 SnapshotCollectConfig() 内部加锁（重要）：
    // GetDataCollectionUiState() 也会调用 SnapshotCollectConfig()，而它由 draw_debug.cpp
    // 在 overlay 线程调用，此时 overlay.cpp 已经持有 configMutex。std::mutex 不可重入，
    // 在 SnapshotCollectConfig 内加锁会让 UI 一切到"调试"页就整个死锁卡死 —— 比原缺陷
    // 严重得多。因此锁必须加在"确定不持锁"的调用方，即本函数。
    //
    // 锁的持有范围严格限制在快照期间：后续的目录创建、PNG 编码、磁盘写入（5-20ms）
    // 全部在锁外执行，不会阻塞 overlay 渲染线程。
    CollectConfigSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        snapshot = SnapshotCollectConfig(cfg);
    }

    std::uint64_t sample_id = 0;
    if (!TryBeginCollectAttempt(snapshot, boxes, aimbot_enabled, sample_id))
        return;

    SaveCollectedFrame(
        root_dir,
        model_name,
        frame,
        boxes,
        classes,
        confidences,
        CollectAttempt{ std::move(snapshot), sample_id });
}

#ifdef USE_CUDA
void MaybeCollectDataSample(const std::string& root_dir,
                            const char* model_name,
                            const cv::cuda::GpuMat& frame,
                            const std::vector<cv::Rect>& boxes,
                            const std::vector<int>& classes,
                            const std::vector<float>& confidences,
                            bool aimbot_enabled,
                            const Config& cfg)
{
    if (frame.empty())
        return;

    // 加锁理由与上面的 cv::Mat 重载完全相同（消除与 overlay 线程的 std::string 竞争，
    // 且不能把锁下沉到 SnapshotCollectConfig，否则 UI 侧会自死锁）。
    // 锁只覆盖快照，GPU 下载与磁盘写入均在锁外。
    CollectConfigSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        snapshot = SnapshotCollectConfig(cfg);
    }

    std::uint64_t sample_id = 0;
    if (!TryBeginCollectAttempt(snapshot, boxes, aimbot_enabled, sample_id))
        return;

    cv::Mat downloaded;
    try
    {
        frame.download(downloaded);
    }
    catch (...)
    {
        UpdateRuntimeStatus("", "Collect save failed: GPU download.");
        return;
    }

    SaveCollectedFrame(
        root_dir,
        model_name,
        downloaded,
        boxes,
        classes,
        confidences,
        CollectAttempt{ std::move(snapshot), sample_id });
}
#endif

}  // namespace cvm

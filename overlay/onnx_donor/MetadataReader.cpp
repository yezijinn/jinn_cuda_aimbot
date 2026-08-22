#include "MetadataReader.h"

#include <string>

namespace {

std::string ToString(const Ort::AllocatedStringPtr& p) {
    return (p && p.get()) ? std::string(p.get()) : std::string();
}

}  // namespace

bool MetadataReader::Read(Ort::Session& session, ModelReport& report) {
    Ort::AllocatorWithDefaultOptions allocator;
    Ort::ModelMetadata meta = session.GetModelMetadata();

    report.producer = ToString(meta.GetProducerNameAllocated(allocator));
    report.graph_name = ToString(meta.GetGraphNameAllocated(allocator));
    report.domain = ToString(meta.GetDomainAllocated(allocator));
    report.graph_description = ToString(meta.GetDescriptionAllocated(allocator));

    // 自定义 metadata（Ultralytics 通常写入 names/stride/task/date 等）
    auto keys = meta.GetCustomMetadataMapKeysAllocated(allocator);
    for (auto& key_ptr : keys) {
        std::string key = ToString(key_ptr);
        if (key.empty()) continue;
        auto val = meta.LookupCustomMetadataMapAllocated(key.c_str(), allocator);
        report.custom_metadata[key] = ToString(val);
    }

    return true;
}

#ifndef CAMERA_ENUMERATION_CACHE_H
#define CAMERA_ENUMERATION_CACHE_H

#include <utility>
#include <vector>

template <typename T>
class CameraEnumerationCache
{
public:
    template <typename Scanner>
    std::vector<T> Get(bool forceRescan, Scanner&& scanner)
    {
        if (forceRescan || !initialized_)
        {
            values_ = scanner();
            initialized_ = true;
        }
        return values_;
    }

    void Clear()
    {
        values_.clear();
        initialized_ = false;
    }

private:
    bool initialized_ = false;
    std::vector<T> values_;
};

#endif // CAMERA_ENUMERATION_CACHE_H

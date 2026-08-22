#include <iostream>
#include <vector>

#include "camera_enumeration_cache.h"

int main()
{
    CameraEnumerationCache<int> cache;
    int scans = 0;
    const auto scanNoDevices = [&]()
    {
        ++scans;
        return std::vector<int>{};
    };

    cache.Get(false, scanNoDevices);
    cache.Get(false, scanNoDevices);
    if (scans != 1)
    {
        std::cerr << "FAIL: an empty camera result must be cached" << std::endl;
        return 1;
    }

    cache.Get(true, scanNoDevices);
    if (scans != 2)
    {
        std::cerr << "FAIL: an explicit refresh must rescan devices" << std::endl;
        return 1;
    }

    cache.Clear();
    cache.Get(false, scanNoDevices);
    if (scans != 3)
    {
        std::cerr << "FAIL: clearing the cache must permit a rescan" << std::endl;
        return 1;
    }

    std::cout << "PASS: empty camera enumerations are cached until refresh" << std::endl;
    return 0;
}

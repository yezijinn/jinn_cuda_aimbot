#include "runtime/thread_loops.h"

std::mutex g_trackerDebugMutex;
std::vector<TrackDebugInfo> g_trackerDebugTracks;
int g_trackerLockedId = -1;
std::atomic<double> g_dynamicEffectiveFov{-1.0};

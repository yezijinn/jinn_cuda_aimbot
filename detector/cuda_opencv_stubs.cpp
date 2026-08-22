#include <opencv2/core/cuda_stream_accessor.hpp>
#include <opencv2/core/cuda.hpp>
#include <cuda_runtime.h>

namespace cv { namespace cuda {

Stream StreamAccessor::wrapStream(cudaStream_t stream)
{
    return cv::cuda::wrapStream(reinterpret_cast<size_t>(stream));
}

}} // namespace cv::cuda

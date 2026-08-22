#ifdef USE_CUDA
#include <cuda_runtime.h>
#include <iostream>

static __global__ void hwc_to_chw_norm_kernel(
    const float* __restrict__ srcHwc, int srcStepFloats,
    float* __restrict__ dstChw,
    int width, int height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const int hw = height * width;
    const int idx = y * width + x;

    const float* p = srcHwc + y * srcStepFloats + x * 3;

    dstChw[0 * hw + idx] = p[2]; // R from BGR source
    dstChw[1 * hw + idx] = p[1]; // G
    dstChw[2 * hw + idx] = p[0]; // B from BGR source
}

void launch_hwc_to_chw_norm(
    const float* srcHwc,
    size_t srcStepBytes,
    float* dstChw,
    int width,
    int height,
    cudaStream_t stream)
{
    // Validate parameters, matching launch_bgra_resize_to_rgb_nchw.
    // Without this check a null pointer or bad size causes an illegal memory access
    // that poisons the whole CUDA context and makes every later inference call fail.
    if (!srcHwc || !dstChw || width <= 0 || height <= 0 ||
        srcStepBytes < static_cast<size_t>(width) * 3 * sizeof(float))
    {
        std::cerr << "[CUDA Preprocess] Invalid HWC->CHW preprocessing parameters." << std::endl;
        return;
    }

    const dim3 block(16, 16);
    const dim3 grid(
        (static_cast<unsigned int>(width) + block.x - 1) / block.x,
        (static_cast<unsigned int>(height) + block.y - 1) / block.y);

    const int stepFloats = static_cast<int>(srcStepBytes / sizeof(float));

    hwc_to_chw_norm_kernel << <grid, block, 0, stream >> > (
        srcHwc, stepFloats, dstChw, width, height
        );

    // The original code never checked the launch status, so a failed launch was
    // swallowed silently and only surfaced on the next unrelated CUDA call.
    const cudaError_t error = cudaGetLastError();
    if (error != cudaSuccess)
    {
        std::cerr << "[CUDA Preprocess] HWC->CHW kernel launch failed: "
                  << cudaGetErrorString(error) << std::endl;
    }
}

static __global__ void bgra_resize_to_rgb_nchw_kernel(
    const unsigned char* __restrict__ source,
    size_t sourceStepBytes,
    int sourceWidth,
    int sourceHeight,
    float* __restrict__ destination,
    int destinationWidth,
    int destinationHeight)
{
    const int destinationX = blockIdx.x * blockDim.x + threadIdx.x;
    const int destinationY = blockIdx.y * blockDim.y + threadIdx.y;
    if (destinationX >= destinationWidth || destinationY >= destinationHeight)
        return;

    const float sourceX = fminf(
        fmaxf((static_cast<float>(destinationX) + 0.5f) * sourceWidth / destinationWidth - 0.5f, 0.0f),
        static_cast<float>(sourceWidth - 1));
    const float sourceY = fminf(
        fmaxf((static_cast<float>(destinationY) + 0.5f) * sourceHeight / destinationHeight - 0.5f, 0.0f),
        static_cast<float>(sourceHeight - 1));
    const int x0 = static_cast<int>(sourceX);
    const int y0 = static_cast<int>(sourceY);
    const int x1 = min(x0 + 1, sourceWidth - 1);
    const int y1 = min(y0 + 1, sourceHeight - 1);
    const float xWeight = sourceX - static_cast<float>(x0);
    const float yWeight = sourceY - static_cast<float>(y0);

    const uchar4 topLeft = *reinterpret_cast<const uchar4*>(source + y0 * sourceStepBytes + x0 * 4);
    const uchar4 topRight = *reinterpret_cast<const uchar4*>(source + y0 * sourceStepBytes + x1 * 4);
    const uchar4 bottomLeft = *reinterpret_cast<const uchar4*>(source + y1 * sourceStepBytes + x0 * 4);
    const uchar4 bottomRight = *reinterpret_cast<const uchar4*>(source + y1 * sourceStepBytes + x1 * 4);
    const float topLeftWeight = (1.0f - xWeight) * (1.0f - yWeight);
    const float topRightWeight = xWeight * (1.0f - yWeight);
    const float bottomLeftWeight = (1.0f - xWeight) * yWeight;
    const float bottomRightWeight = xWeight * yWeight;
    const int pixelIndex = destinationY * destinationWidth + destinationX;
    const int channelSize = destinationWidth * destinationHeight;
    constexpr float kNormalize = 1.0f / 255.0f;

    destination[pixelIndex] = (topLeftWeight * topLeft.z + topRightWeight * topRight.z +
        bottomLeftWeight * bottomLeft.z + bottomRightWeight * bottomRight.z) * kNormalize;
    destination[channelSize + pixelIndex] = (topLeftWeight * topLeft.y + topRightWeight * topRight.y +
        bottomLeftWeight * bottomLeft.y + bottomRightWeight * bottomRight.y) * kNormalize;
    destination[2 * channelSize + pixelIndex] = (topLeftWeight * topLeft.x + topRightWeight * topRight.x +
        bottomLeftWeight * bottomLeft.x + bottomRightWeight * bottomRight.x) * kNormalize;
}

bool launch_bgra_resize_to_rgb_nchw(
    const unsigned char* source,
    size_t sourceStepBytes,
    int sourceWidth,
    int sourceHeight,
    float* destination,
    int destinationWidth,
    int destinationHeight,
    cudaStream_t stream)
{
    if (!source || !destination || sourceWidth <= 0 || sourceHeight <= 0 ||
        destinationWidth <= 0 || destinationHeight <= 0 ||
        sourceStepBytes < static_cast<size_t>(sourceWidth) * 4)
    {
        std::cerr << "[CUDA Preprocess] Invalid BGRA preprocessing parameters." << std::endl;
        return false;
    }

    const dim3 block(16, 16);
    const dim3 grid(
        (static_cast<unsigned int>(destinationWidth) + block.x - 1) / block.x,
        (static_cast<unsigned int>(destinationHeight) + block.y - 1) / block.y);
    bgra_resize_to_rgb_nchw_kernel<<<grid, block, 0, stream>>>(
        source,
        sourceStepBytes,
        sourceWidth,
        sourceHeight,
        destination,
        destinationWidth,
        destinationHeight);

    const cudaError_t error = cudaGetLastError();
    if (error != cudaSuccess)
    {
        std::cerr << "[CUDA Preprocess] Kernel launch failed: " << cudaGetErrorString(error) << std::endl;
        return false;
    }
    return true;
}
#endif

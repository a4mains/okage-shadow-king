#include "okage/texture_address_batch.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace okage {
namespace {
constexpr const char metal_shader_source[] = R"OKAGE_METAL(
#include <metal_stdlib>
using namespace metal;
struct Query { uint psm, tbp, tbw, x, y; };
struct Result { uint address, valid; };
static_assert(sizeof(Query) == 20, "query stride");
static_assert(sizeof(Result) == 8, "result stride");
uint spread3(uint value) {
    value &= 7u;
    return (value & 1u) | ((value & 2u) << 1u) | ((value & 4u) << 2u);
}
uint spread_column_x_low(uint value) {
    return (value & 1u) | ((value & 2u) << 1u);
}
uint block_number(uint bx, uint by) {
    return spread3(bx) | ((by & 1u) << 1u) | ((by & 2u) << 2u);
}
uint page_base(uint tbp, uint page) {
    return (tbp * 256u + page * 8192u) & 0x3fffffu;
}
uint address32(Query q) {
    const uint page = q.x / 64u + (q.y / 32u) * q.tbw;
    const uint px = q.x % 64u, py = q.y % 32u;
    const uint block = block_number(px / 8u, py / 8u);
    const uint ix = px % 8u, iy = py % 8u;
    const uint column_word = spread_column_x_low(ix) | ((iy & 1u) << 1u) |
        ((ix & 4u) << 1u);
    const uint band = (iy / 2u) * 64u;
    return (page_base(q.tbp, page) + block * 256u + band + column_word * 4u) & 0x3fffffu;
}
uint address8(Query q) {
    // Preserve floor(page_y * TBW / 2), including odd and zero raw TBW.
    const uint page = q.x / 128u + ((q.y / 64u) * (q.tbw * 64u)) / 128u;
    const uint px = q.x % 128u, py = q.y % 64u;
    const uint block = block_number(px / 16u, py / 16u);
    const uint ix = px % 16u, iy = py % 16u;
    const uint band = (iy / 4u) * 64u, local_y = iy % 4u;
    const uint table_bit = ((local_y >> 1u) ^ (py >> 2u)) & 1u;
    const uint column_word = spread_column_x_low(ix) | ((local_y & 1u) << 1u) |
        ((table_bit ^ ((ix >> 2u) & 1u)) << 3u);
    const uint byte_lane = ((ix & 8u) >> 2u) | ((local_y & 2u) >> 1u);
    return (page_base(q.tbp, page) + block * 256u + band + column_word * 4u + byte_lane) & 0x3fffffu;
}
kernel void texture_address_batch(device const Query* queries [[buffer(0)]],
                                  device Result* results [[buffer(1)]],
                                  constant uint& count [[buffer(2)]],
                                  uint tid [[thread_position_in_grid]]) {
    if (tid >= count) return;
    const Query q = queries[tid];
    Result result = {0u, 0u};
    if (q.tbp <= 0x3fffu && q.tbw <= 0x3fu && q.x <= 2047u && q.y <= 2047u &&
        (q.psm == 0u || q.psm == 19u)) {
        result.address = q.psm == 0u ? address32(q) : address8(q);
        result.valid = 1u;
    }
    // Element zero is a prefix guard. The trailing guard starts at count+1.
    results[tid + 1u] = result;
}
)OKAGE_METAL";

std::runtime_error metal_error(const char* operation, NSError* error = nil) {
    std::string message(operation);
    if (error != nil) {
        const char* detail = [[error localizedDescription] UTF8String];
        if (detail != nullptr) message += std::string(": ") + detail;
    }
    return std::runtime_error(message);
}

class MetalTextureAddressBatch final : public TextureAddressBatchBackend {
public:
    MetalTextureAddressBatch(std::size_t capacity, bool diagnostic)
        : capacity_(capacity), diagnostic_(diagnostic) {
        // This also checks byte-size arithmetic before any Metal allocation.
        validate_texture_address_batch(capacity, capacity, capacity);
        @autoreleasepool {
            device_ = MTLCreateSystemDefaultDevice();
            if (device_ == nil) throw metal_error("Metal device unavailable");
            queue_ = [device_ newCommandQueue];
            if (queue_ == nil) throw metal_error("Metal command queue allocation failed");
            NSError* error = nil;
            NSString* source = [[NSString alloc] initWithBytes:metal_shader_source
                length:sizeof(metal_shader_source) - 1 encoding:NSUTF8StringEncoding];
            if (source == nil) throw metal_error("Metal source string allocation failed");
            MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
            id<MTLLibrary> library = [device_ newLibraryWithSource:source options:options error:&error];
            if (library == nil) throw metal_error("Metal shader compilation failed", error);
            id<MTLFunction> function = [library newFunctionWithName:@"texture_address_batch"];
            if (function == nil) throw metal_error("Metal kernel function unavailable");
            pipeline_ = [device_ newComputePipelineStateWithFunction:function error:&error];
            if (pipeline_ == nil) throw metal_error("Metal compute pipeline creation failed", error);
            const auto width = pipeline_.threadExecutionWidth;
            const auto maximum = pipeline_.maxTotalThreadsPerThreadgroup;
            if (width == 0 || maximum < width)
                throw metal_error("Metal pipeline has invalid threadgroup limits");
            threads_ = std::min<NSUInteger>(256, maximum);
            threads_ = (threads_ / width) * width;
            if (threads_ == 0) threads_ = width;
            const std::size_t input_bytes = std::max<std::size_t>(capacity_, 1) * sizeof(TextureAddressQuery);
            const std::size_t output_bytes = (capacity_ + 2) * sizeof(TextureAddressResult);
            if (input_bytes > device_.maxBufferLength || output_bytes > device_.maxBufferLength)
                throw metal_error("Metal buffer length exceeds device limit");
            input_ = [device_ newBufferWithLength:input_bytes options:MTLResourceStorageModeShared];
            output_ = [device_ newBufferWithLength:output_bytes options:MTLResourceStorageModeShared];
            if (input_ == nil || output_ == nil || input_.contents == nullptr || output_.contents == nullptr)
                throw metal_error("Metal shared buffer allocation failed");
            const char* device_name = [device_.name UTF8String];
            name_ = std::string("Metal: ") + (device_name == nullptr ? "unnamed device" : device_name);
        }
    }

    std::string name() const override { return name_; }

    TextureAddressBatchTiming execute(std::span<const TextureAddressQuery> input,
                                       std::span<TextureAddressResult> output) override {
        validate_texture_address_batch(input.size(), output.size(), capacity_);
        if (input.empty()) return {};
        @autoreleasepool {
            std::memcpy(input_.contents, input.data(), input.size_bytes());
            auto* results = static_cast<TextureAddressResult*>(output_.contents);
            constexpr TextureAddressResult poison{0xfedcba98u, 0xdeadbeefu};
            if (diagnostic_) std::fill_n(results, capacity_ + 2, poison);
            id<MTLCommandBuffer> command = [queue_ commandBuffer];
            if (command == nil) throw metal_error("Metal command buffer allocation failed");
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            if (encoder == nil) throw metal_error("Metal compute encoder allocation failed");
            const auto count = static_cast<std::uint32_t>(input.size());
            [encoder setComputePipelineState:pipeline_];
            [encoder setBuffer:input_ offset:0 atIndex:0];
            [encoder setBuffer:output_ offset:0 atIndex:1];
            [encoder setBytes:&count length:sizeof(count) atIndex:2];
            const auto groups = (input.size() + threads_ - 1) / threads_;
            [encoder dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(threads_, 1, 1)];
            [encoder endEncoding];
            [command commit];
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted)
                throw metal_error("Metal command did not complete successfully", command.error);
            if (diagnostic_) {
                if (!(results[0] == poison)) throw metal_error("Metal prefix guard overwritten");
                for (std::size_t i = input.size() + 1; i < capacity_ + 2; ++i)
                    if (!(results[i] == poison)) throw metal_error("Metal trailing guard overwritten");
                for (std::size_t i = 1; i <= input.size(); ++i) {
                    const auto result = results[i];
                    if (result.valid > 1 || (result.valid == 0 && result.address != 0) ||
                        (result.valid == 1 && result.address > 0x3fffffu))
                        throw metal_error("Metal missing or malformed result write");
                }
            }
            std::memcpy(output.data(), results + 1, input.size() * sizeof(TextureAddressResult));
            TextureAddressBatchTiming timing;
            const auto start = command.GPUStartTime;
            const auto end = command.GPUEndTime;
            if (std::isfinite(start) && std::isfinite(end) && start > 0 && end > start)
                timing.gpu_seconds = end - start;
            return timing;
        }
    }

private:
    const std::size_t capacity_;
    const bool diagnostic_;
    std::string name_;
    NSUInteger threads_ = 0;
    id<MTLDevice> __strong device_;
    id<MTLCommandQueue> __strong queue_;
    id<MTLComputePipelineState> __strong pipeline_;
    id<MTLBuffer> __strong input_;
    id<MTLBuffer> __strong output_;
};
} // namespace

std::unique_ptr<TextureAddressBatchBackend> make_metal_texture_address_batch(
    std::size_t capacity, bool diagnostic) {
    // Reject unsupported capacities before constructing/allocating a backend.
    if (capacity > texture_address_batch_limit)
        throw std::length_error("Metal texture-address capacity exceeds finite limit");
    validate_texture_address_batch(capacity, capacity, capacity);
    return std::make_unique<MetalTextureAddressBatch>(capacity, diagnostic);
}
} // namespace okage

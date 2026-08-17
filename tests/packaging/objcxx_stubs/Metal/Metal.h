// Metal stub — declarations only, for the off-Apple Objective-C++ syntax gate
// (tests/packaging/test_metal_syntax.py). See Foundation/Foundation.h for what
// this is and the rule it follows: mirror the real signatures exactly, because
// a stub laxer than the SDK cannot catch anything.
//
// Only the surface src/accel/src/metal_backend.mm actually touches is declared.
// A method the backend starts calling must be ADDED here (with the SDK's
// signature) before the gate will accept it — that is the point: the gate fails
// closed on a selector it has never seen, exactly as the SDK would on a typo.
#pragma once

#import <Foundation/Foundation.h>

typedef NSUInteger MTLResourceOptions;

typedef struct {
    NSUInteger width;
    NSUInteger height;
    NSUInteger depth;
} MTLSize;

static inline MTLSize MTLSizeMake(NSUInteger width, NSUInteger height, NSUInteger depth) {
    MTLSize size;
    size.width = width;
    size.height = height;
    size.depth = depth;
    return size;
}

@class MTLCompileOptions;

@protocol MTLBuffer <NSObject>
@property(readonly) void* contents;
@property(readonly) NSUInteger length;
@end

@protocol MTLFunction <NSObject>
@property(readonly) NSString* name;
@end

@protocol MTLComputePipelineState <NSObject>
@property(readonly) NSUInteger maxTotalThreadsPerThreadgroup;
@end

@protocol MTLLibrary <NSObject>
- (id<MTLFunction>)newFunctionWithName:(NSString*)functionName;
@end

@protocol MTLComputeCommandEncoder <NSObject>
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state;
- (void)setBytes:(const void*)bytes length:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)dispatchThreads:(MTLSize)threadsPerGrid
  threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup;
- (void)endEncoding;
@end

@protocol MTLCommandBuffer <NSObject>
- (id<MTLComputeCommandEncoder>)computeCommandEncoder;
- (void)commit;
- (void)waitUntilCompleted;
@end

@protocol MTLCommandQueue <NSObject>
- (id<MTLCommandBuffer>)commandBuffer;
@end

@protocol MTLDevice <NSObject>
@property(readonly) NSString* name;
- (id<MTLCommandQueue>)newCommandQueue;
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options;
- (id<MTLBuffer>)newBufferWithBytes:(const void*)pointer
                             length:(NSUInteger)length
                            options:(MTLResourceOptions)options;
- (id<MTLLibrary>)newLibraryWithSource:(NSString*)source
                               options:(MTLCompileOptions*)options
                                 error:(NSError**)error;
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)function
                                                             error:(NSError**)error;
@end

id<MTLDevice> MTLCreateSystemDefaultDevice(void);

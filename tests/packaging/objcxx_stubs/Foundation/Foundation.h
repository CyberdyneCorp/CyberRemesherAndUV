// Foundation stub — declarations only, for the off-Apple Objective-C++ syntax
// gate (tests/packaging/test_metal_syntax.py). It exists so a Linux clang can
// PARSE src/accel/src/metal_backend.mm, which no CI lane can otherwise feed to
// any compiler at all.
//
// It is deliberately NOT a re-implementation: nothing here has a body, and the
// gate only ever runs -fsyntax-only. Every declaration mirrors the real
// signature, because a stub that is laxer than the SDK turns the gate into a
// rubber stamp — an argument type or a return type invented here would let a
// mismatch through. Keep it that way when adding to it.
#pragma once

#if !defined(__OBJC__)
#error "Foundation stub included outside Objective-C/C++"
#endif

// objc/objc.h, which the real Foundation pulls in.
#if !defined(nil)
#define nil nullptr
#endif
#if !defined(Nil)
#define Nil nullptr
#endif

typedef signed char BOOL;
#define YES ((BOOL)1)
#define NO ((BOOL)0)

typedef signed long NSInteger;
typedef unsigned long NSUInteger;

@protocol NSObject
- (instancetype)self;
@end

@interface NSObject <NSObject>
+ (instancetype)alloc;
- (instancetype)init;
@end

@interface NSString : NSObject
+ (NSString*)stringWithUTF8String:(const char*)bytes;
@property(readonly) const char* UTF8String;
@end

@interface NSError : NSObject
@property(readonly) NSInteger code;
@property(readonly) NSString* localizedDescription;
@end

@interface NSDictionary : NSObject
@end

// macOS support helpers for SWE1Racer (Objective-C++)
//
// SDL's Metal view (SDL_cocoametalview) resizes the drawable of its backing
// CAMetalLayer every time the NSView's frame changes (-updateDrawableSize).
// RT64 owns the swap chain and sizes the drawable itself from the window's
// size, so SDL doing it too races with RT64 and can hand Metal a drawable
// whose size disagrees with what RT64 just configured. We neutralize SDL's
// version by swapping its implementation with a no-op at load time.
//
// Every lookup is guarded: if the SDL build in use names the class or method
// differently (or strips it), this quietly does nothing.

#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <cstdio>

namespace {

void swe1r_noop_updateDrawableSize(id /*self*/, SEL /*_cmd*/) {
    // Intentionally empty: RT64 manages the CAMetalLayer drawable size.
}

void swe1r_disable_sdl_metal_view_resize() {
    // SDL2 names the view SDL_cocoametalview; sdl2-compat on top of SDL3
    // (Homebrew's default) names it SDL3_cocoametalview.
    Class view_class = objc_getClass("SDL3_cocoametalview");
    if (view_class == nil) {
        view_class = objc_getClass("SDL_cocoametalview");
    }
    if (view_class == nil) {
        fprintf(stderr, "[SWE1R] SDL metal view class not found; drawable resize swizzle skipped\n");
        return;
    }

    SEL target_sel = sel_registerName("updateDrawableSize");
    Method target_method = class_getInstanceMethod(view_class, target_sel);
    if (target_method == nullptr) {
        return;
    }

    const char* type_encoding = method_getTypeEncoding(target_method);
    if (type_encoding == nullptr) {
        return;
    }

    // Replace the implementation in place. class_replaceMethod returns the
    // previous IMP (which we discard) or NULL if it had to add the method;
    // either way the view now ignores drawable-size updates.
    class_replaceMethod(view_class, target_sel,
                        (IMP)swe1r_noop_updateDrawableSize, type_encoding);
    fprintf(stderr, "[SWE1R] %s.updateDrawableSize neutralized (RT64 owns the drawable size)\n",
            class_getName(view_class));
}

} // namespace

// Pin an SDL-created CAMetalLayer to a 1:1 pixel/point scale. RT64 is built
// without Retina support and sizes its swap chain in window points; SDL's
// Metal view defaults to the backing scale (2x on Retina), which left the
// picture rendered at half the window. Called from create_window with the
// layer returned by SDL_Metal_GetLayer.
extern "C" void swe1r_pin_metal_layer_scale(void* layer_ptr) {
    if (layer_ptr == nullptr) return;
    CAMetalLayer* layer = (__bridge CAMetalLayer*)layer_ptr;
    layer.contentsScale = 1.0;
    CGSize bounds = layer.bounds.size;
    layer.drawableSize = CGSizeMake(bounds.width, bounds.height);
    fprintf(stderr, "[SWE1R] CAMetalLayer pinned to scale 1 (%.0fx%.0f)\n", bounds.width, bounds.height);
}

// Runs when the executable is loaded, before main(), so the swizzle is in
// place before SDL creates any Metal view.
__attribute__((constructor))
static void swe1r_apple_support_init() {
    @autoreleasepool {
        swe1r_disable_sdl_metal_view_resize();
    }
}

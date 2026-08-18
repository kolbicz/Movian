/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#import <CoreVideo/CVMetalTextureCache.h>

#include "main.h"
#include "osx.h"
#include "osx_c.h"
#include "src/ui/glw/glw.h"

CVPixelBufferRef
osx_metal_convert_yuv(CVPixelBufferRef source, int transfer,
                      float hdr_peak, float edr_headroom)
{
  static id<MTLDevice> device;
  static id<MTLCommandQueue> queue;
  static id<MTLComputePipelineState> pipeline;
  static CVMetalTextureCacheRef cache;
  static CVPixelBufferPoolRef pool;
  static size_t pool_width;
  static size_t pool_height;
  static dispatch_once_t once;

  dispatch_once(&once, ^{
    device = MTLCreateSystemDefaultDevice();
    queue = [device newCommandQueue];
    if(device == nil || queue == nil)
      return;

    NSString *shader =
      @"#include <metal_stdlib>\n"
       "using namespace metal;\n"
       "struct Params { int transfer; int depth; float hdrPeak; float headroom; };\n"
       "float3 yuv2020(float y,float2 uv,int d){ if(d==8){y=(y-16.0/255.0)*(255.0/219.0);uv=(uv-float2(128.0/255.0))*(255.0/224.0);}else{y=(y-64.0/1023.0)*(1023.0/876.0);uv=(uv-float2(512.0/1023.0))*(1023.0/896.0);} return float3(y+1.4746*uv.y,y-.164553*uv.x-.571353*uv.y,y+1.8814*uv.x);}\n"
       "float3 yuv709(float y,float2 uv,int d){ if(d==8){y=(y-16.0/255.0)*(255.0/219.0);uv=(uv-float2(128.0/255.0))*(255.0/224.0);}else{y=(y-64.0/1023.0)*(1023.0/876.0);uv=(uv-float2(512.0/1023.0))*(1023.0/896.0);} return float3(y+1.7927*uv.y,y-.2132*uv.x-.5329*uv.y,y+2.1124*uv.x);}\n"
       "float3 hlg(float3 e){const float a=.17883277,b=.28466892,c=.55991073; return select(e*e/3.0,(exp((e-c)/a)+b)/12.0,e>=.5);}\n"
       "float3 pq(float3 e){const float m1=.1593017578125,m2=78.84375,c1=.8359375,c2=18.8515625,c3=18.6875; float3 p=pow(max(e,0.0),1.0/m2); return pow(max(p-c1,0.0)/max(c2-c3*p,.00001),1.0/m1);}\n"
       "float3 gamut(float3 c){return float3(1.6605*c.r-.5876*c.g-.0728*c.b,-.1246*c.r+1.1329*c.g-.0083*c.b,-.0182*c.r-.1006*c.g+1.1187*c.b);}\n"
       "float3 srgb(float3 x){return select(12.92*x,1.055*pow(max(x,0.0),1.0/2.4)-.055,x>.0031308);}\n"
       "float3 peakmap(float3 rgb,float src,float dst){float l=dot(rgb,float3(.2126,.7152,.0722)),k=min(1.0,dst*.75); if(l<=k||src<=dst)return rgb; float t=clamp((l-k)/max(src-k,.001),0.0,1.0); float m=k+(dst-k)*(1.0-exp(-3.0*t))/(1.0-exp(-3.0)); return rgb*(m/max(l,.0001));}\n"
       "kernel void convert(texture2d<float,access::sample> y [[texture(0)]],texture2d<float,access::sample> uv [[texture(1)]],texture2d<half,access::write> out [[texture(2)]],constant Params&p [[buffer(0)]],uint2 q [[thread_position_in_grid]]){if(q.x>=out.get_width()||q.y>=out.get_height())return; constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear); float2 t=(float2(q)+.5)/float2(out.get_width(),out.get_height()); float3 c=max((p.transfer==16||p.transfer==18)?yuv2020(y.sample(s,t).r,uv.sample(s,t).rg,p.depth):yuv709(y.sample(s,t).r,uv.sample(s,t).rg,p.depth),0.0); if(p.transfer==16)c=srgb(peakmap(max(gamut(pq(c)*100.0),0.0),max(1.0,p.hdrPeak/100.0),p.headroom)); else if(p.transfer==18)c=srgb(peakmap(max(gamut(pow(hlg(c),1.2)*4.0),0.0),10.0,p.headroom)); out.write(half4(half3(min(c,p.headroom)),half(1.0)),q);}\n";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:shader
                                                  options:nil error:&error];
    id<MTLFunction> function = [library newFunctionWithName:@"convert"];
    pipeline = function != nil ?
      [device newComputePipelineStateWithFunction:function error:&error] : nil;
    if(pipeline != nil)
      CVMetalTextureCacheCreate(NULL, NULL, device, NULL, &cache);
    if(pipeline == nil)
      TRACE(TRACE_ERROR, "Metal", "Unable to compile macOS P010 pipeline: %s",
            error != nil ? [[error description] UTF8String] : "unknown error");
  });

  if(source == NULL || pipeline == nil || cache == NULL)
    return NULL;

  const size_t width = CVPixelBufferGetWidth(source);
  const size_t height = CVPixelBufferGetHeight(source);
  if(pool == NULL || width != pool_width || height != pool_height) {
    if(pool != NULL)
      CVPixelBufferPoolRelease(pool);
    NSDictionary *attrs = @{
      (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_64RGBAHalf),
      (id)kCVPixelBufferWidthKey: @(width),
      (id)kCVPixelBufferHeightKey: @(height),
      (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
      (id)kCVPixelBufferMetalCompatibilityKey: @YES,
      (id)kCVPixelBufferOpenGLCompatibilityKey: @YES
    };
    if(CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs,
                               &pool) != kCVReturnSuccess)
      pool = NULL;
    pool_width = width;
    pool_height = height;
  }

  CVPixelBufferRef output = NULL;
  if(pool == NULL || CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &output))
    return NULL;

  const OSType source_format = CVPixelBufferGetPixelFormatType(source);
  const BOOL nv12 = source_format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
                    source_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
  const MTLPixelFormat y_format = nv12 ? MTLPixelFormatR8Unorm :
                                         MTLPixelFormatR16Unorm;
  const MTLPixelFormat uv_format = nv12 ? MTLPixelFormatRG8Unorm :
                                          MTLPixelFormatRG16Unorm;
  CVMetalTextureRef yref = NULL, uvref = NULL, outref = NULL;
  CVReturn status = CVMetalTextureCacheCreateTextureFromImage(NULL, cache,
    source, NULL, y_format, width, height, 0, &yref);
  if(status == kCVReturnSuccess)
    status = CVMetalTextureCacheCreateTextureFromImage(NULL, cache, source,
      NULL, uv_format, width / 2, height / 2, 1, &uvref);
  if(status == kCVReturnSuccess)
    status = CVMetalTextureCacheCreateTextureFromImage(NULL, cache, output,
      NULL, MTLPixelFormatRGBA16Float, width, height, 0, &outref);

  if(status == kCVReturnSuccess) {
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:CVMetalTextureGetTexture(yref) atIndex:0];
    [encoder setTexture:CVMetalTextureGetTexture(uvref) atIndex:1];
    [encoder setTexture:CVMetalTextureGetTexture(outref) atIndex:2];
    struct { int transfer; int depth; float hdrPeak; float headroom; } params = {
      transfer, nv12 ? 8 : 10, hdr_peak >= 100.0f ? hdr_peak : 1000.0f,
      edr_headroom > 1.0f ? edr_headroom : 1.0f
    };
    [encoder setBytes:&params length:sizeof(params) atIndex:0];
    MTLSize group = MTLSizeMake(16, 16, 1);
    MTLSize groups = MTLSizeMake((width + 15) / 16, (height + 15) / 16, 1);
    [encoder dispatchThreadgroups:groups threadsPerThreadgroup:group];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if(command.status != MTLCommandBufferStatusCompleted)
      status = kCVReturnError;
  }

  if(yref != NULL) CFRelease(yref);
  if(uvref != NULL) CFRelease(uvref);
  if(outref != NULL) CFRelease(outref);
  if(status != kCVReturnSuccess) {
    CVPixelBufferRelease(output);
    return NULL;
  }
  return output;
}

@interface MovianTextField : NSTextField {
  NSTimeInterval activation_timestamp;
}
- (void)setActivationTimestamp:(NSTimeInterval)timestamp;
@end

@implementation MovianTextField

- (void)setActivationTimestamp:(NSTimeInterval)timestamp
{
  activation_timestamp = timestamp;
}

- (void)mouseDown:(NSEvent *)event
{
  [super mouseDown:event];
  /* The first click is delivered to GLW and creates this overlay. AppKit then
   * sees the second click as the new field's first click, so clickCount alone
   * cannot recognize a double-click across the control replacement. */
  if([event clickCount] >= 2 ||
     ([event timestamp] >= activation_timestamp &&
      [event timestamp] - activation_timestamp <= [NSEvent doubleClickInterval])) {
    NSTextView *editor = (NSTextView *)[self currentEditor];
    [editor selectAll:self];
  }
}

@end

@interface GLWView (hidden)

- (CVReturn)getFrameForTime:(const CVTimeStamp *)ot;
- (void)drawFrame;
- (void)openNativeEditorForWidget:(glw_t *)w
                             rect:(const glw_rect_t *)rect
                             text:(const char *)text
                         password:(int)password;

@end


#define _NSTabKey 9
#define _NSShiftTabKey 25 /* why other value when shift is pressed? */
#define _NSEnterKey 13
#define _NSBackspaceKey 127
#define _NSEscapeKey 27
#define _NSSpaceKey 32

static const struct {
  int key;
  int mod;
  int action1;
  int action2;
  int action3;
} keysym2action[] = {

  /* NSFunctionKeyMask is filtered out when matching mappings */

  { NSLeftArrowFunctionKey,   0,                ACTION_LEFT },
  { NSRightArrowFunctionKey,  0,                ACTION_RIGHT },
  { NSUpArrowFunctionKey,     0,                ACTION_UP },
  { NSDownArrowFunctionKey,   0,                ACTION_DOWN },


  { NSLeftArrowFunctionKey,   NSShiftKeyMask,   ACTION_MOVE_LEFT },
  { NSRightArrowFunctionKey,  NSShiftKeyMask,   ACTION_MOVE_RIGHT },
  { NSUpArrowFunctionKey,     NSShiftKeyMask,   ACTION_MOVE_UP },
  { NSDownArrowFunctionKey,   NSShiftKeyMask,   ACTION_MOVE_DOWN },

  { NSPageUpFunctionKey,      0, ACTION_PAGE_UP, ACTION_PREV_CHANNEL, ACTION_SKIP_BACKWARD },
  { NSPageDownFunctionKey,    0, ACTION_PAGE_DOWN, ACTION_NEXT_CHANNEL, ACTION_SKIP_FORWARD },
  { NSHomeFunctionKey,        0,                ACTION_TOP },
  { NSEndFunctionKey,         0,                ACTION_BOTTOM },

  { _NSShiftTabKey,           NSShiftKeyMask,   ACTION_FOCUS_PREV },

  { NSLeftArrowFunctionKey,   NSAlternateKeyMask, ACTION_NAV_BACK },
  { NSRightArrowFunctionKey,  NSAlternateKeyMask, ACTION_NAV_FWD },

  { NSLeftArrowFunctionKey,   NSControlKeyMask, ACTION_SKIP_BACKWARD },
  { NSRightArrowFunctionKey,  NSControlKeyMask, ACTION_SKIP_FORWARD },
  { NSUpArrowFunctionKey,     NSControlKeyMask, ACTION_VOLUME_UP },
  { NSDownArrowFunctionKey,   NSControlKeyMask, ACTION_VOLUME_DOWN },

  { NSLeftArrowFunctionKey,   NSControlKeyMask | NSShiftKeyMask, ACTION_SEEK_BACKWARD },
  { NSRightArrowFunctionKey,  NSControlKeyMask | NSShiftKeyMask , ACTION_SEEK_FORWARD },
  { NSDownArrowFunctionKey,   NSControlKeyMask | NSShiftKeyMask, ACTION_VOLUME_MUTE_TOGGLE },

  /* only used for fullscreen, in windowed mode we dont get events with
   * NSCommandKeyMask set */
  { '0',                      NSCommandKeyMask, ACTION_ZOOM_UI_RESET },
  { '+',                      NSCommandKeyMask, ACTION_ZOOM_UI_INCR },
  { '-',                      NSCommandKeyMask, ACTION_ZOOM_UI_DECR },
  { _NSEnterKey,              NSCommandKeyMask, ACTION_FULLSCREEN_TOGGLE },

  { _NSBackspaceKey,          0,                ACTION_BS, ACTION_NAV_BACK },
  { _NSEnterKey,              0,                ACTION_ENTER, ACTION_ACTIVATE},
  { _NSEnterKey,              NSShiftKeyMask,   ACTION_ITEMMENU },
  { _NSEscapeKey,             0,                ACTION_CANCEL,ACTION_NAV_BACK },
  { _NSTabKey,                0,                ACTION_FOCUS_NEXT},
};


@implementation GLWView


/**
 *
 */
static CVReturn
newframe(CVDisplayLinkRef displayLink, const CVTimeStamp *now,
	 const CVTimeStamp *outputTime, CVOptionFlags flagsIn,
	 CVOptionFlags *flagsOut, void *displayLinkContext)
{
  CVReturn result = [(GLWView *)displayLinkContext getFrameForTime:outputTime];
  return result;
}


static void
openosk(glw_root_t *gr, const char *title, const char *text, glw_t *w,
        int password)
{
  GLWView *view = (GLWView *)gr->gr_private;
  if(view == nil || w->glw_matrix == NULL)
    return;

  glw_rect_t rect;
  glw_project_matrix(&rect, w->glw_matrix, gr);
  NSString *initialText = text != NULL ? [NSString stringWithUTF8String:text] : @"";
  glw_ref(w);

  dispatch_async(dispatch_get_main_queue(), ^{
    glw_lock(gr);
    BOOL stillEditing = gr->gr_osk_widget == w;
    glw_unlock(gr);

    if(stillEditing)
      [view openNativeEditorForWidget:w rect:&rect text:[initialText UTF8String]
                              password:password];

    glw_lock(gr);
    glw_unref(w);
    glw_unlock(gr);
  });
}


/**
 *
 */
static void
glw_in_fullwindow(void *opaque, int val)
{
  GLWView *view = (GLWView *)opaque;
  view->in_full_window = val;
}


static void
glw_inhibit_display_sleep(void *opaque, int val)
{
  GLWView *view = (GLWView *)opaque;

  if(val && view->display_sleep_activity == nil) {
    view->display_sleep_activity =
      [[[NSProcessInfo processInfo]
        beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep |
                                 NSActivityIdleDisplaySleepDisabled
        reason:@"Movian video playback"] retain];
  } else if(!val && view->display_sleep_activity != nil) {
    [[NSProcessInfo processInfo] endActivity:view->display_sleep_activity];
    [view->display_sleep_activity release];
    view->display_sleep_activity = nil;
  }
}


/**
 *
 */
- (void)hideCursor
{
  glw_pointer_event_t gpe = {0};
  if(!is_key_window)
    return;

  if(cursor_hidden)
    return;

  cursor_hidden = YES;
  [NSCursor hide];

  gpe.type = GLW_POINTER_GONE;
  glw_pointer_event(gr, &gpe);
}


/**
 *
 */
- (void)showCursor
{
  hide_cursor_at = gr->gr_time_usec + GLW_CURSOR_AUTOHIDE_TIME;

  if(!cursor_hidden)
    return;

  cursor_hidden = NO;
  [NSCursor unhide];
}


/**
 *
 */
- (void)autoHideCursor
{
  if(cursor_hidden)
    return;

  if(gr->gr_time_usec > hide_cursor_at) {
    [self hideCursor];
  }
}


/**
 *
 */
- (CVReturn)getFrameForTime:(const CVTimeStamp *)ot
{
  gr->gr_framerate = ot->rateScalar * ot->videoTimeScale /
    ot->videoRefreshPeriod;

  gr->gr_frameduration = 1000000.0 * ot->videoRefreshPeriod /
    (ot->rateScalar * ot->videoTimeScale);

  prop_set_float(prop_create(gr->gr_prop_ui, "framerate"), gr->gr_framerate);

  [self drawFrame];

  return kCVReturnSuccess;
}

- (void)glwMouseEvent:(int)type event:(NSEvent*)event {
  NSPoint loc = [self convertPoint:[event locationInWindow] fromView:nil];
  loc = [self convertPointToBacking:loc];
  glw_pointer_event_t gpe;

  gpe.screen_x = (2.0 * loc.x / gr->gr_width) - 1;
  gpe.screen_y = (2.0 * loc.y / gr->gr_height) - 1;
  gpe.type = type;

  gpe.ts = [event timestamp] * 1000000.0;

  switch(type) {
  case GLW_POINTER_SCROLL:
    {
      NSPoint p = {[event deltaX], [event deltaY]};
      NSPoint p2 = [self convertPointToBacking:p];

      gpe.delta_x = p2.x / 8;
      gpe.delta_y = -p2.y / 8;
      break;
    }

  case GLW_POINTER_FINE_SCROLL:
    {
      NSPoint p = {[event deltaX], [event deltaY]};
      NSPoint p2 = [self convertPointToBacking:p];
      gpe.delta_x = p2.x * 4;
      gpe.delta_y = p2.y * -4;
      break;
    }
  }

  glw_lock(gr);
  glw_pointer_event(gr, &gpe);
  glw_unlock(gr);
}


- (int)nativeCursorPosition
{
  NSText *editor = [native_text_field currentEditor];
  if(editor == nil)
    return (int)([[native_text_field stringValue]
                  lengthOfBytesUsingEncoding:NSUTF32LittleEndianStringEncoding] /
                 sizeof(uint32_t));

  NSRange selection = [(NSTextView *)editor selectedRange];
  NSString *value = [native_text_field stringValue];
  NSUInteger position = MIN(selection.location, [value length]);
  NSString *prefix = [value substringToIndex:position];
  return (int)([prefix lengthOfBytesUsingEncoding:NSUTF32LittleEndianStringEncoding] /
               sizeof(uint32_t));
}


- (void)syncNativeTextToMovian
{
  if(native_text_field == nil || gr == NULL)
    return;

  const char *text = [[native_text_field stringValue] UTF8String];
  int cursor = [self nativeCursorPosition];
  glw_lock(gr);
  if(gr->gr_osk_widget != NULL)
    glw_gtb_set_edit_text(gr->gr_osk_widget, text, cursor);
  glw_unlock(gr);
}


- (void)controlTextDidChange:(NSNotification *)notification
{
  [self syncNativeTextToMovian];
}


- (BOOL)control:(NSControl *)control
       textView:(NSTextView *)textView
doCommandBySelector:(SEL)commandSelector
{
  if(commandSelector == @selector(insertNewline:)) {
    [self finishNativeEditing];
    event_t *e = event_create_action(ACTION_ENTER);
    prop_send_ext_event(eventSink, e);
    event_release(e);
    return YES;
  }

  if(commandSelector == @selector(cancelOperation:)) {
    [self finishNativeEditing];
    return YES;
  }
  if(commandSelector == @selector(insertTab:) ||
     commandSelector == @selector(insertBacktab:)) {
    BOOL backwards = commandSelector == @selector(insertBacktab:);
    [self finishNativeEditing];
    event_t *e = event_create_action(backwards ? ACTION_FOCUS_PREV :
                                                 ACTION_FOCUS_NEXT);
    prop_send_ext_event(eventSink, e);
    event_release(e);
    return YES;
  }
  return NO;
}


- (void)finishNativeEditing
{
  NSTextField *field = native_text_field;
  if(field == nil || native_edit_ending)
    return;

  native_edit_ending = YES;
  [self syncNativeTextToMovian];
  native_text_field = nil;
  [field setDelegate:nil];
  [[self window] endEditingFor:nil];
  [field removeFromSuperview];
  [field release];

  /* NSTextField installs a field editor as the window's first responder.
   * endEditing removes it but does not reliably return keyboard ownership to
   * the OpenGL view. This is especially visible after an SMB authentication
   * popup closes: the directory has GLW focus, yet no key events arrive until
   * the user clicks the window. */
  [[self window] makeFirstResponder:self];

  glw_lock(gr);
  if(gr->gr_osk_widget != NULL) {
    glw_gtb_set_native_editor(gr->gr_osk_widget, 0);
    glw_osk_close(gr);
  }
  glw_unlock(gr);
  native_edit_ending = NO;
}


- (void)openNativeEditorForWidget:(glw_t *)w
                             rect:(const glw_rect_t *)rect
                             text:(const char *)text
                         password:(int)password
{
  /* A deferred request may replace an editor after GLW has already switched
   * gr_osk_widget to the new target. Tear down only the Cocoa control here;
   * finishNativeEditing would incorrectly close that new GLW edit session. */
  if(native_text_field != nil) {
    NSTextField *oldField = native_text_field;
    native_text_field = nil;
    [oldField setDelegate:nil];
    [[self window] endEditingFor:nil];
    [oldField removeFromSuperview];
    [oldField release];
  }

  NSRect backingRect = NSMakeRect(rect->x1, gr->gr_height - rect->y2,
                                  rect->x2 - rect->x1,
                                  rect->y2 - rect->y1);
  NSRect frame = [self convertRectFromBacking:backingRect];
  if(frame.size.height < 28.0) {
    CGFloat center = NSMidY(frame);
    frame.size.height = 28.0;
    frame.origin.y = center - frame.size.height / 2.0;
  }
  frame = NSIntersectionRect(frame, [self bounds]);

  NSTextField *field = password ? [[NSSecureTextField alloc] initWithFrame:frame]
                                : [[MovianTextField alloc] initWithFrame:frame];
  if(!password)
    [(MovianTextField *)field setActivationTimestamp:
      [[NSProcessInfo processInfo] systemUptime]];
  [field setDelegate:self];
  [field setStringValue:text != NULL ? [NSString stringWithUTF8String:text] : @""];
  [field setFont:[NSFont systemFontOfSize:16.0]];
  [field setTextColor:[NSColor whiteColor]];
  [field setBackgroundColor:[NSColor colorWithCalibratedWhite:0.08 alpha:0.98]];
  [field setDrawsBackground:YES];
  /* Keep AppKit's native padding and vertical alignment, but force its dark
   * control appearance so Light Mode cannot replace the fill with white. */
  [field setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]];
  [field setBezeled:YES];
  [field setBezelStyle:NSTextFieldRoundedBezel];

  glw_lock(gr);
  if(gr->gr_osk_widget == w)
    glw_gtb_set_native_editor(w, 1);
  glw_unlock(gr);

  native_text_field = field;
  [self addSubview:field];
  [[self window] makeFirstResponder:field];
  NSTextView *editor = (NSTextView *)[field currentEditor];
  [editor setSelectedRange:NSMakeRange([[field stringValue] length], 0)];
}

- (void)scrollWheel:(NSEvent *)event
{
  if([event hasPreciseScrollingDeltas]) {
    [self glwMouseEvent:GLW_POINTER_FINE_SCROLL event:event];
  } else {
    [self glwMouseEvent:GLW_POINTER_SCROLL event:event];
  }
}


- (void)glwEventFromMouseEvent:(NSEvent *)event {
  struct {
    int nsevent;
    int glw_event;
  } events[] = {
    {NSLeftMouseDown, GLW_POINTER_LEFT_PRESS},
    {NSLeftMouseUp, GLW_POINTER_LEFT_RELEASE},
    {NSRightMouseDown, GLW_POINTER_RIGHT_PRESS},
    {NSRightMouseUp, GLW_POINTER_RIGHT_RELEASE}
  };

  int i;
  for(i = 0; i < sizeof(events)/sizeof(events[0]); i++) {
    if(events[i].nsevent != [event type])
      continue;

    if([event type] == NSLeftMouseDown ||
       [event type] == NSRightMouseDown)
      mouse_down++;
    else
      mouse_down--;

    [self glwMouseEvent:events[i].glw_event event:event];
    return;
  }

  if([event type] == NSOtherMouseUp) {
    event_t *e = event_create_action(ACTION_MENU);
    event_to_ui(e);
  }
}



- (void)mouseDown:(NSEvent *)event {
  if(native_text_field != nil)
    [self finishNativeEditing];
  [self glwEventFromMouseEvent:event];
}

- (void)mouseMoved:(NSEvent *)event {
  [self showCursor];
  [self glwMouseEvent:GLW_POINTER_MOTION_UPDATE event:event];
}

- (void)mouseDragged:(NSEvent *)event {
  [self glwMouseEvent:GLW_POINTER_MOTION_UPDATE event:event];
}
- (void)mouseUp:(NSEvent *)event {
  [self glwEventFromMouseEvent:event];
}

- (void)rightMouseDown:(NSEvent *)event {
  [self glwEventFromMouseEvent:event];
}

- (void)rightMouseDragged:(NSEvent *)event {
  [self glwMouseEvent:GLW_POINTER_MOTION_UPDATE event:event];
}
- (void)rightMouseUp:(NSEvent *)event {
  [self glwEventFromMouseEvent:event];
}

- (void)otherMouseDown:(NSEvent *)event {
  [self glwEventFromMouseEvent:event];
}

- (void)otherMouseDragged:(NSEvent *)event {
  [self glwMouseEvent:GLW_POINTER_MOTION_UPDATE event:event];
}

- (void)otherMouseUp:(NSEvent *)event {
  [self glwEventFromMouseEvent:event];
}

- (void)viewDidMoveToWindow {
  [[self window] setAcceptsMouseMovedEvents:YES];
  NSScreen *screen = [[self window] screen];
  if(screen == nil)
    screen = [NSScreen mainScreen];
  const CGFloat potential = screen != nil ?
    [screen maximumPotentialExtendedDynamicRangeColorComponentValue] : 1.0;
  [self setWantsExtendedDynamicRangeOpenGLSurface:potential > 1.0];
  const CGFloat current = screen != nil ?
    [screen maximumExtendedDynamicRangeColorComponentValue] : 1.0;
  edr_headroom = current > 1.0 ? current : potential;
  TRACE(TRACE_INFO, "GLW",
        "macOS EDR surface %s, display headroom=%.2f potential=%.2f",
        potential > 1.0 ? "enabled" : "disabled", current, potential);
}

- (BOOL)performKeyEquivalent:(NSEvent *)event
{
  if(native_text_field != nil &&
     ([event modifierFlags] & NSDeviceIndependentModifierFlagsMask) ==
       NSCommandKeyMask &&
     [[[event charactersIgnoringModifiers] lowercaseString] isEqualToString:@"a"]) {
    NSTextView *editor = (NSTextView *)[native_text_field currentEditor];
    if(editor != nil) {
      [editor selectAll:self];
      return YES;
    }
  }
  return [super performKeyEquivalent:event];
}

/**
 *
 */
- (void)keyDown:(NSEvent *)event
{
  NSString *chars = [event characters];
  #if 0
  static NSMutableArray *eventArray;// static == bad
  NSString *charsim = [event charactersIgnoringModifiers];
  if(compositeKey || [chars length] == 0  || [charsim length] == 0) {
    if(!eventArray)
      eventArray = [[NSMutableArray alloc] initWithCapacity:1];

    compositeKey = YES;
    [eventArray addObject:event];
    /* uses NSTextInput protocol and results in calls to insertText: */
    [self interpretKeyEvents:eventArray];
    [eventArray removeObject:event];
    return;
  }
  #endif

  unichar c = [chars characterAtIndex:0];
  unichar cim = [[event charactersIgnoringModifiers] characterAtIndex:0];
  /* only care for some modifier keys */
  int mod = [event modifierFlags] &
  (NSShiftKeyMask | NSCommandKeyMask | NSControlKeyMask |
   NSFunctionKeyMask | NSAlternateKeyMask);

  event_t *e = NULL;
  action_type_t av[3];
  int i;

  for(i = 0; i < sizeof(keysym2action) / sizeof(keysym2action[0]); i++) {
    if(keysym2action[i].key == cim &&
       keysym2action[i].mod == (mod & ~NSFunctionKeyMask)) {
      av[0] = keysym2action[i].action1;
      av[1] = keysym2action[i].action2;
      av[2] = keysym2action[i].action3;

      if(keysym2action[i].action3 != ACTION_NONE)
	e = event_create_action_multi(av, 3);
      else if(keysym2action[i].action2 != ACTION_NONE)
	e = event_create_action_multi(av, 2);
      else
          e = event_create_action_multi(av, 1);
      break;
    }
  }

  if(e == NULL && cim >= NSF1FunctionKey && cim <= NSF35FunctionKey)
    e = event_from_Fkey(cim - NSF1FunctionKey + 1,
			mod & NSShiftKeyMask ? 1 : 0);

  if(e == NULL)
    e = event_create_int(EVENT_UNICODE, c);

  e->e_flags |= EVENT_KEYPRESS;

  [self hideCursor];

  prop_send_ext_event(eventSink, e);
  event_release(e);
}



- (BOOL)acceptsFirstResponder
{
  return YES;
}

- (void)windowDidMiniaturize:(NSNotification *)notification
{
  minimized = YES;
}

- (void)windowDidDeminiaturize:(NSNotification *)notification
{
  minimized = NO;
}

/**
 *
 */
- (void)reshape
{
  if(stopped)
    return;

  NSRect bb = [self convertRectToBacking:[self bounds]];

  glw_lock(gr);
  gr->gr_width = bb.size.width;
  gr->gr_height = bb.size.height;
  glw_need_refresh(gr, 0);
  glw_unlock(gr);

  NSOpenGLContext *cc = [self openGLContext];
  [cc makeCurrentContext];

  CGLLockContext((CGLContextObj)[cc CGLContextObj]);
  [[self openGLContext] update];
  CGLUnlockContext((CGLContextObj)[cc CGLContextObj]);
}


/**
 *
 */
- (void)drawRect:(NSRect)rect
{
  if(stopped)
    return;
  [self drawFrame];
}


/**
 *
 */
- (void)drawFrame
{
  NSOpenGLContext *currentContext = [self openGLContext];
  if(stopped || currentContext == nil || gr->gr_be_render_unlocked == NULL)
    return;

  [currentContext makeCurrentContext];
  CGLLockContext((CGLContextObj)[currentContext CGLContextObj]);

  glw_lock(gr);

  if(in_full_window)
    [self autoHideCursor];

  glw_prepare_frame(gr, GLW_NO_FRAMERATE_UPDATE);

  int refresh = gr->gr_need_refresh;
  gr->gr_need_refresh = 0;

  if(!minimized && gr->gr_width > 1 && gr->gr_height > 1 && gr->gr_universe) {

    if(refresh) {
      glw_rctx_t rc;
      int zmax = 0;
      glw_rctx_init(&rc, gr->gr_width, gr->gr_height, 1, &zmax);
      glw_layout0(gr->gr_universe, &rc);

      if(refresh & GLW_REFRESH_FLAG_RENDER) {
        glViewport(0, 0, gr->gr_width, gr->gr_height);
        glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
        glw_render0(gr->gr_universe, &rc);
      }
    }

    glw_unlock(gr);
    if(refresh & GLW_REFRESH_FLAG_RENDER)
      glw_post_scene(gr);

  } else {
    glw_unlock(gr);
  }

  if(refresh & GLW_REFRESH_FLAG_RENDER)
    [currentContext flushBuffer];

  CGLUnlockContext((CGLContextObj)[currentContext CGLContextObj]);
}


/**
 *
 */
- (void)stop
{
  [self finishNativeEditing];
  stopped = YES;

  [self showCursor];

  CVDisplayLinkStop(m_displayLink);

  NSOpenGLContext *currentContext = [self openGLContext];
  [currentContext makeCurrentContext];

  CGLLockContext((CGLContextObj)[currentContext CGLContextObj]);

  glw_lock(gr);
  glw_flush(gr);
  glw_unlock(gr);

  CGLUnlockContext((CGLContextObj)[currentContext CGLContextObj]);

  [self showCursor];
  prop_unsubscribe(fullWindow);
  fullWindow = NULL;
  prop_unsubscribe(disableScreensaver);
  disableScreensaver = NULL;
  glw_inhibit_display_sleep(self, 0);
}


/**
 *
 */
- (void)dealloc
{
  CVDisplayLinkRelease(m_displayLink);
  [super dealloc];
}


/**
 *
 */
- (void)becomeKeyWindow {
  is_key_window = YES;
  [[self window] setAcceptsMouseMovedEvents:YES];
}

- (void) resignKeyWindow {
  is_key_window = NO;
  [self showCursor];

  glw_pointer_event_t gpe = {0};
  gpe.type = GLW_POINTER_GONE;
  glw_pointer_event(gr, &gpe);
}

/**
 *
 */
- (id)initWithFrame:(NSRect)frameRect :(struct glw_root *)root
{
  NSOpenGLPixelFormat *wpf;
  NSOpenGLPixelFormatAttribute attribs_windowed[] = {
    NSOpenGLPFAWindow,
    NSOpenGLPFAColorSize, 32,
    NSOpenGLPFADoubleBuffer,
    NSOpenGLPFASingleRenderer,
    0 };

  wpf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attribs_windowed];

  if(wpf == nil) {
    NSLog(@"Unable to create pixel format.");
    exit(0);
  }
  self = [super initWithFrame:frameRect pixelFormat:wpf];
  if(self == nil) {
    NSLog(@"Unable to create a OpenGL context.");
    exit(0);
  }

  [self setWantsBestResolutionOpenGLSurface:YES];
  edr_headroom = 1.0;
  [wpf release];

  gr = root;
  gr->gr_private = self;
  gr->gr_open_osk = openosk;
  minimized = NO;
  eventSink = prop_create(gr->gr_prop_ui, "eventSink");

  fullWindow =
    prop_subscribe(0,
		   PROP_TAG_NAME("ui", "fullwindow"),
		   PROP_TAG_COURIER, gr->gr_courier,
		   PROP_TAG_CALLBACK_INT, glw_in_fullwindow, self,
		   PROP_TAG_ROOT, gr->gr_prop_ui,
		   NULL);

  disableScreensaver =
    prop_subscribe(0,
		   PROP_TAG_NAME("ui", "disableScreensaver"),
		   PROP_TAG_COURIER, gr->gr_courier,
		   PROP_TAG_CALLBACK_INT, glw_inhibit_display_sleep, self,
		   PROP_TAG_ROOT, gr->gr_prop_ui,
		   NULL);

  NSOpenGLContext *openGLContext = [self openGLContext];
  [openGLContext makeCurrentContext];

  GLint one = 1;
  [openGLContext setValues:&one forParameter:NSOpenGLCPSwapInterval];

  CVDisplayLinkCreateWithActiveCGDisplays(&m_displayLink);
  CVDisplayLinkSetOutputCallback(m_displayLink, newframe, self);
  m_cgl_context = (CGLContextObj)[openGLContext CGLContextObj];
  m_cgl_pixel_format =
    (CGLPixelFormatObj)[[self pixelFormat] CGLPixelFormatObj];

  CVDisplayLinkSetCurrentCGDisplayFromOpenGLContext(m_displayLink,
                                                    m_cgl_context,
                                                    m_cgl_pixel_format);

  NSSize s = [self bounds].size;
  gr->gr_width = s.width;
  gr->gr_height = s.height;

  CGLLockContext(m_cgl_context);
  int gl_init_status = glw_opengl_init_context(gr);
  CGLUnlockContext(m_cgl_context);
  stopped = NO;

  if(gl_init_status == 0 && gr->gr_be_render_unlocked != NULL) {
    CVDisplayLinkStart(m_displayLink);
  } else {
    stopped = YES;
    NSLog(@"Unable to initialize Movian's OpenGL renderer");
  }

  [self registerForDraggedTypes:[NSArray arrayWithObjects:NSFilenamesPboardType,NSURLPboardType,nil]];

  return self;
}



- (NSDragOperation)draggingEntered:(id <NSDraggingInfo>)sender
{
  return NSDragOperationLink;
}

- (BOOL)performDragOperation:(id)sender {
  NSPasteboard *pb = [sender draggingPasteboard];

  const char *u = NULL;

  NSArray *filenames = [pb propertyListForType:@"NSFilenamesPboardType"];
  if(filenames) {
    NSString *path = [filenames objectAtIndex:0];
    u = [path cStringUsingEncoding:NSUTF8StringEncoding];

  } else if([[pb types] containsObject:NSURLPboardType]) {
    NSArray *urls = [pb readObjectsForClasses:@[[NSURL class]] options:nil];
    NSURL *url = [urls objectAtIndex:0];
    u = [url.absoluteString cStringUsingEncoding:NSUTF8StringEncoding];

  } else {
    return NO;
  }

  event_t *e = event_create_openurl(u);
  prop_send_ext_event(eventSink, e);
  event_release(e);
  return YES;
}



- (void) copy: (NSNotification *)not
{
  event_t *e = event_create_action(ACTION_COPY);
  prop_send_ext_event(eventSink, e);
  event_release(e);
}


- (void) paste: (NSNotification *)not
{
  event_t *e = event_create_action(ACTION_PASTE);
  prop_send_ext_event(eventSink, e);
  event_release(e);
}


/**
 *
 */
- (CGLContextObj)getCglContext {
  return m_cgl_context;
}


/**
 *
 */
- (CGLPixelFormatObj)getCglPixelFormat {
  return m_cgl_pixel_format;
}

- (float)edrHeadroom {
  NSScreen *screen = [[self window] screen];
  if(screen == nil)
    screen = [NSScreen mainScreen];
  if(screen != nil) {
    const CGFloat potential =
      [screen maximumPotentialExtendedDynamicRangeColorComponentValue];
    [self setWantsExtendedDynamicRangeOpenGLSurface:potential > 1.0];
    const CGFloat current =
      [screen maximumExtendedDynamicRangeColorComponentValue];
    edr_headroom = current > 1.0 ? current : potential;
  }
  return edr_headroom;
}



@end


CGLContextObj
osx_get_cgl_context(glw_root_t *gr)
{
  GLWView *v = gr->gr_private;
  return [v getCglContext];
}

CGLPixelFormatObj
osx_get_cgl_pixel_format(glw_root_t *gr)
{
  GLWView *v = gr->gr_private;
  return [v getCglPixelFormat];
}

float
osx_get_edr_headroom(glw_root_t *gr)
{
  GLWView *v = gr->gr_private;
  return v != nil ? [v edrHeadroom] : 1.0f;
}

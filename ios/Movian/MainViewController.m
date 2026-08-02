//
//  MainViewController.m
//  Movian
//
//  Created by Andreas Öman on 03/06/15.
//  Copyright (c) 2015 Lonelycoder AB. All rights reserved.
//



#import "MainViewController.h"
#import <OpenGLES/ES2/glext.h>
#include <fenv.h>

#include "ui/glw/glw.h"
#include "ui/longpress.h"
#include "navigator.h"

#include "media/media.h"

@interface MainViewController () {
  lphelper_t longpress;
}
@property (strong, nonatomic) EAGLContext *context;
@property (nonatomic) glw_root_t *gr;
@property (nonatomic) CGPoint touch_start_pos;
@property (nonatomic) CGPoint touch_end_pos;
@property (nonatomic) NSTimeInterval touch_start_time;


- (void)setupGL;
- (void)tearDownGL;

@end


@interface GLWView: GLKView <UIKeyInput, UITextFieldDelegate>
@property (nonatomic) prop_t *eventSink;
@property (nonatomic) glw_rect_t rect;
@property (nonatomic) glw_root_t *gr;
@property (nonatomic) int oskscroll;
@property (strong, nonatomic) UITextField *nativeTextField;
@end

@implementation GLWView

- (void)insertText:(NSString *)text {
  const char *cstr = [text UTF8String];
  event_t *e;
  if(!strcmp(cstr, "\n")) {
    [self resignFirstResponder];
    e = event_create_action(ACTION_ENTER);
  } else {
    e = event_create_str(EVENT_INSERT_STRING, cstr);
  }
  
  prop_send_ext_event(self.eventSink, e);
  event_release(e);
}

- (void)deleteBackward {
  event_t *e = event_create_action(ACTION_BS);
  prop_send_ext_event(self.eventSink, e);
  event_release(e);
}

- (BOOL)hasText {
  return YES;
}
- (BOOL)canBecomeFirstResponder {
  return YES;
}

- (int)nativeCursorPosition
{
  UITextPosition *beginning = self.nativeTextField.beginningOfDocument;
  UITextPosition *cursor = self.nativeTextField.selectedTextRange.start;
  NSInteger utf16Position = [self.nativeTextField offsetFromPosition:beginning
                                                          toPosition:cursor];
  if(utf16Position <= 0)
    return 0;

  NSString *prefix = [self.nativeTextField.text substringToIndex:utf16Position];
  return (int)([prefix lengthOfBytesUsingEncoding:NSUTF32LittleEndianStringEncoding] /
               sizeof(uint32_t));
}

- (void)syncNativeTextToMovian
{
  if(self.gr == NULL || self.gr->gr_osk_widget == NULL ||
     self.nativeTextField == nil)
    return;

  const char *text = [self.nativeTextField.text UTF8String];
  int cursor = [self nativeCursorPosition];
  glw_lock(self.gr);
  if(self.gr->gr_osk_widget != NULL)
    glw_gtb_set_edit_text(self.gr->gr_osk_widget, text, cursor);
  glw_unlock(self.gr);
}

- (void)nativeTextChanged:(UITextField *)textField
{
  [self syncNativeTextToMovian];
}

- (void)finishNativeEditing
{
  UITextField *field = self.nativeTextField;
  if(field == nil)
    return;

  [self syncNativeTextToMovian];

  /* Clear this first: resignFirstResponder may synchronously post the
   * keyboard-hide notification, which must not close a later GLW editor. */
  self.nativeTextField = nil;
  field.delegate = nil;
  [field resignFirstResponder];
  [field removeFromSuperview];
  self.oskscroll = 0;

  if(self.gr != NULL) {
    glw_lock(self.gr);
    if(self.gr->gr_osk_widget != NULL) {
      glw_gtb_set_native_editor(self.gr->gr_osk_widget, 0);
      glw_osk_close(self.gr);
    }
    glw_unlock(self.gr);
  }
}

- (void)textFieldDidChangeSelection:(UITextField *)textField
{
  [self syncNativeTextToMovian];
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField
{
  [self finishNativeEditing];
  event_t *e = event_create_action(ACTION_ENTER);
  prop_send_ext_event(self.eventSink, e);
  event_release(e);
  return YES;
}

- (void)openOSKforWidget:(struct glw *)w
                position:(const glw_rect_t *)rect
                    text:(const char *)text
                password:(int)password
{
  self.gr = w->glw_root;
  self.rect = *rect;

  [self.nativeTextField removeFromSuperview];

  CGFloat scale = self.contentScaleFactor;
  UIEdgeInsets insets = self.safeAreaInsets;
  CGRect frame = CGRectMake(insets.left + rect->x1 / scale,
                            insets.top + rect->y1 / scale,
                            (rect->x2 - rect->x1) / scale,
                            (rect->y2 - rect->y1) / scale);
  if(frame.size.height < 44.0) {
    CGFloat center = CGRectGetMidY(frame);
    frame.size.height = 44.0;
    frame.origin.y = center - frame.size.height / 2.0;
  }
  frame = CGRectIntersection(frame, UIEdgeInsetsInsetRect(self.bounds, insets));

  UITextField *field = [[UITextField alloc] initWithFrame:frame];
  field.delegate = self;
  field.text = text != NULL ? [NSString stringWithUTF8String:text] : @"";
  field.secureTextEntry = password != 0;
  field.textColor = UIColor.whiteColor;
  field.tintColor = UIColor.systemBlueColor;
  field.backgroundColor = [UIColor colorWithWhite:0.08 alpha:0.96];
  field.borderStyle = UITextBorderStyleRoundedRect;
  field.clearButtonMode = UITextFieldViewModeWhileEditing;
  field.returnKeyType = UIReturnKeyDone;
  field.autocorrectionType = UITextAutocorrectionTypeNo;
  field.autocapitalizationType = UITextAutocapitalizationTypeNone;
  field.spellCheckingType = UITextSpellCheckingTypeNo;
  [field addTarget:self action:@selector(nativeTextChanged:)
                         forControlEvents:UIControlEventEditingChanged];

  glw_lock(self.gr);
  if(self.gr->gr_osk_widget == w)
    glw_gtb_set_native_editor(w, 1);
  glw_unlock(self.gr);

  [self addSubview:field];
  self.nativeTextField = field;
  [field becomeFirstResponder];
}

#if TARGET_OS_IOS == 1

- (void)keyboardWasShown:(NSNotification*)aNotification
{
  NSDictionary* info = [aNotification userInfo];
  CGRect keyboardFrame = [[info objectForKey:UIKeyboardFrameEndUserInfoKey] CGRectValue];
  keyboardFrame = [self convertRect:keyboardFrame fromView:nil];
  CGFloat keyboardHeight = CGRectGetHeight(CGRectIntersection(self.bounds,
                                                               keyboardFrame));

  int h = self.bounds.size.height - keyboardHeight;
  int h2 = self.rect.y2 / self.contentScaleFactor;

  self.oskscroll = (h2 - h) * self.contentScaleFactor;
  if(self.oskscroll < 0)
    self.oskscroll = 0;
}


- (void)keyboardWillBeHidden:(NSNotification*)aNotification
{
  if(self.nativeTextField != nil)
    [self finishNativeEditing];
  else
    self.oskscroll = 0;
}
#endif

@end





static void
openosk(struct glw_root *gr,
        const char *title, const char *str, struct glw *w,
        int password)
{
  GLWView *view = (__bridge GLWView *)(gr->gr_window);
  
  if(!w->glw_matrix)
    return;
  glw_rect_t r;
  glw_project_matrix(&r, w->glw_matrix, gr);

  /*
   * glw_osk_open() is invoked while the GLW root is locked. UIKit may call
   * textFieldDidChangeSelection synchronously from becomeFirstResponder;
   * that callback synchronizes into GLW and needs the same lock. Defer the
   * native editor until this GLW event has unwound to avoid self-deadlock.
   */
  NSString *initialText = str != NULL ? [NSString stringWithUTF8String:str] : @"";
  glw_rect_t rect = r;
  glw_ref(w);
  dispatch_async(dispatch_get_main_queue(), ^{
    glw_lock(gr);
    BOOL stillEditing = gr->gr_osk_widget == w;
    glw_unlock(gr);

    if(stillEditing) {
      [view openOSKforWidget:w
                    position:&rect
                        text:initialText.UTF8String
                    password:password];
    }

    glw_lock(gr);
    glw_unref(w);
    glw_unlock(gr);
  });
}

static void
glw_in_fullwindow(void *opaque, int val)
{
  BOOL disabled = val ? YES : NO;
  dispatch_async(dispatch_get_main_queue(), ^{
    [UIApplication sharedApplication].idleTimerDisabled = disabled;
  });
}







@implementation MainViewController

- (void)viewDidLoad
{
  [super viewDidLoad];
  
  self.context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];

  if (!self.context) {
    NSLog(@"Failed to create ES context");
  }
  
  GLWView *view = (GLWView *)self.view;
  view.context = self.context;
  view.drawableDepthFormat = GLKViewDrawableDepthFormat24;
  self.preferredFramesPerSecond = 60;

  [self setupGL];
  
  self.gr = calloc(1, sizeof(glw_root_t));
  glw_root_t *gr = self.gr;
  
  gr->gr_private = (__bridge void *)self.context;
  gr->gr_prop_ui = prop_create_root("ui");
  gr->gr_prop_nav = nav_spawn();
  gr->gr_window = (__bridge void *)view;
  
  view.eventSink = prop_create(gr->gr_prop_ui, "eventSink");
  
  prop_subscribe(0,
                 PROP_TAG_NAME("ui", "fullwindow"),
                 PROP_TAG_COURIER, gr->gr_courier,
                 PROP_TAG_CALLBACK_INT, glw_in_fullwindow, NULL,
                 PROP_TAG_ROOT, gr->gr_prop_ui,
                 NULL);


  int flags = 0;
#if TARGET_OS_TV
  flags |= GLW_INIT_KEYBOARD_MODE | GLW_INIT_OVERSCAN | GLW_INIT_IN_FULLSCREEN;
#endif
  glw_init2(gr, flags);

  
#if TARGET_OS_IOS == 1
  gr->gr_open_osk = openosk;
#endif
  
  glw_opengl_init_context(gr);

  glw_lock(gr);
  glw_load_universe(gr);
  glw_unlock(gr);
  
#if TARGET_OS_TV == 1
  UISwipeGestureRecognizer *r;
  
  r = [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(swipeRight:)];
  r.direction = UISwipeGestureRecognizerDirectionRight;
  [self.view addGestureRecognizer:r];
  
  r = [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(swipeLeft:)];
  r.direction = UISwipeGestureRecognizerDirectionLeft;
  [self.view addGestureRecognizer:r];

  r = [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(swipeUp:)];
  r.direction = UISwipeGestureRecognizerDirectionUp;
  [self.view addGestureRecognizer:r];

  r = [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(swipeDown:)];
  r.direction = UISwipeGestureRecognizerDirectionDown;
  [self.view addGestureRecognizer:r];
#endif
  
#if TARGET_OS_IOS == 1
  [[NSNotificationCenter defaultCenter] addObserver:self.view
                                           selector:@selector(keyboardWasShown:)
                                               name:UIKeyboardDidShowNotification object:nil];
  
  [[NSNotificationCenter defaultCenter] addObserver:self.view
                                           selector:@selector(keyboardWillBeHidden:)
                                               name:UIKeyboardWillHideNotification object:nil];

#endif
  
  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(becomeActive)
                                               name:@"appDidBecomeActive"
                                             object:nil];

  [[NSNotificationCenter defaultCenter] addObserver:self
                                           selector:@selector(resignActive)
                                               name:@"appDidResignActive"
                                             object:nil];

  self.pauseOnWillResignActive = NO;
  self.resumeOnDidBecomeActive = NO;
}

- (void)dealloc
{
    [self tearDownGL];
    
    if ([EAGLContext currentContext] == self.context) {
        [EAGLContext setCurrentContext:nil];
    }
}

- (void)didReceiveMemoryWarning
{
  [super didReceiveMemoryWarning];
}

- (void)viewSafeAreaInsetsDidChange
{
  [super viewSafeAreaInsetsDidChange];
  if(self.gr != NULL)
    glw_need_refresh(self.gr, 0);
}

- (BOOL)prefersStatusBarHidden {
    return YES;
}

- (void)setupGL
{
    [EAGLContext setCurrentContext:self.context];
    
}

- (void)tearDownGL
{
    [EAGLContext setCurrentContext:self.context];
    
}

- (void)emitAction:(int)type
{
  glw_root_t *gr = self.gr;
  glw_lock(gr);
  
  event_t *e = event_create_action(type);
  e->e_flags |= EVENT_KEYPRESS;
  glw_inject_event(gr, e);
  glw_unlock(gr);
}


- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event
{
  for(UIPress *press in presses) {
    
    switch(press.type) {
        
      case UIPressTypeUpArrow:
        [self emitAction:ACTION_UP];
        break;
      case UIPressTypeDownArrow:
        [self emitAction:ACTION_DOWN];
        break;
      case UIPressTypeLeftArrow:
        [self emitAction:ACTION_LEFT];
        break;
      case UIPressTypeRightArrow:
        [self emitAction:ACTION_RIGHT];
        break;
      case UIPressTypeSelect:
        longpress_down(&self->longpress);
        break;
      case UIPressTypeMenu:
        [self emitAction:ACTION_NAV_BACK];
        break;
      case UIPressTypePlayPause:
        [self emitAction:ACTION_PLAYPAUSE];
        break;

    }
  }
}


- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event
{
  for(UIPress *press in presses) {
    
    if(press.type == UIPressTypeSelect) {
      if(longpress_up(&self->longpress)) {
        [self emitAction:ACTION_ACTIVATE];
        break;
      }
    }
  }
}

- (void)swipeLeft:(UISwipeGestureRecognizer *)sender
{
  [self emitAction:ACTION_LEFT];
}

- (void)swipeRight:(UISwipeGestureRecognizer *)sender
{
  [self emitAction:ACTION_RIGHT];
}

- (void)swipeUp:(UISwipeGestureRecognizer *)sender
{
  [self emitAction:ACTION_UP];
}

- (void)swipeDown:(UISwipeGestureRecognizer *)sender
{
  [self emitAction:ACTION_DOWN];
}



- (void)emitPointerEvent:(int) type withPoint:(CGPoint *)point time:(NSTimeInterval)ts
{
  glw_root_t *gr = self.gr;
  glw_lock(gr);
  glw_pointer_event_t gpe;

  UIView *view = [self view];
  UIEdgeInsets insets = view.safeAreaInsets;
  CGRect bounds = view.bounds;
  CGFloat width = bounds.size.width - insets.left - insets.right;
  CGFloat height = bounds.size.height - insets.top - insets.bottom;

  if(width <= 0 || height <= 0) {
    glw_unlock(gr);
    return;
  }

  gpe.screen_x =  (2.0 * (point->x - insets.left) / width)  - 1;
  gpe.screen_y = -(2.0 * (point->y - insets.top) / height) + 1;
  gpe.ts = ts * 1000000.0;
  gpe.type = type;
  glw_pointer_event(gr, &gpe);
  glw_unlock(gr);
}


- (void)touchesBegan:(NSSet *)touches withEvent:(UIEvent *)event
{
  if([self.view isFirstResponder]) {
    [self.view resignFirstResponder];
    return;
  }

  if ([touches count] < 1) return;
  CGPoint point = [[touches anyObject] locationInView:[self view]];

  /* A native UITextField owns touches inside its frame. A touch delivered
   * here is outside the active editor: commit and blur it, but keep handling
   * this same touch so the newly tapped Movian control activates normally. */
  GLWView *glwView = (GLWView *)self.view;
  if(glwView.nativeTextField != nil &&
     !CGRectContainsPoint(glwView.nativeTextField.frame, point))
    [glwView finishNativeEditing];

  self.touch_start_pos = point;
  self.touch_start_time = event.timestamp;
    
//  printf("touch begin at %f,%f\n", point.x, point.y);
#if TARGET_OS_TV == 0
  [self emitPointerEvent:GLW_POINTER_TOUCH_START withPoint:&point time:event.timestamp];
#endif
}

- (void)touchesMoved:(NSSet *)touches withEvent:(UIEvent *)event
{
  if ([touches count] < 1) return;
  CGPoint point = [[touches anyObject] locationInView:[self view]];
#if 0
  CGFloat distance = sqrtf((point.x - self.touch_start_pos.x) * (point.x - self.touch_start_pos.x) +
                           (point.y - self.touch_start_pos.y) * (point.y - self.touch_start_pos.y));
  double delta_time = event.timestamp - self.touch_start_time;
//  printf("touch end at %f,%f distance=%f  delta-time=%f  speed: %f\n", point.x, point.y, distance, delta_time, distance / delta_time);
#endif
#if TARGET_OS_TV == 0
  [self emitPointerEvent:GLW_POINTER_TOUCH_MOVE withPoint:&point time:event.timestamp];
#endif
}

- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event
{
  if ([touches count] < 1) return;
  CGPoint point = [[touches anyObject] locationInView:[self view]];


#if TARGET_OS_TV == 0
  [self emitPointerEvent:GLW_POINTER_TOUCH_END withPoint:&point time:event.timestamp];
#endif
}

- (void)touchesCancelled:(NSSet *)touches withEvent:(UIEvent *)event;
{
  if ([touches count] < 1) return;
  CGPoint point = [[touches anyObject] locationInView:[self view]];

#if TARGET_OS_TV == 0
  [self emitPointerEvent:GLW_POINTER_TOUCH_CANCEL withPoint:&point time:event.timestamp];
#endif
}




static void
denormal_ftz(void)
{
#ifdef FE_DFL_DISABLE_SSE_DENORMS_ENV
  fesetenv(FE_DFL_DISABLE_SSE_DENORMS_ENV);
#elif defined(__arm__)
  fenv_t env;
  fegetenv(&env);
  env.__fpscr |= __fpscr_flush_to_zero;
  fesetenv(&env);
#elif defined(__arm64__)
  fenv_t env;
  fegetenv(&env);
  env.__fpcr |= __fpcr_flush_to_zero;
  fesetenv(&env);
#else
#error No way to make denormals flush to zero
#endif
}





- (void)glkView:(GLWView *)view drawInRect:(CGRect)rect
{
  glw_root_t *gr = self.gr;

  denormal_ftz();
  glw_lock(gr);
  
  UIEdgeInsets insets = view.safeAreaInsets;
  CGRect bounds = view.bounds;
  CGFloat scaleX = view.drawableWidth / bounds.size.width;
  CGFloat scaleY = view.drawableHeight / bounds.size.height;
  int safeLeft = (int)lround(insets.left * scaleX);
  int safeRight = (int)lround(insets.right * scaleX);
  int safeTop = (int)lround(insets.top * scaleY);
  int safeBottom = (int)lround(insets.bottom * scaleY);

  gr->gr_width = (int)view.drawableWidth - safeLeft - safeRight;
  gr->gr_height = (int)view.drawableHeight - safeTop - safeBottom;
  
  glw_prepare_frame(self.gr, 0);

  int refresh = gr->gr_need_refresh;
  gr->gr_need_refresh = 0;

  if(gr->gr_width > 1 && gr->gr_height > 1 && gr->gr_universe) {
    
    if(refresh) {
      glw_rctx_t rc;
      int zmax = 0;
      glw_rctx_init(&rc, gr->gr_width, gr->gr_height, 1, &zmax);
      glw_layout0(gr->gr_universe, &rc);
      
      if(refresh & GLW_REFRESH_FLAG_RENDER) {
        glViewport(safeLeft, safeBottom + view.oskscroll,
                   gr->gr_width, gr->gr_height);
        glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
        glw_render0(gr->gr_universe, &rc);
      }
    }
    
    glw_unlock(gr);
    if(refresh & GLW_REFRESH_FLAG_RENDER) {
      glw_post_scene(gr);
    }
    
  } else {
    glw_unlock(gr);
  }

  if(longpress_periodic(&self->longpress, gr->gr_frame_start)) {
    event_t *e = event_create_action(ACTION_ITEMMENU);
    e->e_flags |= EVENT_KEYPRESS;
    glw_inject_event(gr, e);
  }
}


- (void)becomeActive
{
  glw_need_refresh(self.gr, 0);
  self.paused = NO;
  media_global_hold(0, MP_HOLD_OS);
}

- (void)resignActive
{
  self.paused = YES;
  media_global_hold(1, MP_HOLD_OS);
}


@end

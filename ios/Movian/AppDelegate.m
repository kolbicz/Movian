//
//  AppDelegate.m
//  Movian
//
//  Created by Andreas Öman on 03/06/15.
//  Copyright (c) 2015 Lonelycoder AB. All rights reserved.
//

#import "AppDelegate.h"
#import <AVFoundation/AVFoundation.h>

#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>

#include "arch/posix/posix.h"
#include "main.h"
#include "service.h"
#include "networking/asyncio.h"
#include "arch/atomic.h"

@import MediaPlayer;

const char *appversion;

static char *
ios_storage_path(NSSearchPathDirectory directory, NSString *leaf)
{
  NSFileManager *fm = [NSFileManager defaultManager];
  NSMutableArray<NSString *> *candidates = [NSMutableArray array];
  NSString *primary = [NSSearchPathForDirectoriesInDomains(directory,
                                                            NSUserDomainMask,
                                                            YES) firstObject];
  if(primary != nil)
    [candidates addObject:[primary stringByAppendingPathComponent:leaf]];

  // Platform applications installed by jailbreak package managers do not
  // always receive an Apple application container. Keep their state in the
  // mobile user's Library instead of constructing paths from a nil result.
  [candidates addObject:[[ @"/var/mobile/Library/Movian"
                           stringByAppendingPathComponent:leaf]
                         stringByStandardizingPath]];
  [candidates addObject:[[[NSTemporaryDirectory()
                           stringByAppendingPathComponent:@"Movian"]
                          stringByAppendingPathComponent:leaf]
                         stringByStandardizingPath]];

  for(NSString *candidate in candidates) {
    NSError *error = nil;
    if([fm createDirectoryAtPath:candidate
      withIntermediateDirectories:YES
                       attributes:nil
                            error:&error] &&
       access([candidate fileSystemRepresentation], W_OK) == 0)
      return strdup([candidate fileSystemRepresentation]);

    NSLog(@"Movian cannot use storage directory %@: %@", candidate, error);
  }
  return NULL;
}

static void
ios_migrate_legacy_persistent_path(void)
{
#if !TARGET_OS_TV
  NSString *library = [NSSearchPathForDirectoriesInDomains(NSLibraryDirectory,
                                                            NSUserDomainMask,
                                                            YES) firstObject];
  NSString *caches = [NSSearchPathForDirectoriesInDomains(NSCachesDirectory,
                                                           NSUserDomainMask,
                                                           YES) firstObject];
  if(library == nil || caches == nil)
    return;

  NSString *destination = [library stringByAppendingPathComponent:@"persistent"];
  NSString *legacy = [caches stringByAppendingPathComponent:@"persistent"];
  NSFileManager *fm = [NSFileManager defaultManager];
  if(![fm fileExistsAtPath:destination] && [fm fileExistsAtPath:legacy]) {
    NSError *error = nil;
    if(![fm moveItemAtPath:legacy toPath:destination error:&error])
      NSLog(@"Movian could not migrate persistent data to %@: %@",
            destination, error);
  }
#endif
}

void
arch_open_external_url(const char *url)
{
  NSString *string = [NSString stringWithUTF8String:url];
  if(string == nil)
    return;
  NSURL *nsurl = [NSURL URLWithString:string];
  if(nsurl == nil)
    return;
  dispatch_async(dispatch_get_main_queue(), ^{
    [[UIApplication sharedApplication] openURL:nsurl
                                       options:@{}
                             completionHandler:nil];
  });
}

uint32_t
parse_version_int(const char *str)
{
  int major = 0;
  int minor = 0;
  int commit = 0;
  sscanf(str, "%d.%d.%d", &major, &minor, &commit);

  return
    major * 10000000 +
    minor *   100000 +
    commit;
}

uint32_t
app_get_version_int(void)
{
  return parse_version_int(appversion);
}



@interface AppDelegate ()

@property (strong) NSMutableDictionary *mediaInfo;
@property (strong) MPNowPlayingInfoCenter *mediaInfoCenter;
@property (strong) AVAudioSession *audioSession;
@property BOOL playing;
@end

@implementation AppDelegate



static void set_title(void *opaque, const char *str)
{
  AppDelegate *ad = (__bridge AppDelegate *)opaque;
  @synchronized(ad.mediaInfo) {
    if(str != NULL) {
      [ad.mediaInfo setObject: [NSString stringWithUTF8String:str] forKey:MPMediaItemPropertyTitle];
    } else {
      [ad.mediaInfo removeObjectForKey:MPMediaItemPropertyTitle];
    }
    if(ad.playing)
      ad.mediaInfoCenter.nowPlayingInfo = ad.mediaInfo;
  }
}

static void set_artist(void *opaque, const char *str)
{
  AppDelegate *ad = (__bridge AppDelegate *)opaque;
  @synchronized(ad.mediaInfo) {
    if(str != NULL) {
      [ad.mediaInfo setObject: [NSString stringWithUTF8String:str] forKey:MPMediaItemPropertyArtist];
    } else {
      [ad.mediaInfo removeObjectForKey:MPMediaItemPropertyArtist];
    }
    if(ad.playing)
      ad.mediaInfoCenter.nowPlayingInfo = ad.mediaInfo;
  }
}

static void set_album(void *opaque, const char *str)
{
  AppDelegate *ad = (__bridge AppDelegate *)opaque;
  @synchronized(ad.mediaInfo) {
    if(str != NULL) {
      [ad.mediaInfo setObject: [NSString stringWithUTF8String:str] forKey:MPMediaItemPropertyAlbumTitle];
    } else {
      [ad.mediaInfo removeObjectForKey:MPMediaItemPropertyAlbumTitle];
    }
    if(ad.playing)
      ad.mediaInfoCenter.nowPlayingInfo = ad.mediaInfo;
  }
}

static void set_duration(void *opaque, int duration)
{
  AppDelegate *ad = (__bridge AppDelegate *)opaque;
  @synchronized(ad.mediaInfo) {
    if(duration > 0) {
      [ad.mediaInfo setObject: @(duration) forKey:MPMediaItemPropertyPlaybackDuration];
    } else {
      [ad.mediaInfo removeObjectForKey:MPMediaItemPropertyPlaybackDuration];
    }
    if(ad.playing)
      ad.mediaInfoCenter.nowPlayingInfo = ad.mediaInfo;
  }
}


static void set_media_type(void *opaque, const char *str)
{
  AppDelegate *ad = (__bridge AppDelegate *)opaque;

  @synchronized(ad.mediaInfo) {
    
    
    if(str != NULL) {
      ad.playing = true;
      ad.mediaInfoCenter.nowPlayingInfo = ad.mediaInfo;
    } else {
      ad.playing = false;
      ad.mediaInfoCenter.nowPlayingInfo = nil;
    }

  }
}


- (void)setupMediaInfo
{
  self.mediaInfo = [NSMutableDictionary dictionary];
  self.mediaInfoCenter = [MPNowPlayingInfoCenter defaultCenter];
  
  prop_subscribe(0,
                 PROP_TAG_NAME("global", "media", "current", "metadata", "title"),
                 PROP_TAG_CALLBACK_STRING, set_title, self,
                 NULL);
  
  prop_subscribe(0,
                 PROP_TAG_NAME("global", "media", "current", "metadata", "artist"),
                 PROP_TAG_CALLBACK_STRING, set_artist, self,
                 NULL);
  
  prop_subscribe(0,
                 PROP_TAG_NAME("global", "media", "current", "metadata", "album"),
                 PROP_TAG_CALLBACK_STRING, set_album, self,
                 NULL);
  
  prop_subscribe(0,
                 PROP_TAG_NAME("global", "media", "current", "metadata", "duration"),
                 PROP_TAG_CALLBACK_INT, set_duration, self,
                 NULL);
  
  prop_subscribe(0,
                 PROP_TAG_NAME("global", "media", "current", "type"),
                 PROP_TAG_CALLBACK_STRING, set_media_type, self,
                 NULL);
  
  self.audioSession = [AVAudioSession sharedInstance];
  NSError *setCategoryError = nil;
  BOOL success = [self.audioSession setCategory:AVAudioSessionCategoryPlayback error:&setCategoryError];
  if (!success) {
    NSLog(@"AudioSession Error: %@", [setCategoryError description]);
  }
  
}



- (void)emitMediaAction:(int)type
{
  event_t *e = event_create_action(type);
  e->e_flags |= EVENT_KEYPRESS;
  
  prop_t *p = prop_create_multi(prop_get_global(), "media", "eventSink", NULL);
  prop_send_ext_event(p, e);
  
  prop_ref_dec(p);
  event_release(e);
}



- (MPRemoteCommandHandlerStatus)nextTrack:(MPRemoteCommandEvent *)event
{
  [self emitMediaAction:ACTION_SKIP_FORWARD];
  return MPRemoteCommandHandlerStatusSuccess;
}

- (MPRemoteCommandHandlerStatus)previousTrack:(MPRemoteCommandEvent *)event
{
  [self emitMediaAction:ACTION_SKIP_BACKWARD];
  return MPRemoteCommandHandlerStatusSuccess;
}

- (MPRemoteCommandHandlerStatus)togglePlayPause:(MPRemoteCommandEvent *)event
{
  [self emitMediaAction:ACTION_PLAYPAUSE];
  return MPRemoteCommandHandlerStatusSuccess;
}

- (MPRemoteCommandHandlerStatus)play:(MPRemoteCommandEvent *)event
{
  [self emitMediaAction:ACTION_PLAY];
  return MPRemoteCommandHandlerStatusSuccess;
}

- (MPRemoteCommandHandlerStatus)pause:(MPRemoteCommandEvent *)event
{
  [self emitMediaAction:ACTION_PAUSE];
  return MPRemoteCommandHandlerStatusSuccess;
}




- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    // Override point for customization after application launch.

  NSDictionary* infoDict = [[NSBundle mainBundle] infoDictionary];
  NSString* version = [infoDict objectForKey:@"CFBundleShortVersionString"];
  appversion = strdup([version UTF8String]);

  gconf.concurrency = (int)[[NSProcessInfo processInfo] activeProcessorCount];

#if TARGET_OS_TV
  gconf.persistent_path = ios_storage_path(NSCachesDirectory, @"persistent");
#else
  ios_migrate_legacy_persistent_path();
  gconf.persistent_path = ios_storage_path(NSLibraryDirectory, @"persistent");
#endif
  gconf.cache_path = ios_storage_path(NSCachesDirectory, @"cache");
  
  posix_init();
  
  main_init();
  
  NSString *docsdir = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                                                            NSUserDomainMask,
                                                            YES) firstObject];
  if(docsdir != nil)
    service_createp("Files", _p("Files"), [docsdir fileSystemRepresentation],
                    "files", NULL, 0, 1, SVC_ORIGIN_SYSTEM);

  MPRemoteCommandCenter *remoteCommandCenter = [MPRemoteCommandCenter sharedCommandCenter];
  
  [[remoteCommandCenter nextTrackCommand] addTarget:self action:@selector(nextTrack:)];
  [[remoteCommandCenter previousTrackCommand] addTarget:self action:@selector(previousTrack:)];
  [[remoteCommandCenter togglePlayPauseCommand] addTarget:self action:@selector(togglePlayPause:)];
  [[remoteCommandCenter pauseCommand] addTarget:self action:@selector(pause:)];
  [[remoteCommandCenter playCommand] addTarget:self action:@selector(play:)];

  
  [self setupMediaInfo];
  return YES;
}

- (void)applicationWillResignActive:(UIApplication *)application {
  [[NSNotificationCenter defaultCenter] postNotificationName:@"appDidResignActive" object:nil];
}

- (void)applicationDidEnterBackground:(UIApplication *)application {
  asyncio_suspend();
}

- (void)applicationWillEnterForeground:(UIApplication *)application {
  asyncio_resume();
}

- (void)applicationDidBecomeActive:(UIApplication *)application {
  [[NSNotificationCenter defaultCenter] postNotificationName:@"appDidBecomeActive" object:nil];
}

- (void)applicationWillTerminate:(UIApplication *)application {
  printf("%s\n", __FUNCTION__);
}

@end


void ios_set_audio_status_enable(int delta)
{
  static atomic_t v;
  int c = atomic_add_and_fetch(&v, delta);
  AVAudioSession *audioSession = [AVAudioSession sharedInstance];

  TRACE(TRACE_INFO, "Audio", "OS AudioSession set to %s", !!c ? "on" : "off");
  NSError *activationError = nil;
  [audioSession setActive:!!c error:&activationError];
}

#import <AudioToolbox/AudioToolbox.h>
#import <Cocoa/Cocoa.h>

#include "psx_audio.hpp"
#include "psx_catalog.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace {

struct Row {
  std::uint32_t id = 0;
  std::uint32_t lba = 0;
  std::int32_t length = 0;
  std::string kind;
  std::string detail;
};

}  // namespace

@interface NearestImageView : NSView
@property(nonatomic, strong) NSImage* image;
@end

@implementation NearestImageView
- (void)drawRect:(NSRect)dirtyRect {
  [[NSGraphicsContext currentContext] setImageInterpolation:NSImageInterpolationNone];
  [self.image drawInRect:self.bounds
                fromRect:NSZeroRect
               operation:NSCompositingOperationSourceOver
                fraction:1];
}
@end

@interface XenoDocument : NSObject <NSApplicationDelegate, NSTableViewDataSource, NSTableViewDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSTableView* table;
@property(nonatomic, strong) NearestImageView* picture;
@property(nonatomic, strong) NSTextView* structure;
@property(nonatomic, copy) NSString* imagePath;
@end

@implementation XenoDocument {
  std::vector<Row> _rows;
  std::string _bin;
  std::vector<std::int16_t> _pcm;
  AudioQueueRef _queue;
}

- (instancetype)initWithCue:(NSString*)cue {
  self = [super init];
  if (self != nil) {
    _imagePath = cue;
    _queue = nullptr;
  }
  return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  [self buildMenu];
  [self buildWindow];
  [NSApp activateIgnoringOtherApps:YES];
  if (self.imagePath.length > 0) {
    [self openPath:self.imagePath];
  } else {
    [self openPanel:nil];
  }
}

- (void)buildMenu {
  NSMenu* menuBar = [[NSMenu alloc] init];
  NSMenuItem* appItem = [[NSMenuItem alloc] init];
  NSMenu* appMenu = [[NSMenu alloc] init];
  NSMenuItem* openItem = [[NSMenuItem alloc] initWithTitle:@"Open..."
                                                    action:@selector(openPanel:)
                                             keyEquivalent:@"o"];
  openItem.target = self;
  [appMenu addItem:openItem];
  NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit"
                                                    action:@selector(terminate:)
                                             keyEquivalent:@"q"];
  quitItem.target = NSApp;
  [appMenu addItem:quitItem];
  appItem.submenu = appMenu;
  [menuBar addItem:appItem];
  NSApp.mainMenu = menuBar;
}

- (void)buildWindow {
  NSRect frame = NSMakeRect(80, 80, 1100, 720);
  self.window = [[NSWindow alloc] initWithContentRect:frame
                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                      NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
  self.window.title = @"XenoNative";
  NSSplitView* split = [[NSSplitView alloc] initWithFrame:self.window.contentView.bounds];
  split.vertical = YES;
  split.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  NSScrollView* tableScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 420, 720)];
  self.table = [[NSTableView alloc] initWithFrame:tableScroll.bounds];
  for (NSString* title in @[ @"id", @"lba", @"size", @"type" ]) {
    NSTableColumn* column = [[NSTableColumn alloc] initWithIdentifier:title];
    column.title = title;
    column.width = [title isEqualToString:@"id"] ? 50 : 90;
    [self.table addTableColumn:column];
  }
  self.table.dataSource = self;
  self.table.delegate = self;
  tableScroll.documentView = self.table;
  tableScroll.hasVerticalScroller = YES;
  NSSplitView* detail = [[NSSplitView alloc] initWithFrame:NSMakeRect(0, 0, 680, 720)];
  detail.vertical = NO;
  self.picture = [[NearestImageView alloc] initWithFrame:NSMakeRect(0, 0, 680, 420)];
  NSButton* play = [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 680, 32)];
  play.title = @"Play";
  play.target = self;
  play.action = @selector(playSample:);
  play.bezelStyle = NSBezelStyleRounded;
  NSScrollView* textScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 680, 248)];
  self.structure = [[NSTextView alloc] initWithFrame:textScroll.bounds];
  self.structure.editable = NO;
  self.structure.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
  textScroll.documentView = self.structure;
  textScroll.hasVerticalScroller = YES;
  [detail addSubview:self.picture];
  [detail addSubview:play];
  [detail addSubview:textScroll];
  [split addSubview:tableScroll];
  [split addSubview:detail];
  self.window.contentView = split;
  [self.window makeKeyAndOrderFront:nil];
  [self.window makeFirstResponder:self.table];
  __weak XenoDocument* weakSelf = self;
  [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                        handler:^NSEvent*(NSEvent* event) {
                                          XenoDocument* document = weakSelf;
                                          if (document == nil) {
                                            return event;
                                          }
                                          return [document stepSelection:event];
                                        }];
}

- (NSEvent*)stepSelection:(NSEvent*)event {
  if (event.window != self.window) {
    return event;
  }
  const unsigned short code = event.keyCode;
  const bool previous = code == 126 || code == 123;
  const bool next = code == 125 || code == 124;
  if (!previous && !next) {
    return event;
  }
  NSInteger row = self.table.selectedRow;
  if (row < 0) {
    row = 0;
  } else if (previous && row > 0) {
    --row;
  } else if (next && row + 1 < static_cast<NSInteger>(_rows.size())) {
    ++row;
  }
  if (!_rows.empty()) {
    [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:row] byExtendingSelection:NO];
    [self.table scrollRowToVisible:row];
  }
  return nil;
}

- (void)openPanel:(id)sender {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.canChooseFiles = YES;
  panel.canChooseDirectories = NO;
  if ([panel runModal] == NSModalResponseOK) {
    [self openPath:panel.URL.path];
  }
}

- (void)openPath:(NSString*)path {
  const auto disc = psx_cd_loc::open_indexed_disc(path.UTF8String);
  if (!disc.error.empty()) {
    self.structure.string = [NSString stringWithUTF8String:disc.error.c_str()];
    return;
  }
  _bin = disc.image_path;
  _rows.clear();
  for (const auto& blob : disc.entries) {
    if (blob.file.group || blob.file.length <= 0) {
      continue;
    }
    Row row;
    row.id = blob.file.id;
    row.lba = blob.file.lba;
    row.length = blob.file.length;
    row.kind = blob.read_error ? "error" : psx_cd_loc::file_kind_name(blob.classification.kind);
    row.detail = blob.classification.detail;
    _rows.push_back(row);
  }
  self.window.title = [NSString stringWithFormat:@"XenoNative - %s (%lu files)", disc.volume_id.c_str(),
                                                 static_cast<unsigned long>(_rows.size())];
  [self.table reloadData];
  if (!_rows.empty()) {
    [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    [self.table scrollRowToVisible:0];
  }
  [self.window makeFirstResponder:self.table];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
  return static_cast<NSInteger>(_rows.size());
}

- (id)tableView:(NSTableView*)tableView objectValueForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
  const Row& item = _rows.at(static_cast<std::size_t>(row));
  if ([column.identifier isEqualToString:@"id"]) {
    return [NSString stringWithFormat:@"%u", item.id];
  }
  if ([column.identifier isEqualToString:@"lba"]) {
    return [NSString stringWithFormat:@"%u", item.lba];
  }
  if ([column.identifier isEqualToString:@"size"]) {
    return [NSString stringWithFormat:@"%d", item.length];
  }
  return [NSString stringWithUTF8String:item.kind.c_str()];
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification {
  const NSInteger row = self.table.selectedRow;
  if (row < 0 || static_cast<std::size_t>(row) >= _rows.size() || _bin.empty()) {
    return;
  }
  const Row& item = _rows.at(static_cast<std::size_t>(row));
  const auto preview =
      psx_cd_loc::preview_indexed_file(_bin, item.lba, static_cast<std::uint32_t>(item.length));
  if (!preview.error.empty()) {
    self.picture.image = nil;
    [self.picture setNeedsDisplay:YES];
    self.structure.string = [NSString stringWithUTF8String:preview.error.c_str()];
    return;
  }
  if (!preview.classification.pictures.empty()) {
    const auto& image = preview.classification.pictures.front().image;
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                                    pixelsWide:image.width
                                                                    pixelsHigh:image.height
                                                                 bitsPerSample:8
                                                               samplesPerPixel:3
                                                                      hasAlpha:NO
                                                                      isPlanar:NO
                                                                colorSpaceName:NSCalibratedRGBColorSpace
                                                                   bytesPerRow:image.width * 3
                                                                  bitsPerPixel:24];
    if (rep.bitmapData != nullptr && image.rgb.size() == static_cast<std::size_t>(image.width) *
                                                              static_cast<std::size_t>(image.height) * 3) {
      std::memcpy(rep.bitmapData, image.rgb.data(), image.rgb.size());
    }
    NSImage* picture = [[NSImage alloc] initWithSize:NSMakeSize(image.width, image.height)];
    [picture addRepresentation:rep];
    self.picture.image = picture;
  } else {
    self.picture.image = nil;
  }
  [self.picture setNeedsDisplay:YES];
  std::string text = item.kind;
  if (!item.detail.empty()) {
    text += ' ';
    text += item.detail;
  }
  text += "\n\n";
  text += preview.dump;
  self.structure.string = [NSString stringWithUTF8String:text.c_str()];
  _pcm = preview.playable;
}

static void finishQueue(void* custom, AudioQueueRef queue, AudioQueueBufferRef buffer) {
  (void)custom;
  (void)buffer;
  AudioQueueStop(queue, false);
}

- (void)playSample:(id)sender {
  (void)sender;
  if (_queue != nullptr) {
    AudioQueueStop(_queue, true);
    AudioQueueDispose(_queue, true);
    _queue = nullptr;
  }
  if (_pcm.empty()) {
    return;
  }
  AudioStreamBasicDescription format = {};
  format.mSampleRate = psx_cd_loc::kSpuNominalRate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  format.mBitsPerChannel = 16;
  format.mChannelsPerFrame = 1;
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = 2;
  format.mBytesPerPacket = 2;
  if (AudioQueueNewOutput(&format, finishQueue, nullptr, nullptr, nullptr, 0, &_queue) != noErr) {
    return;
  }
  AudioQueueBufferRef buffer = nullptr;
  const UInt32 bytes = static_cast<UInt32>(_pcm.size() * sizeof(std::int16_t));
  if (AudioQueueAllocateBuffer(_queue, bytes, &buffer) != noErr || buffer == nullptr) {
    return;
  }
  buffer->mAudioDataByteSize = bytes;
  std::memcpy(buffer->mAudioData, _pcm.data(), bytes);
  AudioQueueEnqueueBuffer(_queue, buffer, 0, nullptr);
  AudioQueueStart(_queue, nullptr);
}

@end

int main(int argc, const char** argv) {
  @autoreleasepool {
    NSString* cue = nil;
    for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--cue" && i + 1 < argc) {
        cue = [NSString stringWithUTF8String:argv[++i]];
      }
    }
    NSApplication* app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    XenoDocument* document = [[XenoDocument alloc] initWithCue:cue];
    app.delegate = document;
    [app run];
  }
  return 0;
}

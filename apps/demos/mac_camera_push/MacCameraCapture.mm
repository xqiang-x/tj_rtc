// MacCameraCapture.mm
// AVFoundation 摄像头采集实现（Objective-C++）

#import "MacCameraCapture.h"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <iostream>

// ─────────────────────────────────────────────────────────────
// 内部 Objective-C 代理类：接收 AVCaptureVideoDataOutput 的帧回调
// ─────────────────────────────────────────────────────────────
@interface CaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic, assign) camera::FrameCallback* callbackPtr;
@end

@implementation CaptureDelegate

- (void)captureOutput:(AVCaptureOutput*)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection*)connection
{
    if (!self.callbackPtr || !(*self.callbackPtr)) return;

    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer) return;

    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    (*self.callbackPtr)((CVPixelBufferRef)imageBuffer, pts);
}

- (void)captureOutput:(AVCaptureOutput*)output
  didDropSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection*)connection
{
    // 丢帧时打印一次警告（不阻塞）
    static int dropCount = 0;
    if (++dropCount % 30 == 1) {
        NSLog(@"[Camera] Dropped frame (total: %d)", dropCount);
    }
}

@end

// ─────────────────────────────────────────────────────────────
// MacCameraCapture 实现
// ─────────────────────────────────────────────────────────────
namespace camera {

MacCameraCapture::MacCameraCapture() = default;

MacCameraCapture::~MacCameraCapture() {
    Stop();
}

std::vector<std::string> MacCameraCapture::ListDevices() {
    std::vector<std::string> result;

    AVCaptureDeviceDiscoverySession* session =
        [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera,
                                              AVCaptureDeviceTypeExternal]
                                  mediaType:AVMediaTypeVideo
                                   position:AVCaptureDevicePositionUnspecified];

    for (AVCaptureDevice* dev in session.devices) {
        result.push_back(std::string([dev.localizedName UTF8String]));
    }
    return result;
}

void MacCameraCapture::SetFrameCallback(FrameCallback cb) {
    m_callback = std::move(cb);
}

bool MacCameraCapture::Start(int width, int height, int fps, int deviceIndex) {
    if (m_running) return true;

    // ── 请求摄像头权限 ──────────────────────────────────────
    AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];

    if (status == AVAuthorizationStatusNotDetermined) {
        __block bool granted = false;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                 completionHandler:^(BOOL g) {
            granted = g;
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
        if (!granted) {
            std::cerr << "[Camera] 摄像头权限被拒绝" << std::endl;
            return false;
        }
    } else if (status == AVAuthorizationStatusDenied ||
               status == AVAuthorizationStatusRestricted) {
        std::cerr << "[Camera] 无摄像头权限，请在系统偏好 → 安全性与隐私中授权" << std::endl;
        return false;
    }

    // ── 选择摄像头设备 ────────────────────────────────────────
    AVCaptureDeviceDiscoverySession* discoverSession =
        [AVCaptureDeviceDiscoverySession
            discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera,
                                              AVCaptureDeviceTypeExternal]
                                  mediaType:AVMediaTypeVideo
                                   position:AVCaptureDevicePositionUnspecified];

    NSArray<AVCaptureDevice*>* devices = discoverSession.devices;
    if (devices.count == 0) {
        std::cerr << "[Camera] 未找到摄像头设备" << std::endl;
        return false;
    }

    int idx = (deviceIndex < (int)devices.count) ? deviceIndex : 0;
    AVCaptureDevice* device = devices[idx];
    std::cout << "[Camera] 使用摄像头: " << [device.localizedName UTF8String] << std::endl;

    // ── 配置摄像头格式 & 帧率 ────────────────────────────────
    BOOL formatFound = NO;
    for (AVCaptureDeviceFormat* fmt in device.formats) {
        CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(fmt.formatDescription);
        if (dim.width == width && dim.height == height) {
            for (AVFrameRateRange* range in fmt.videoSupportedFrameRateRanges) {
                if (fps >= range.minFrameRate && fps <= range.maxFrameRate) {
                    NSError* err = nil;
                    if ([device lockForConfiguration:&err]) {
                        device.activeFormat = fmt;
                        device.activeVideoMinFrameDuration = CMTimeMake(1, fps);
                        device.activeVideoMaxFrameDuration = CMTimeMake(1, fps);
                        [device unlockForConfiguration];
                        formatFound = YES;
                    }
                    break;
                }
            }
        }
        if (formatFound) break;
    }

    if (!formatFound) {
        std::cout << "[Camera] 未找到精确匹配的 " << width << "x" << height
                  << "@" << fps << "fps 格式，使用设备默认格式" << std::endl;
    }

    // ── 创建 AVCaptureSession ────────────────────────────────
    AVCaptureSession* session = [[AVCaptureSession alloc] init];

    NSError* inputError = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device
                                                                        error:&inputError];
    if (!input) {
        std::cerr << "[Camera] 创建输入失败: "
                  << [[inputError localizedDescription] UTF8String] << std::endl;
        return false;
    }

    if (![session canAddInput:input]) {
        std::cerr << "[Camera] 无法添加摄像头输入" << std::endl;
        return false;
    }
    [session addInput:input];

    // ── 配置视频输出 ─────────────────────────────────────────
    AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
    output.alwaysDiscardsLateVideoFrames = YES;
    output.videoSettings = @{
        (id)kCVPixelBufferPixelFormatTypeKey :
            @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
    };

    // 使用独立的串行队列处理帧
    dispatch_queue_t queue = dispatch_queue_create("com.demo.camera.capture",
                                                    DISPATCH_QUEUE_SERIAL);

    // 创建代理（生命周期与 MacCameraCapture 绑定）
    CaptureDelegate* delegate = [[CaptureDelegate alloc] init];
    delegate.callbackPtr = &m_callback;
    [output setSampleBufferDelegate:delegate queue:queue];

    if (![session canAddOutput:output]) {
        std::cerr << "[Camera] 无法添加视频输出" << std::endl;
        return false;
    }
    [session addOutput:output];

    // ── 记录实际分辨率 ────────────────────────────────────────
    CMVideoDimensions actualDim =
        CMVideoFormatDescriptionGetDimensions(device.activeFormat.formatDescription);
    m_actualWidth  = actualDim.width;
    m_actualHeight = actualDim.height;

    std::cout << "[Camera] 实际分辨率: " << m_actualWidth << "x" << m_actualHeight << std::endl;

    // ── 保存对象引用 ──────────────────────────────────────────
    // 通过 CFRetain 延长 ARC 对象生命周期（在 Stop 中 CFRelease）
    m_session  = (void*)CFRetain((__bridge CFTypeRef)session);
    m_delegate = (void*)CFRetain((__bridge CFTypeRef)delegate);
    m_output   = (void*)CFRetain((__bridge CFTypeRef)output);
    m_queue    = (void*)queue;  // dispatch_queue_t 本身是引用类型，retain 一次

    // ── 启动采集 ─────────────────────────────────────────────
    [session startRunning];
    m_running = true;

    std::cout << "[Camera] 采集已启动" << std::endl;
    return true;
}

void MacCameraCapture::Stop() {
    if (!m_running) return;
    m_running = false;

    if (m_session) {
        AVCaptureSession* session = (__bridge AVCaptureSession*)m_session;
        [session stopRunning];
        CFRelease(m_session);
        m_session = nullptr;
    }
    if (m_delegate) {
        CFRelease(m_delegate);
        m_delegate = nullptr;
    }
    if (m_output) {
        CFRelease(m_output);
        m_output = nullptr;
    }
    m_queue = nullptr;

    std::cout << "[Camera] 采集已停止" << std::endl;
}

} // namespace camera

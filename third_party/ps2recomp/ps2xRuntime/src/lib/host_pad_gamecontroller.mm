#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include "runtime/host_pad.h"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>

@class RRVControllerMonitor;

class GameControllerHostPadBackend final : public HostPadBackend
{
public:
    GameControllerHostPadBackend();
    ~GameControllerHostPadBackend() override;
    HostPadState snapshot(unsigned port) override;
    HostPadCapabilities capabilities(unsigned port) const override;
    void publish(GCExtendedGamepad *gamepad, GCController *controller);
    void disconnect();

private:
    mutable std::mutex m_mutex;
    HostPadState m_state{};
    HostPadCapabilities m_capabilities{};
    // A reconnect can be selected before the guest observes a disconnect.
    // Keep that handoff on the callback side while snapshot() exposes one
    // neutral cached transition first.
    bool m_emitDisconnectNeutral = false;
    bool m_hasPendingState = false;
    HostPadState m_pendingState{};
    HostPadCapabilities m_pendingCapabilities{};
    RRVControllerMonitor *m_monitor = nil;
};

@interface RRVControllerMonitor : NSObject {
@private
    // This owns all Objective-C controller/handler lifecycle transitions.
    // _callbackMutex separately protects the C++ backend lifetime while a
    // value callback is publishing its already-captured host snapshot.
    std::mutex _lifecycleMutex;
    std::mutex _callbackMutex;
    bool _stopping;
    GameControllerHostPadBackend *_backend;
}
@property(nonatomic, retain) GCController *selectedController;
@property(nonatomic, copy) NSString *deviceFilter;
- (instancetype)initWithBackend:(GameControllerHostPadBackend *)backend;
- (void)start;
- (void)stop;
- (void)publishGamepad:(GCExtendedGamepad *)gamepad controller:(GCController *)controller;
- (void)disconnectBackend;
- (void)reselectControllerExcluding:(GCController *)disconnectedController;
- (void)scheduleReselectionLockedExcluding:(GCController *)disconnectedController;
@end

@implementation RRVControllerMonitor

- (instancetype)initWithBackend:(GameControllerHostPadBackend *)backend
{
    self = [super init];
    if (self)
    {
        _backend = backend;
        _stopping = false;
        const char *filter = std::getenv("RRV_PAD_DEVICE");
        _deviceFilter = filter && filter[0] ? [[NSString alloc] initWithUTF8String:filter] : nil;
    }
    return self;
}

- (BOOL)matches:(GCController *)controller
{
    if (!_deviceFilter || [_deviceFilter length] == 0)
        return YES;
    NSString *vendor = controller.vendorName ?: @"";
    NSString *category = controller.productCategory ?: @"";
    return [vendor rangeOfString:_deviceFilter options:NSCaseInsensitiveSearch].location != NSNotFound ||
           [category rangeOfString:_deviceFilter options:NSCaseInsensitiveSearch].location != NSNotFound;
}

- (void)selectControllerLocked:(GCController *)controller
{
    if (_stopping || _selectedController || !controller || ![self matches:controller] || !controller.extendedGamepad)
        return;
    self.selectedController = controller; // Stable until this physical controller disconnects.
    // The copied handler retains the monitor. stop clears the handler to
    // break this cycle after it has nulled the guarded C++ backend pointer.
    RRVControllerMonitor *monitor = self;
    controller.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *gamepad, GCControllerElement *) {
        [monitor publishGamepad:gamepad controller:controller];
    };
    // selectControllerLocked already owns _lifecycleMutex, so publish this
    // initial cached state directly rather than recursively taking it again.
    {
        std::lock_guard<std::mutex> callbackLock(_callbackMutex);
        if (_backend)
            _backend->publish(controller.extendedGamepad, controller);
    }
    if (std::getenv("RRV_PAD_DIAG") && std::strcmp(std::getenv("RRV_PAD_DIAG"), "0") != 0)
        fprintf(stderr, "[pad] GameController selected vendor=%s category=%s\n",
                (controller.vendorName ?: @"").UTF8String, (controller.productCategory ?: @"").UTF8String);
}

- (void)selectController:(GCController *)controller
{
    std::lock_guard<std::mutex> lock(_lifecycleMutex);
    [self selectControllerLocked:controller];
}

- (void)controllerConnected:(NSNotification *)notification
{
    [self selectController:(GCController *)notification.object];
}

- (void)controllerDisconnected:(NSNotification *)notification
{
    std::lock_guard<std::mutex> lock(_lifecycleMutex);
    if (_stopping)
        return;
    GCController *controller = (GCController *)notification.object;
    if (_selectedController == controller)
    {
        _selectedController.extendedGamepad.valueChangedHandler = nil;
        self.selectedController = nil;
        [self disconnectBackend];
        // Do not make GameController enumeration part of a guest snapshot.
        // Reprobe from a queued platform callback after staging the neutral
        // transition, so an already-connected compatible controller can take
        // over without a restart.
        [self scheduleReselectionLockedExcluding:controller];
    }
}

- (void)reselectControllerExcluding:(GCController *)disconnectedController
{
    std::lock_guard<std::mutex> lock(_lifecycleMutex);
    if (_stopping || _selectedController)
        return;
    for (GCController *controller in GCController.controllers)
    {
        // A disconnect notification can precede GameController removing A
        // from its controller list.  Do not immediately reacquire that stale
        // object; a later connect notification remains eligible as normal.
        if (controller != disconnectedController)
            [self selectControllerLocked:controller];
    }
}

- (void)scheduleReselectionLockedExcluding:(GCController *)disconnectedController
{
    // The caller owns _lifecycleMutex.  The copied block retains monitor only
    // until the queued task runs; stop() makes it a no-op through _stopping.
    RRVControllerMonitor *monitor = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [monitor reselectControllerExcluding:disconnectedController];
    });
}

- (void)start
{
    std::lock_guard<std::mutex> lock(_lifecycleMutex);
    if (_stopping)
        return;
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserver:self selector:@selector(controllerConnected:) name:GCControllerDidConnectNotification object:nil];
    [center addObserver:self selector:@selector(controllerDisconnected:) name:GCControllerDidDisconnectNotification object:nil];
    for (GCController *controller in GCController.controllers)
        [self selectControllerLocked:controller];
}

- (void)stop
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    if (_stopping)
        return;
    // Mark stopping before observers are removed: queued connect/disconnect
    // notifications and value callbacks must not reinstall a handler after
    // the C++ backend has begun teardown.
    _stopping = true;
    [NSNotificationCenter.defaultCenter removeObserver:self];
    _selectedController.extendedGamepad.valueChangedHandler = nil;
    self.selectedController = nil;
    // Serializes with an in-flight callback. Once this returns no callback can
    // reach the C++ object that is about to be destroyed.
    [self disconnectBackend];
    std::lock_guard<std::mutex> callbackLock(_callbackMutex);
    _backend = nullptr;
}

- (void)publishGamepad:(GCExtendedGamepad *)gamepad controller:(GCController *)controller
{
    std::lock_guard<std::mutex> lifecycleLock(_lifecycleMutex);
    // A queued handler from a just-disconnected/replaced controller is stale.
    // Ignore it rather than allowing it to overwrite the cached snapshot.
    if (_stopping || _selectedController != controller)
        return;
    std::lock_guard<std::mutex> lock(_callbackMutex);
    if (_backend)
        _backend->publish(gamepad, controller);
}

- (void)disconnectBackend
{
    std::lock_guard<std::mutex> lock(_callbackMutex);
    if (_backend)
        _backend->disconnect();
}

- (void)dealloc
{
    [_deviceFilter release];
    [super dealloc];
}
@end

namespace
{
bool pressed(GCControllerButtonInput *button) { return button && button.isPressed; }
uint64_t controllerId(GCController *controller)
{
    NSString *vendor = controller.vendorName ?: @"";
    NSString *category = controller.productCategory ?: @"";
    const std::string identity = std::string(vendor.UTF8String ?: "") + ":" + (category.UTF8String ?: "");
    return static_cast<uint64_t>(std::hash<std::string>{}(identity));
}
}

GameControllerHostPadBackend::GameControllerHostPadBackend()
{
    m_monitor = [[RRVControllerMonitor alloc] initWithBackend:this];
    [m_monitor start];
}

GameControllerHostPadBackend::~GameControllerHostPadBackend()
{
    [m_monitor stop];
    [m_monitor release];
}

HostPadState GameControllerHostPadBackend::snapshot(unsigned port)
{
    if (port != 0)
        return {};
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_emitDisconnectNeutral)
    {
        m_emitDisconnectNeutral = false;
        if (m_hasPendingState)
        {
            m_state = m_pendingState;
            m_capabilities = m_pendingCapabilities;
            m_hasPendingState = false;
        }
        return {};
    }
    return m_state;
}

HostPadCapabilities GameControllerHostPadBackend::capabilities(unsigned port) const
{
    if (port != 0)
        return {};
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_capabilities;
}

void GameControllerHostPadBackend::publish(GCExtendedGamepad *gamepad, GCController *controller)
{
    HostPadState state{};
    if (gamepad)
    {
        state.connected = true;
        state.deviceId = controllerId(controller);
        auto set = [&state](HostPadButton button, bool isPressed) {
            if (isPressed) state.pressedButtons |= static_cast<uint32_t>(button);
        };
        set(HostPadButtonUp, pressed(gamepad.dpad.up));
        set(HostPadButtonDown, pressed(gamepad.dpad.down));
        set(HostPadButtonLeft, pressed(gamepad.dpad.left));
        set(HostPadButtonRight, pressed(gamepad.dpad.right));
        // Apple A/B/X/Y are bottom/right/left/top respectively: Cross,
        // Circle, Square, Triangle independent of controller brand or transport.
        set(HostPadButtonCross, pressed(gamepad.buttonA));
        set(HostPadButtonCircle, pressed(gamepad.buttonB));
        set(HostPadButtonSquare, pressed(gamepad.buttonX));
        set(HostPadButtonTriangle, pressed(gamepad.buttonY));
        set(HostPadButtonL1, pressed(gamepad.leftShoulder));
        set(HostPadButtonR1, pressed(gamepad.rightShoulder));
        set(HostPadButtonL3, pressed(gamepad.leftThumbstickButton));
        set(HostPadButtonR3, pressed(gamepad.rightThumbstickButton));
        set(HostPadButtonStart, pressed(gamepad.buttonMenu));
        set(HostPadButtonSelect, pressed(gamepad.buttonOptions));
        state.leftX = gamepad.leftThumbstick.xAxis.value;
        state.leftY = gamepad.leftThumbstick.yAxis.value; // GameController is positive-up.
        state.rightX = gamepad.rightThumbstick.xAxis.value;
        state.rightY = gamepad.rightThumbstick.yAxis.value;
        state.leftTrigger = gamepad.leftTrigger.value;
        state.rightTrigger = gamepad.rightTrigger.value;
        set(HostPadButtonL2, pressed(gamepad.leftTrigger));
        set(HostPadButtonR2, pressed(gamepad.rightTrigger));
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const HostPadCapabilities capabilities = gamepad ? HostPadCapabilities{true, true, false} : HostPadCapabilities{};
    if (m_emitDisconnectNeutral)
    {
        m_pendingState = state;
        m_pendingCapabilities = capabilities;
        m_hasPendingState = true;
        return;
    }
    m_state = state;
    m_capabilities = capabilities;
}

void GameControllerHostPadBackend::disconnect()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = {};
    m_capabilities = {};
    m_pendingState = {};
    m_pendingCapabilities = {};
    m_hasPendingState = false;
    m_emitDisconnectNeutral = true;
}

std::shared_ptr<HostPadBackend> createGameControllerHostPadBackend()
{
    return std::make_shared<GameControllerHostPadBackend>();
}

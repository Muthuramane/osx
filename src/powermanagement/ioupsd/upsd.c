/*
 * Copyright (c) 1998-2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */
//---------------------------------------------------------------------------
// Includes
//---------------------------------------------------------------------------

#include <syslog.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_error.h>
#include <libc.h>
#include <servers/bootstrap.h>
#include <sysexits.h>
#include <notify.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/IOKitServer.h>
#include <IOKit/IOCFURLAccess.h>
#include <IOKit/IOCFSerialize.h>
#include <IOKit/IOCFUnserialize.h>
#include <IOKit/IOMessage.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/AppleHIDUsageTables.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPowerSourcesPrivate.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMLibPrivate.h>

#include <SystemConfiguration/SystemConfiguration.h>
#if UPS_DEBUG
#include <SystemConfiguration/SCPrivate.h>
#endif

#if TARGET_OS_EMBEDDED
#include <IOKit/accessory/IOAccessoryManagerLib.h>
#include <IOKit/platform/AppleARMPMUKeys.h>
#endif

#include <IOKit/ps/IOUPSPlugIn.h>
#include "IOUPSPrivate.h"

#define kDefaultUPSName		"Generic UPS"
#define kDefaultTransport   "UNK"

//---------------------------------------------------------------------------
// Globals
//---------------------------------------------------------------------------
static CFRunLoopSourceRef       gClientRequestRunLoopSource = NULL;
static CFRunLoopRef             gMainRunLoop = NULL;
static CFMutableArrayRef        gUPSDataArrayRef = NULL;
static unsigned int             gUPSCount = 0;
static IONotificationPortRef	gNotifyPort = NULL;
static io_iterator_t            gAddedIter = MACH_PORT_NULL;

//---------------------------------------------------------------------------
// TypeDefs
//---------------------------------------------------------------------------
typedef struct UPSData {
    IOPSPowerSourceID       powerSourceID;
    io_object_t             notification;
    IOUPSPlugInInterface    **upsPlugInInterface;
    int                     upsID;
    Boolean                 isPresent;
    CFMutableDictionaryRef  upsStoreDict;
    CFRunLoopSourceRef      upsEventSource;
    CFRunLoopTimerRef       upsEventTimer;
    Boolean                 isBatteryCase;
    Boolean                 isAccessoryBattery;
    Boolean                 hasACPower;
    io_object_t             batteryStateNotification;
    io_object_t             currentLimitNotification;
    io_object_t             requiredVoltageNotification;
} UPSData;

typedef UPSData *UPSDataRef;


//---------------------------------------------------------------------------
// Methods
//---------------------------------------------------------------------------
void CleanupAndExit(void);
static void SignalHandler(int sigraised);
static void InitUPSNotifications(int usagePages[], int usages[], int count);
static void ProcessUPSEventSource(CFTypeRef typeRef, CFRunLoopTimerRef * pTimer, CFRunLoopSourceRef * pSource);
static void UPSDeviceAdded(void *refCon, io_iterator_t iterator);
static void DeviceNotification(void *refCon, io_service_t service,
                               natural_t messageType, void *messageArgument);
static void RemoveAndReleasePowerManagerUPSEntry(UPSDataRef upsDataRef);
static void UPSEventCallback(void * target, IOReturn result, void *refcon,
                             void *sender, CFDictionaryRef event);
static void ProcessUPSEvent(UPSDataRef upsDataRef, CFDictionaryRef event);
static void BatteryCaseHandleACStateChange(UPSDataRef upsDataRef, CFTypeRef powerState);
static UPSDataRef GetPrivateData( CFDictionaryRef properties );
static IOReturn CreatePowerManagerUPSEntry(UPSDataRef upsDataRef,
                                           CFDictionaryRef properties,
                                           CFSetRef capabilities);
static Boolean SetupMIGServer();
static int scheduleRTCWake(io_service_t chargerService, int deadlineSec);

//---------------------------------------------------------------------------
// Battery case helper functions
//---------------------------------------------------------------------------
kern_return_t BatteryCaseSendCommand(UPSDataRef upsDataRef, CFStringRef commandString, SInt32 value);
void BatteryCaseBatteryStateChangedCallback(void *refcon, io_service_t service,
                                            uint32_t messageType, void *messageArgument);
void BatteryCaseCurrentLimitChangeCallback(void *refcon, io_service_t service,
                                           uint32_t messageType, void *messageArgument);
void BatteryCaseRequiredVoltageChangeCallback(void *refcon, io_service_t service,
                                              uint32_t messageType, void *messageArgument);

//---------------------------------------------------------------------------
// main
//
//---------------------------------------------------------------------------
int main (int argc, const char *argv[]) {
    openlog("upsd", LOG_PID|LOG_NDELAY, LOG_USER);
    signal(SIGINT, SignalHandler);
    SetupMIGServer();
    
    // Create a notification port and add its run loop event source to our run
    // loop. This is how async notifications get set up.
    gNotifyPort = IONotificationPortCreate(kIOMasterPortDefault);
    CFRunLoopAddSource(CFRunLoopGetCurrent(),
                       IONotificationPortGetRunLoopSource(gNotifyPort),
                       kCFRunLoopDefaultMode);
    
    // Listen for any HID Power Devices or Battery Systems
    int usagePages[]    = {kIOPowerDeviceUsageKey,  kIOBatterySystemUsageKey,   kHIDPage_AppleVendor};
    int usages[]        = {0,                       0,                          kHIDUsage_AppleVendor_AccessoryBattery};
    InitUPSNotifications(usagePages, usages, sizeof(usagePages)/sizeof(usagePages[0]));
    
    CFRunLoopRun();
    
    return 0;
}


//---------------------------------------------------------------------------
// SignalHandler
//---------------------------------------------------------------------------
void CleanupAndExit(void) {
    // Clean up here
    IONotificationPortDestroy(gNotifyPort);
    if (gAddedIter) {
        IOObjectRelease(gAddedIter);
        gAddedIter = 0;
    }
    exit(0);
}


//---------------------------------------------------------------------------
// SignalHandler
//---------------------------------------------------------------------------
void SignalHandler(int sigraised) {
    syslog(LOG_INFO, "upsd: exiting SIGINT\n");
    CleanupAndExit();
}


//---------------------------------------------------------------------------
// SetupMIGServer
//---------------------------------------------------------------------------
extern void upsd_mach_port_callback(CFMachPortRef port, void *msg, CFIndex size,
                                    void *info);

Boolean SetupMIGServer() {
    Boolean         result = true;
    kern_return_t   kern_result = KERN_SUCCESS;
    CFMachPortRef   upsdMachPort = NULL;  // must release
    mach_port_t     ups_port = MACH_PORT_NULL;
    
    /*if (IOUPSMIGServerIsRunning(&bootstrap_port, NULL)) {
     result = false;
     goto finish;
     }*/
    
    kern_result = task_get_bootstrap_port(mach_task_self(), &bootstrap_port);
    if (kern_result != KERN_SUCCESS) {
        result = false;
        goto finish;
    }
    
    gMainRunLoop = CFRunLoopGetCurrent();
    if (!gMainRunLoop) {
        result = false;
        goto finish;
    }
    
    
    kern_result = bootstrap_check_in(bootstrap_port, kIOUPSPlugInServerName,
                                     &ups_port);
    
    if (BOOTSTRAP_SUCCESS != kern_result) {
        syslog(LOG_ERR, "ioupsd: bootstrap_check_in \"%s\" error = %d\n",
               kIOUPSPlugInServerName, kern_result);
    } else {
        
        upsdMachPort = CFMachPortCreateWithPort(kCFAllocatorDefault, ups_port,
                                                upsd_mach_port_callback, NULL,
                                                NULL);
        gClientRequestRunLoopSource = CFMachPortCreateRunLoopSource(
                                                                    kCFAllocatorDefault, upsdMachPort, 0);
        if (!gClientRequestRunLoopSource) {
            result = false;
            goto finish;
        }
        CFRunLoopAddSource(gMainRunLoop, gClientRequestRunLoopSource,
                           kCFRunLoopDefaultMode);
    }
finish:
    if (gClientRequestRunLoopSource) CFRelease(gClientRequestRunLoopSource);
    if (upsdMachPort) CFRelease(upsdMachPort);
    
    return result;
}

//---------------------------------------------------------------------------
// InitUPSNotifications
//
// This routine just creates our master port for IOKit and turns around
// and calls the routine that will alert us when a UPS Device is plugged in.
//---------------------------------------------------------------------------

void InitUPSNotifications(int usagePages[], int usages[], int count) {
    CFMutableArrayRef devicePairs = NULL;
    CFMutableDictionaryRef matchingDict = NULL;
    
    matchingDict = IOServiceMatching(kIOHIDDeviceKey);
    if (!matchingDict)
        goto ERROR;
    
    devicePairs = CFArrayCreateMutable(kCFAllocatorDefault, 3, &kCFTypeArrayCallBacks);
    if (!devicePairs)
        goto ERROR;
    
    for (int i = 0; i < count; i++) {
        CFMutableDictionaryRef pair = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (!pair)
            goto ERROR;
        
        // We need to box the Usage Page value up into a CFNumber... sorry bout that
        CFNumberRef cfUsagePageKey = CFNumberCreate(kCFAllocatorDefault,
                                                    kCFNumberIntType,
                                                    &usagePages[i]);
        if (!cfUsagePageKey)
            goto ERROR;
        
        CFDictionarySetValue(pair,
                             CFSTR(kIOHIDDeviceUsagePageKey),
                             cfUsagePageKey);
        CFRelease(cfUsagePageKey);
        
        if (usages[i]) {
            CFNumberRef cfUsageKey = CFNumberCreate(kCFAllocatorDefault,
                                                    kCFNumberIntType,
                                                    &usages[i]);
            if (!cfUsageKey)
                goto ERROR;
            
            CFDictionarySetValue(pair,
                                 CFSTR(kIOHIDDeviceUsageKey),
                                 cfUsageKey);
            CFRelease(cfUsageKey);
        }
        
        CFArrayAppendValue(devicePairs, pair);
        CFRelease(pair);
    }
    
    CFDictionarySetValue(matchingDict, CFSTR(kIOHIDDeviceUsagePairsKey), devicePairs);
    CFRelease(devicePairs);
    devicePairs = 0;
    
    // Now set up a notification to be called when a device is first matched by
    // I/O Kit. Note that this will not catch any devices that were already
    // plugged in so we take care of those later.
    kern_return_t kr =
    IOServiceAddMatchingNotification(gNotifyPort,
                                     kIOFirstMatchNotification,
                                     matchingDict,
                                     UPSDeviceAdded,
                                     NULL,
                                     &gAddedIter);
    
    matchingDict = 0; // reference consumed by AddMatchingNotification
    if (kr == kIOReturnSuccess) {
        // Check for existing matching devices
        UPSDeviceAdded(NULL, gAddedIter);
    }
    
ERROR:
    
    if (matchingDict)
        CFRelease(matchingDict);
    
    if (devicePairs)
        CFRelease(devicePairs);

    return;
}

//---------------------------------------------------------------------------
// ProcessUPSEventSource
//
// Performs cast on EventSource to determine if this is a timer or normal
// event source.
//---------------------------------------------------------------------------
void ProcessUPSEventSource(CFTypeRef typeRef, CFRunLoopTimerRef * pTimer, CFRunLoopSourceRef * pSource)
{
    if ( CFGetTypeID(typeRef) == CFRunLoopTimerGetTypeID() )
    {
        *pTimer = (CFRunLoopTimerRef)typeRef;
    }
    else if ( CFGetTypeID(typeRef) == CFRunLoopSourceGetTypeID() )
    {
        *pSource = (CFRunLoopSourceRef)typeRef;
    }
}

//---------------------------------------------------------------------------
// UPSDeviceAdded
//
// This routine is the callback for our IOServiceAddMatchingNotification.
// When we get called we will look at all the devices that were added and
// we will:
//
// Create some private data to relate to each device
//
// Submit an IOServiceAddInterestNotification of type kIOGeneralInterest for
// this device using the refCon field to store a pointer to our private data.
// When we get called with this interest notification, we can grab the refCon
// and access our private data.
//---------------------------------------------------------------------------

void UPSDeviceAdded(void *refCon, io_iterator_t iterator) {
    io_object_t             upsDevice           = MACH_PORT_NULL;
    UPSDataRef              upsDataRef          = NULL;
    CFDictionaryRef         upsProperties       = NULL;
    CFDictionaryRef         upsEvent            = NULL;
    CFSetRef                upsCapabilites 		= NULL;
    CFRunLoopSourceRef      upsEventSource      = NULL;
    CFRunLoopTimerRef       upsEventTimer       = NULL;
    CFTypeRef               typeRef             = NULL;
    IOCFPlugInInterface **  plugInInterface 	= NULL;
    IOUPSPlugInInterface_v140 ** upsPlugInInterface = NULL;
    HRESULT                 result              = S_FALSE;
    IOReturn                kr;
    SInt32                  score;
    
    while ( (upsDevice = IOIteratorNext(iterator)) ) {
        // Create the CF plugin for this device
        kr = IOCreatePlugInInterfaceForService(upsDevice, kIOUPSPlugInTypeID,
                                               kIOCFPlugInInterfaceID,
                                               &plugInInterface, &score);
        
        if (kr != kIOReturnSuccess)
            goto UPSDEVICEADDED_NONPLUGIN_CLEANUP;
        
        // Grab the new v140 interface
        result = (*plugInInterface)->QueryInterface(plugInInterface,
                                                    CFUUIDGetUUIDBytes(kIOUPSPlugInInterfaceID_v140),
                                                    (LPVOID)&upsPlugInInterface);
        
        if ( ( result == S_OK ) && upsPlugInInterface )
        {
            kr = (*upsPlugInInterface)->createAsyncEventSource(upsPlugInInterface, &typeRef);
            
            if ((kr != kIOReturnSuccess) || !typeRef)
                goto UPSDEVICEADDED_FAIL;
            
            if ( CFGetTypeID(typeRef) == CFArrayGetTypeID() )
            {
                CFArrayRef  arrayRef = (CFArrayRef)typeRef;
                CFIndex     index, count;
                
                for (index=0, count=CFArrayGetCount(typeRef); index<count; index++)
                    ProcessUPSEventSource(CFArrayGetValueAtIndex(arrayRef, index), &upsEventTimer, &upsEventSource);
            }
            else
            {
                ProcessUPSEventSource(typeRef, &upsEventTimer, &upsEventSource);
            }
            
            if ( upsEventSource )
                CFRunLoopAddSource(CFRunLoopGetCurrent(), upsEventSource, kCFRunLoopDefaultMode);
            
            if ( upsEventTimer )
                CFRunLoopAddTimer(CFRunLoopGetCurrent(), upsEventTimer, kCFRunLoopDefaultMode);
            
            if ( typeRef )
                CFRelease(typeRef);
        }
        // Couldn't grab the new interface.  Fallback on the old.
        else
        {
            result = (*plugInInterface)->QueryInterface(plugInInterface, CFUUIDGetUUIDBytes(kIOUPSPlugInInterfaceID),
                                                        (LPVOID)&upsPlugInInterface);
        }

        // Got the interface
        if ( ( result == S_OK ) && upsPlugInInterface )
        {
            kr = (*upsPlugInInterface)->getProperties(upsPlugInInterface, &upsProperties);
            
            if (kr != kIOReturnSuccess)
                goto UPSDEVICEADDED_FAIL;
            
            upsDataRef = GetPrivateData(upsProperties);
            
            if ( !upsDataRef )
                goto UPSDEVICEADDED_FAIL;
            
            upsDataRef->upsPlugInInterface  = (IOUPSPlugInInterface **)upsPlugInInterface;
            upsDataRef->upsEventSource      = upsEventSource;
            upsDataRef->upsEventTimer       = upsEventTimer;
            upsDataRef->isPresent           = true;
            
            
            // On iOS HID UPS devices that describe themself with:
            //    Primary Usage Page: Battery System (0x85)
            //    Usage:              Primary Battery (0x2E)
            // are considered to be battery cases which upsd is responsible for
            // managing the current draw to and from. This is because these
            // devices cannot identify power sources plugged into them, since
            // they pass that information straight through to the iOS device.
            //
            
            CFArrayRef usagePairs = IORegistryEntrySearchCFProperty(upsDevice,
                                                                         kIOServicePlane,
                                                                         CFSTR(kIOHIDDeviceUsagePairsKey),
                                                                         kCFAllocatorDefault,
                                                                         NULL);

            if (usagePairs) {
                int     i           = 0;
                CFIndex count       = CFArrayGetCount(usagePairs);
                int     usagePage   = 0;
                int     usage       = 0;
                
                for (;i < count; i++) {
                    CFDictionaryRef usagePair = (CFDictionaryRef)CFArrayGetValueAtIndex(usagePairs, i);
                    if (!usagePair) continue;
                    
                    CFNumberRef usagePageRef = CFDictionaryGetValue(usagePair, CFSTR(kIOHIDDeviceUsagePageKey));
                    CFNumberRef usageRef = CFDictionaryGetValue(usagePair, CFSTR(kIOHIDDeviceUsageKey));
                    
                    if (usagePageRef) {
                        CFNumberGetValue(usagePageRef, kCFNumberIntType, &usagePage);
                    }
                    
                    if (usageRef) {
                        CFNumberGetValue(usageRef, kCFNumberIntType, &usage);
                    }
                    
                    if ((usagePage == kHIDPage_AppleVendor) && (usage == kHIDUsage_AppleVendor_AccessoryBattery)) {
                        upsDataRef->isAccessoryBattery = true;
                    }
                    else if ((usagePage == kHIDPage_BatterySystem) && (usage == kHIDUsage_BS_PrimaryBattery)) {
                        upsDataRef->isBatteryCase = true;
                        upsDataRef->hasACPower = false;
                    }
                }
                
                CFRelease(usagePairs);
            }
            
            kr = (*upsPlugInInterface)->getCapabilities(upsPlugInInterface, &upsCapabilites);
            
            if (kr != kIOReturnSuccess)
                goto UPSDEVICEADDED_FAIL;
            
            kr = CreatePowerManagerUPSEntry(upsDataRef, upsProperties, upsCapabilites);
            
            if (kr != kIOReturnSuccess)
                goto UPSDEVICEADDED_FAIL;
            
            if (upsDataRef->isBatteryCase) {
                // Initialize AC state manually according to the default value.
                // If a UPS is not on battery power, this will get fixed in the
                // first ProcessUPSEvent call below
                BatteryCaseHandleACStateChange(upsDataRef, CFSTR(kIOPSBatteryPowerValue));
                
                // Register for interest in battery state changes to update
                // battery cases on our state of charge
                io_service_t chargerService = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                                          IOServiceMatching("AppleARMPMUCharger"));
                
                if (chargerService != MACH_PORT_NULL) {
                    kr = IOServiceAddInterestNotification(gNotifyPort,
                                                          chargerService,
                                                          kIOGeneralInterest,
                                                          BatteryCaseBatteryStateChangedCallback,
                                                          upsDataRef,
                                                          &(upsDataRef->batteryStateNotification));
                }
            }
            
            kr = (*upsPlugInInterface)->getEvent(upsPlugInInterface, &upsEvent);
            
            if (kr != kIOReturnSuccess)
                goto UPSDEVICEADDED_FAIL;
            
            ProcessUPSEvent(upsDataRef, upsEvent);
            
            (*upsPlugInInterface)->setEventCallback(upsPlugInInterface,
                                                    UPSEventCallback, NULL,
                                                    upsDataRef);
            
            kr = IOServiceAddInterestNotification(gNotifyPort, // notifyPort
                                                  upsDevice, // service
                                                  kIOGeneralInterest, // interestType
                                                  DeviceNotification, // callback
                                                  upsDataRef, // refCon
                                                  &(upsDataRef->notification)); // notification
            if (kr != kIOReturnSuccess)
                goto UPSDEVICEADDED_FAIL;
            
            notify_post(kIOPSNotifyAttach);
            goto UPSDEVICEADDED_CLEANUP;
        }
        
        
    UPSDEVICEADDED_FAIL:
        // Failed to allocate a UPS interface.  Do some cleanup
        if (upsPlugInInterface) {
            (*upsPlugInInterface)->Release(upsPlugInInterface);
            upsPlugInInterface = NULL;
        }
        
        if (upsEventSource) {
            CFRunLoopRemoveSource(CFRunLoopGetCurrent(), upsEventSource,
                                  kCFRunLoopDefaultMode);
            upsEventSource = NULL;
        }
        
        if (upsEventTimer) {
            CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), upsEventTimer,
                                 kCFRunLoopDefaultMode);
            upsEventSource = NULL;
        }
        if (upsDataRef) {
            RemoveAndReleasePowerManagerUPSEntry(upsDataRef);
            upsDataRef = NULL;
        }
        
    UPSDEVICEADDED_CLEANUP:
        // Clean up
        (*plugInInterface)->Release(plugInInterface);
        
    UPSDEVICEADDED_NONPLUGIN_CLEANUP:
        IOObjectRelease(upsDevice);
    }
}

//---------------------------------------------------------------------------
// DeviceNotification
//
// This routine will get called whenever any kIOGeneralInterest notification
// happens.
//---------------------------------------------------------------------------

void DeviceNotification(void *refCon, io_service_t service,
                        natural_t messageType, void *messageArgument ) {
    UPSDataRef upsDataRef = (UPSDataRef) refCon;
    
    if ((upsDataRef != NULL) && (messageType == kIOMessageServiceIsTerminated)) {
        RemoveAndReleasePowerManagerUPSEntry(upsDataRef);
    }
}


//---------------------------------------------------------------------------
// RemoveAndReleasePowerManagerUPSEntry
//
// Remove a UPS from the list of active power sources and perform all
// necessary cleanup.
//
// No-op if upsDataRef is NULL
//---------------------------------------------------------------------------
void RemoveAndReleasePowerManagerUPSEntry(UPSDataRef upsDataRef) {
    if (upsDataRef == NULL)
        return;
    
    upsDataRef->isPresent = FALSE;
    
    notify_post(kIOPSNotifyAttach);
    
    if (upsDataRef->upsEventSource) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(),
                              upsDataRef->upsEventSource,
                              kCFRunLoopDefaultMode);
        CFRelease(upsDataRef->upsEventSource);
        upsDataRef->upsEventSource = NULL;
    }
    
    if (upsDataRef->upsEventTimer) {
        CFRunLoopRemoveTimer(CFRunLoopGetCurrent(),
                             upsDataRef->upsEventTimer,
                             kCFRunLoopDefaultMode);
        CFRelease(upsDataRef->upsEventTimer);
        upsDataRef->upsEventTimer = NULL;
    }
    
    if (upsDataRef->upsPlugInInterface != NULL) {
        (*(upsDataRef->upsPlugInInterface))->Release(upsDataRef->upsPlugInInterface);
        upsDataRef->upsPlugInInterface = NULL;
    }
    
    if (upsDataRef->batteryStateNotification != MACH_PORT_NULL) {
        IOObjectRelease(upsDataRef->batteryStateNotification);
        upsDataRef->batteryStateNotification = MACH_PORT_NULL;
    }
    
    if (upsDataRef->currentLimitNotification != MACH_PORT_NULL) {
        IOObjectRelease(upsDataRef->currentLimitNotification);
        upsDataRef->currentLimitNotification = MACH_PORT_NULL;
    }
    
    if (upsDataRef->requiredVoltageNotification != MACH_PORT_NULL) {
        IOObjectRelease(upsDataRef->requiredVoltageNotification);
        upsDataRef->requiredVoltageNotification = MACH_PORT_NULL;
    }
    
    if (upsDataRef->notification != MACH_PORT_NULL) {
        IOObjectRelease(upsDataRef->notification);
        upsDataRef->notification = MACH_PORT_NULL;
    }
    
    if (upsDataRef->upsStoreDict) {
        CFRelease(upsDataRef->upsStoreDict);
        upsDataRef->upsStoreDict = NULL;
    }
    
    if (upsDataRef->powerSourceID) {
        IOReturn result = IOPSReleasePowerSource(upsDataRef->powerSourceID);
        gUPSCount--;
        if (result != kIOReturnSuccess) {
            syslog(LOG_ERR, "IOPSReleasePowerSource failed (IOReturn: %d)\n", result);
        }
        upsDataRef->powerSourceID = NULL;
    }
    
    if (gUPSCount == 0) {
        CFRelease(gUPSDataArrayRef);
        CleanupAndExit();
    }
}

//---------------------------------------------------------------------------
// UPSEventCallback
//
// This routine will get called whenever any data is available from the UPS
//---------------------------------------------------------------------------
void UPSEventCallback(void *target, IOReturn result, void *refcon, void *sender,
                      CFDictionaryRef event) {
    ProcessUPSEvent((UPSDataRef) refcon, event);
}

//---------------------------------------------------------------------------
// BatteryCaseSetDeviceCurrentLimit
//
// Sets the current limit we can draw from a battery case.
//---------------------------------------------------------------------------
kern_return_t BatteryCaseSetDeviceCurrentLimit(CFTypeRef currentLimitRef) {
#if TARGET_OS_EMBEDDED
    kern_return_t kr = KERN_FAILURE;
    SInt32 currentLimit, baseCurrentLimit;
    io_service_t service = NULL;
    io_connect_t connect = NULL;
    
    // Ensure the argument is a CFNumber
    if (CFGetTypeID(currentLimitRef) != CFNumberGetTypeID()) {
        return KERN_INVALID_ARGUMENT;
    }
    
    CFNumberGetValue(currentLimitRef, kCFNumberSInt32Type, &currentLimit);
    
    // If the battery case ever tells us to charge below 200 mA, it means we've
    // sent them an iLim of 100 mA (USB unconfigured). Ignore this.
    if (currentLimit < 200) {
        syslog(LOG_INFO, "ignoring battery case current limit < 200 (limit=%d)", currentLimit);
        return KERN_SUCCESS;
    }
    
    // Open a connection to the IOAccessoryManager
    service = IOAccessoryManagerGetServiceWithPrimaryPort(kIOAccessoryPortIDSerial);
    if (service == IO_OBJECT_NULL) {
        syslog(LOG_ERR, "could not find IOAccessoryManager service for port %d\n", kIOAccessoryPortIDSerial);
        return kIOReturnNoDevice;
    }
    kr = IOServiceOpen(service, mach_task_self(), 0, &connect);
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "could not open IOAccessoryManager service: %s\n", mach_error_string(kr));
        return kr;
    }
    
    kr = IOAccessoryManagerGetUSBCurrentLimitBase(service, &baseCurrentLimit);
    if (kr == KERN_SUCCESS) {
        kr = IOAccessoryManagerSetUSBCurrentOffset(connect, currentLimit - baseCurrentLimit);
    }
    
    IOServiceClose(connect);
    return kr;
#else
    // NOOP on OS X
    return KERN_NOT_SUPPORTED;
#endif
}

//---------------------------------------------------------------------------
// ProcessUPSEvent
//
//---------------------------------------------------------------------------
void ProcessUPSEvent(UPSDataRef upsDataRef, CFDictionaryRef event) {
    long count, index;
    
    if (!upsDataRef || !event)
        return;
    
    if ((count = CFDictionaryGetCount(event))) {
        CFTypeRef *keys     = (CFTypeRef *) malloc(sizeof(CFTypeRef) * count);
        CFTypeRef *values   = (CFTypeRef *) malloc(sizeof(CFTypeRef) * count);
        
        CFDictionaryGetKeysAndValues(event, (const void **)keys,
                                     (const void **)values);
        
        for (index = 0; index < count; index++) {
            // If a battery case changes from "unplugged" to "plugged in",
            // or vice versa, we need to configure it.
            if (CFEqual(keys[index], CFSTR(kIOPSPowerSourceStateKey)) &&
                upsDataRef->isBatteryCase) {
                CFTypeRef oldValue = CFDictionaryGetValue(upsDataRef->upsStoreDict, keys[index]);
                if (oldValue && !CFEqual(oldValue, values[index])) {
                    BatteryCaseHandleACStateChange(upsDataRef, values[index]);
                }
            // Battery cases will indicate how much current we can draw from them
            } else if (CFEqual(keys[index], CFSTR(kIOPSAppleBatteryCaseAvailableCurrentKey)) &&
                       upsDataRef->isBatteryCase && !upsDataRef->hasACPower) {
                kern_return_t kr = BatteryCaseSetDeviceCurrentLimit(values[index]);
                if (kr != KERN_SUCCESS) {
                    syslog(LOG_ERR, "Unable to set device current limit (Error: %s)\n", mach_error_string(kr));
                }
            }
            
            CFDictionarySetValue(upsDataRef->upsStoreDict, keys[index],
                                 values[index]);
        }
        
        free (keys);
        free (values);
        
        IOReturn result = IOPSSetPowerSourceDetails(upsDataRef->powerSourceID,
                                                    upsDataRef->upsStoreDict);
        if (result != kIOReturnSuccess) {
            syslog(LOG_ERR, "updating power source details failed\n");
        }
        notify_post(kIOPSNotifyTimeRemaining);
    }
}


//---------------------------------------------------------------------------
// BatteryCaseSendCommand
//
// Sends a given command and corresponding value to the UPS plug-in interface
//---------------------------------------------------------------------------
kern_return_t BatteryCaseSendCommand(UPSDataRef upsDataRef, CFStringRef commandString, SInt32 value) {
#if TARGET_OS_EMBEDDED
    if (!upsDataRef || !(upsDataRef->upsPlugInInterface) || !(*upsDataRef->upsPlugInInterface)) {
        return KERN_INVALID_OBJECT;
    }
    
    // Wrap the command and value in the appropriate structures to be handed
    // to the plug-in interface
    CFTypeRef commandKeys[] = { commandString };
    CFTypeRef commandValues[] = { CFNumberCreate(kCFAllocatorDefault,
                                                 kCFNumberSInt32Type,
                                                 &value) };
    CFDictionaryRef command = CFDictionaryCreate(kCFAllocatorDefault,
                                                 commandKeys,
                                                 commandValues,
                                                 sizeof(commandKeys) / sizeof(CFTypeRef),
                                                 &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    
    return (*upsDataRef->upsPlugInInterface)->sendCommand(upsDataRef->upsPlugInInterface, command);
#else
    // NOOP on OS X
    return KERN_NOT_SUPPORTED;
#endif
}

//---------------------------------------------------------------------------
// BatteryCaseBatteryStateChangedCallback
//
// Called whenever the PMU charger updates. We filter for battery state
// changes, and tell battery cases what the internal state of charge is.
//---------------------------------------------------------------------------
void BatteryCaseBatteryStateChangedCallback(void *refcon, io_service_t service,
                                            uint32_t messageType, void *messageArgument) {
#if TARGET_OS_EMBEDDED
    UPSDataRef upsDataRef = (UPSDataRef) refcon;
    CFNumberRef currentCapacityRef;
    SInt32 currentCapacity;
    CFNumberRef maxCapacityRef;
    SInt32 maxCapacity;
    
    if (messageType == kIOPMMessageBatteryStatusHasChanged) {
        // Battery State has updated, get the current and max capacity (in mAh)
        currentCapacityRef = IORegistryEntrySearchCFProperty(service,
                                                              kIOServicePlane,
                                                              CFSTR("CurrentCapacity"),
                                                              kCFAllocatorDefault,
                                                              NULL);
        maxCapacityRef = IORegistryEntrySearchCFProperty(service,
                                                         kIOServicePlane,
                                                         CFSTR("MaxCapacity"),
                                                         kCFAllocatorDefault,
                                                         NULL);
        
        CFNumberGetValue(currentCapacityRef, kCFNumberSInt32Type, &currentCapacity);
        CFNumberGetValue(maxCapacityRef, kCFNumberSInt32Type, &maxCapacity);
        
        // If we read a negative or 0 MaxCapacity, something's wrong, don't
        // send the value.
        if (maxCapacity > 0) {
            // Send the command "Send Current State of Charge", as a %
            BatteryCaseSendCommand(upsDataRef,
                                   CFSTR(kIOPSCommandSendCurrentStateOfCharge),
                                   (currentCapacity * 100) / maxCapacity);
        }
    }
#endif
    // NOOP on OS X
}

//---------------------------------------------------------------------------
// BatteryCaseCurrentLimitChangeCallback
//
// Called when AC is present for the battery case, whenever the current limit
// from the power source attached to a battery case changes.
//---------------------------------------------------------------------------
void BatteryCaseCurrentLimitChangeCallback(void *refcon, io_service_t service,
                                           uint32_t messageType, void *messageArgument) {
#if TARGET_OS_EMBEDDED
    kern_return_t kr = KERN_FAILURE;
    UPSDataRef upsDataRef = (UPSDataRef) refcon;
    SInt32 newCurrentLimit = 0;
    
    if (messageType == kIOAccessoryManagerMessageUSBCurrentLimitChange) {
        // Message came from IOAccessoryManager that the power source's current
        // limit has changed. Get the new base current limit.
        kr = IOAccessoryManagerGetUSBCurrentLimitBase(service, &newCurrentLimit);
        if (kr != KERN_SUCCESS) {
            syslog(LOG_ERR, "unable to get new current limit\n");
            return;
        }
        
        // Send the command, {"Set Current Limit": newCurrentLimit}, to the battery
        // case so it knows how much to draw from the power source behind it.
        kr = BatteryCaseSendCommand(upsDataRef,
                                    CFSTR(kIOPSCommandSetCurrentLimitKey),
                                    newCurrentLimit);
        if (kr != KERN_SUCCESS) {
            syslog(LOG_ERR, "unable to send new current limit\n");
        }
    }
    
#endif
    // NOOP on OS X
}

//---------------------------------------------------------------------------
// BatteryCaseRequiredVoltageChangeCallback
//
// Called when AC is present for the battery case, whenever the PMU lowers the
// current limit from the power source attached to a battery case changes.
//---------------------------------------------------------------------------
void BatteryCaseRequiredVoltageChangeCallback(void *refcon, io_service_t service,
                                              uint32_t messageType, void *messageArgument) {
#if TARGET_OS_EMBEDDED
    UPSDataRef upsDataRef = (UPSDataRef) refcon;
    CFNumberRef reqVoltageRef;
    SInt32 reqVoltageValue;
    
    // Get the required voltage from the IORegistry
    reqVoltageRef = IORegistryEntrySearchCFProperty(service,
                                                    kIOServicePlane,
                                                    CFSTR("Voltage"),
                                                    kCFAllocatorDefault,
                                                    NULL);
    CFNumberGetValue(reqVoltageRef, kCFNumberSInt32Type, &reqVoltageValue);
    
    // Send the command, "Set Required Voltage", with the value read from the
    // registry + 150 mV of headroom.
    BatteryCaseSendCommand(upsDataRef,
                           CFSTR(kIOPSCommandSetRequiredVoltageKey),
                           reqVoltageValue + 150);
    
#endif
    // NOOP on OS X
}

//---------------------------------------------------------------------------
// BatteryCaseHandleACStateChange
//
// iOS Battery cases that act as UPS devices should pass through all
// information from devices attached to the case. Because of this, we're
// responsible for telling these cases the current limit of any downstream
// power sources.
//---------------------------------------------------------------------------
void BatteryCaseHandleACStateChange(UPSDataRef upsDataRef, CFTypeRef powerState) {
#if TARGET_OS_EMBEDDED
    Boolean acPresent = CFEqual(powerState, CFSTR(kIOPSACPowerValue));
    io_service_t accessoryManagerService;
    io_connect_t accessoryManagerConnect;
    io_service_t chargerService;
    kern_return_t kr;
    UInt32 currentLimit = 0;
    
    accessoryManagerService = IOAccessoryManagerGetServiceWithPrimaryPort(kIOAccessoryPortIDSerial);
    if (accessoryManagerService == IO_OBJECT_NULL) {
        syslog(LOG_ERR, "could not find IOAccessoryManager service for port %d\n", kIOAccessoryPortIDSerial);
        return;
    }
    
    kr = IOServiceOpen(accessoryManagerService, mach_task_self(), 0, &accessoryManagerConnect);
    if (kr != KERN_SUCCESS) {
        syslog(LOG_ERR, "could not open IOAccessoryManager service: %s\n", mach_error_string(kr));
        return;
    }
    
    if (acPresent) {
        // When attaching power, need to disable current inflow until downstream
        // current limits are negotiated with the battery case.
        IOPMAssertionID disabledInflowAssertionID = kIOPMNullAssertionID;
        IOPMAssertionCreateWithName(kIOPMInflowDisableAssertion,
                                    kIOPMAssertionLevelOn,
                                    CFSTR("com.apple.ioupsd.battery_case_disable_inflow"),
                                    &disabledInflowAssertionID);
        
        // Re-eneble USB and unmask charging
        upsDataRef->hasACPower = true;
        kr = IOAccessoryManagerSetUSBPowerIgnore(accessoryManagerConnect, false);
        kr = IOAccessoryManagerRestoreUSBCurrentLimitBase(accessoryManagerConnect);
        kr = IOAccessoryManagerSetUSBCurrentOffset(accessoryManagerConnect, 0);
        
        // Wait until IOAccessoryManager reports a non-zero iLim before sending
        // it to the battery case
        int retries = 20; // 20 * 250ms -> 5 second timeout
        while (currentLimit == 0 && retries-- > 0) {
            usleep(250000); // sleep for 250ms for now...
            kr = IOAccessoryManagerGetUSBCurrentLimitBase(accessoryManagerService, &currentLimit);
            syslog(LOG_ERR, "Battery Case AC Present: iLim = %d mA", currentLimit);
        }
        
        if (currentLimit != 0) {
            syslog(LOG_INFO, "Initial downstream charger iLim: %d mA\n", currentLimit);
        } else {
            syslog(LOG_ERR, "Timedout waiting for AC charger's current limit\n");
        }
        
        // Send the battery case the power source's current limit
        kr = BatteryCaseSendCommand(upsDataRef,
                                    CFSTR(kIOPSCommandSetCurrentLimitKey),
                                    currentLimit);
        
        // Once the iLim has been sent to the B244, we can let current inflow
        // resume at the new non-zero iLim
        IOPMAssertionRelease(disabledInflowAssertionID);
        disabledInflowAssertionID = kIOPMNullAssertionID;
        
        // Set up notifications to send the battery case new current limits
        IOObjectRetain(accessoryManagerService);
        kr = IOServiceAddInterestNotification(gNotifyPort,
                                              accessoryManagerService,
                                              kIOGeneralInterest,
                                              BatteryCaseCurrentLimitChangeCallback,
                                              upsDataRef,
                                              &(upsDataRef->currentLimitNotification));
        
        // Set up notifications to send the battery case required charging voltage
        chargerService = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                     IOServiceMatching("AppleARMPMUCharger"));
        kr = IOServiceAddInterestNotification(gNotifyPort,
                                              chargerService,
                                              kIOGeneralInterest,
                                              BatteryCaseRequiredVoltageChangeCallback,
                                              upsDataRef,
                                              &(upsDataRef->requiredVoltageNotification));

        // Schedule wake-up to ensure we complete enumeration and don't get stuck in sleep
        kr = scheduleRTCWake(chargerService, 15);

    } else {
        // Disable USB and mask charging from the rest of the system
        kr = IOAccessoryManagerSetUSBPowerIgnore(accessoryManagerConnect, true);
        upsDataRef->hasACPower = false;
        
        // Stop listening for changes in current limits, and revert to
        // default of drawing 1A from an untethered battery case
        if (upsDataRef->currentLimitNotification != MACH_PORT_NULL) {
            IOObjectRelease(upsDataRef->currentLimitNotification);
            upsDataRef->currentLimitNotification = MACH_PORT_NULL;
        }
        if (upsDataRef->requiredVoltageNotification != MACH_PORT_NULL) {
            IOObjectRelease(upsDataRef->requiredVoltageNotification);
            upsDataRef->requiredVoltageNotification = MACH_PORT_NULL;
        }
    }
    
    // Cleanup
    IOServiceClose(accessoryManagerConnect);
#endif
    // NOOP on OS X
}

int scheduleRTCWake(io_service_t chargerService, int deadlineSec)
{
    CFDictionaryRef  dict = NULL;
    CFStringRef parameterNames[1];
    CFTypeRef   parameterValues[1];
    kern_return_t result=KERN_ABORTED;
    parameterNames[0] = CFStringCreateWithCString( kCFAllocatorDefault, "RTCWakeImmediate", kCFStringEncodingUTF8 );
    parameterValues[0] = CFNumberCreate( kCFAllocatorDefault, kCFNumberSInt32Type, &deadlineSec );
    dict = CFDictionaryCreate( kCFAllocatorDefault,
                              (const void **) &parameterNames[0],
                              (const void **) &parameterValues[0],
                              1,
                              &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks );
    if (dict) {
        result = IORegistryEntrySetCFProperties(chargerService, dict);
        CFRelease(dict);
    }
    return result;
}

//---------------------------------------------------------------------------
// GetPrivateData
//
// Now that UPS entries remain in the System Configuration store, we also
// preserve the UPSDeviceData struct that is associated with it. Before
// getting a null entry from the gUPSDataRef that means we will have to
// create a new UPSDeviceData struct, we check the existing ones to see if
// there is a matching one that we can just reactivate. If we can't find an
// existing UPSDeviceData struct, we will create the storage that is
// necessary to keep track of the UPS.  We also update the global array of
// UPSDeviceData and fill in that data ref with the values that we want to
// track from the UPS
//---------------------------------------------------------------------------

UPSDataRef GetPrivateData(CFDictionaryRef properties) {
    UPSDataRef upsDataRef = NULL;
    CFMutableDataRef data = NULL;
    long   i = 0;
    long   count = 0;
    
    
    // Allocated the global array if necessary
    if (!gUPSDataArrayRef &&
        !(gUPSDataArrayRef = CFArrayCreateMutable(kCFAllocatorDefault, 0,
                                                  &kCFTypeArrayCallBacks))) {
        return NULL;
    }
    
    // Find an empty location in our array
    count = CFArrayGetCount(gUPSDataArrayRef);
    for (i = 0; i < count; i++) {
        data = (CFMutableDataRef)CFArrayGetValueAtIndex(gUPSDataArrayRef, i);
        if (!data)
            continue;
        
        upsDataRef = (UPSDataRef)CFDataGetMutableBytePtr(data);
        
        if (upsDataRef && !(upsDataRef->isPresent))
            break;
        
        upsDataRef = NULL;
    }
    
    // No valid upsDataRef was found, so let's go ahead and allocate one
    if ((upsDataRef == NULL) &&
        (data = CFDataCreateMutable(kCFAllocatorDefault,
                                    sizeof(UPSData)))) {
        upsDataRef = (UPSDataRef)CFDataGetMutableBytePtr(data);
        bzero(upsDataRef, sizeof(UPSData));
        
        CFArrayAppendValue(gUPSDataArrayRef, data);
        CFRelease(data);
    }
    
    // If we have a pointer to our global, then fill in some of the field in that structure
    //
    if (upsDataRef != NULL) {
        upsDataRef->upsID = (int)i;
    }
    
    return upsDataRef;
}


//---------------------------------------------------------------------------
// CreatePowerManagerUPSEntry
//
//---------------------------------------------------------------------------
#define kInternalUPSLabelLength 20

IOReturn CreatePowerManagerUPSEntry(UPSDataRef upsDataRef,
                                    CFDictionaryRef properties,
                                    CFSetRef capabilities) {
    CFMutableDictionaryRef upsStoreDict = NULL;
    CFStringRef upsName = NULL;
    CFStringRef transport = NULL;
    CFNumberRef vid = NULL;
    CFNumberRef pid = NULL;
    CFNumberRef number = NULL;
    IOReturn result = kIOReturnSuccess;
    int elementValue = 0;
    uint32_t psID = 0;
    char upsLabelString[kInternalUPSLabelLength];
    
    if (!upsDataRef || !properties || !capabilities)
        return kIOReturnError;
    
    upsStoreDict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
    
    // Set some Store values
    if (upsStoreDict) {
        // We need to save a name for this device.  First, try to see if we have
        // a USB Product Name.  If that fails then use the manufacturer and if
        // that fails, then use a generic name.  Couldn't we use a serial # here?
        //
        upsName = (CFStringRef) CFDictionaryGetValue(properties,
                                                     CFSTR(kIOPSNameKey));
        if (!upsName)
            upsName = CFSTR(kDefaultUPSName);
        
        transport = (CFStringRef) CFDictionaryGetValue(properties,
                                                       CFSTR(kIOPSTransportTypeKey));
        if (!transport)
            transport = CFSTR(kDefaultTransport);
        
        vid = (CFNumberRef) CFDictionaryGetValue(properties,
                                                 CFSTR(kIOPSVendorIDKey));
        
        
        pid = (CFNumberRef) CFDictionaryGetValue(properties,
                                                 CFSTR(kIOPSProductIDKey));
        
        CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSNameKey), upsName);
        CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSTransportTypeKey), transport);
        CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSIsPresentKey), kCFBooleanTrue);
        CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSIsChargingKey), kCFBooleanFalse);
        CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSPowerSourceStateKey), CFSTR(kIOPSBatteryPowerValue));
        if (vid) {
            CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSVendorIDKey), vid);
        }
        if (pid) {
            CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSProductIDKey), pid);
        }
        
        psID = MAKE_UNIQ_SOURCE_ID(getpid(), upsDataRef->upsID);
        number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                &psID);
        CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSPowerSourceIDKey), number);
        CFRelease(number);
        
        // rdar://problem/21817316 Initialize battery cases to a max capacity
        // of 0 so they don't show up in UI until we've received the correct
        // value from the device.
        if (upsDataRef->isBatteryCase) {
            elementValue = 0;
        } else {
            elementValue = 100;
        }
        number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                &elementValue);
        CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSMaxCapacityKey), number);
        CFRelease(number);
        
        if (CFSetContainsValue(capabilities, CFSTR(kIOPSCurrentCapacityKey))) {
            //  Initialize kIOPSCurrentCapacityKey
            //
            //  For Power Manager, we will be sharing capacity with Power Book
            //  battery capacities, so we want a consistent measure. For now we
            // have settled on percentage of full capacity.
            //
            elementValue = 100;
            number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                    &elementValue);
            CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSCurrentCapacityKey),
                                 number);
            CFRelease(number);
        }
        
        if (CFSetContainsValue(capabilities, CFSTR(kIOPSTimeToEmptyKey))) {
            // Initialize kIOPSTimeToEmptyKey
            // (OS 9 PowerClass.c assumed 100 milliwatt-hours)
            //
            elementValue = 100;
            number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                    &elementValue);
            CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSTimeToEmptyKey),
                                 number);
            CFRelease(number);
        }
        
        if (CFSetContainsValue(capabilities, CFSTR(kIOPSVoltageKey))) {
            // Initialize kIOPSVoltageKey (OS 9 PowerClass.c assumed millivolts.
            // (Shouldn't that be 130,000 millivolts for AC?))
            // Actually, Power Devices Usage Tables say units will be in Volts.
            // However we have to check what exponent is used because that may
            // make the value we get in centiVolts (exp = -2). So it looks like
            // OS 9 sources said millivolts, but used centivolts. Our final
            // answer should device by proper exponent to get back to Volts.
            //
            elementValue = 13 * 1000 / 100;
            number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                    &elementValue);
            CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSVoltageKey), number);
            CFRelease(number);
        }
        
        if (CFSetContainsValue(capabilities, CFSTR(kIOPSCurrentKey))) {
            // Initialize kIOPSCurrentKey (What would be a good amperage to
            // initialize to?) Same discussion as for Volts, where the unit
            // for current is Amps. But with typical exponents (-2), we get
            // centiAmps. Hmm... typical current for USB may be 500 milliAmps,
            // which would be .5 A. Since that is not an integer, that may be
            // why our displays get larger numbers
            //
            elementValue = 1;	// Just a guess!
            number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType,
                                    &elementValue);
            CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSCurrentKey), number);
            CFRelease(number);
        }
        
        if (upsDataRef->isAccessoryBattery) {
            // This is an accessory battery
            CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSTypeKey), CFSTR(kIOPSAccessoryType));
        }
#if TARGET_OS_EMBEDDED
        else if (upsDataRef->isBatteryCase) {
            // Override the type value of battery cases to be "Battery Case" so
            // clients of IOPowerSources can handle the state where the internal
            // battery IS charging, but we don't truly have AC power
            CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSTypeKey), CFSTR(kIOPSPrivateBatteryCaseType));
        }
#endif
        else
            CFDictionarySetValue(upsStoreDict, CFSTR(kIOPSTypeKey), CFSTR(kIOPSUPSType));
    }
    
    // Uniquely name each Sys Config key
    //
    snprintf(upsLabelString, kInternalUPSLabelLength, "/UPS%d",
             upsDataRef->upsID);
    
    result = IOPSCreatePowerSource(&(upsDataRef->powerSourceID));
    gUPSCount++;
    
    // TODO: possible trouble spot.
    //       Shouldn't need to check/release since it only exists if we succeed.
    if (result != kIOReturnSuccess) {
        upsDataRef->powerSourceID = NULL;
        return result;
    }
    
    result = IOPSSetPowerSourceDetails(upsDataRef->powerSourceID, upsStoreDict);
    
    if (result == kIOReturnSuccess) {
        // Store our SystemConfiguration variables in our private data
        //
        upsDataRef->upsStoreDict = upsStoreDict;
        
    } else if (upsStoreDict) {
        CFRelease(upsStoreDict);
    }
    
    return result;
}


//===========================================================================
// MIG Routines
//===========================================================================

//---------------------------------------------------------------------------
// _io_ups_send_command
//
// This routine allow remote processes to issue commands to the UPS.  It is
// expected that command will come in as a serialized CFDictionaryRef.
//---------------------------------------------------------------------------
kern_return_t _io_ups_send_command(mach_port_t server, int srcId,
                                   void *commandBuffer, IOByteCount commandSize) {
    CFDictionaryRef	command;
    CFMutableDataRef data;
    UPSDataRef upsDataRef;
    IOReturn res = kIOReturnError;
    
    command = (CFDictionaryRef)IOCFUnserialize(commandBuffer,
                                               kCFAllocatorDefault,
                                               kNilOptions, NULL);
    if (command) {
        if (!gUPSDataArrayRef || (GET_UPSID(srcId) >= CFArrayGetCount(gUPSDataArrayRef))) {
            res = kIOReturnBadArgument;
        } else {
            data = (CFMutableDataRef)CFArrayGetValueAtIndex(gUPSDataArrayRef,
                                                            GET_UPSID(srcId));
            upsDataRef =(UPSDataRef)CFDataGetMutableBytePtr(data);
            
            if (upsDataRef && upsDataRef->upsPlugInInterface)
                res = (*upsDataRef->upsPlugInInterface)->sendCommand(upsDataRef->upsPlugInInterface, command);
        }
        CFRelease(command);
    }
    
    return res;
}

//---------------------------------------------------------------------------
// _io_ups_get_event
//
// This routine allow remote processes to issue commands to the UPS.  It will
// return a CFDictionaryRef that is serialized.
//---------------------------------------------------------------------------
kern_return_t _io_ups_get_event( mach_port_t server, int srcId,
                                void **eventBufferPtr,
                                IOByteCount *eventBufferSizePtr) {
    CFDictionaryRef	event;
    CFMutableDataRef data;
    CFDataRef serializedData;
    UPSDataRef upsDataRef;
    IOReturn res = kIOReturnError;
    
    if (!eventBufferPtr || !eventBufferSizePtr ||
        !gUPSDataArrayRef || (GET_UPSID(srcId) >= CFArrayGetCount(gUPSDataArrayRef))) {
        
        return kIOReturnBadArgument;
    }
    
    data = (CFMutableDataRef)CFArrayGetValueAtIndex(gUPSDataArrayRef, GET_UPSID(srcId));
    upsDataRef = (UPSDataRef)CFDataGetMutableBytePtr(data);
    
    if (!upsDataRef || !upsDataRef->upsPlugInInterface)
        return kIOReturnBadArgument;
    
    res = (*upsDataRef->upsPlugInInterface)->getEvent(upsDataRef->upsPlugInInterface, &event);
    
    if ((res != kIOReturnSuccess) || !event)
        return kIOReturnError;
    
    
    serializedData = (CFDataRef)IOCFSerialize(event, kNilOptions);
    
    if (!serializedData)
        return kIOReturnError;
    
    *eventBufferSizePtr = CFDataGetLength(serializedData);
    
    vm_allocate(mach_task_self(), (vm_address_t *)eventBufferPtr,
                *eventBufferSizePtr, TRUE);
    
    if(*eventBufferPtr)
        memcpy(*eventBufferPtr, CFDataGetBytePtr(serializedData),
               *eventBufferSizePtr);
    
    CFRelease(serializedData);
    
    return res;
}

//---------------------------------------------------------------------------
// _io_ups_get_capabilities
//
// This routine allow remote processes to issue commands to the UPS.  It will
// return a CFSetRef that is serialized.
//---------------------------------------------------------------------------
kern_return_t _io_ups_get_capabilities(mach_port_t server, int srcId,
                                       void **capabilitiesBufferPtr,
                                       IOByteCount *capabilitiesBufferSizePtr) {
    CFSetRef capabilities;
    CFMutableDataRef data;
    CFDataRef serializedData;
    UPSDataRef upsDataRef;
    IOReturn res = kIOReturnError;
    
    if (!capabilitiesBufferPtr || !capabilitiesBufferSizePtr ||
        !gUPSDataArrayRef || (GET_UPSID(srcId) >= CFArrayGetCount(gUPSDataArrayRef))) {
        
        return kIOReturnBadArgument;
    }
    
    data = (CFMutableDataRef)CFArrayGetValueAtIndex(gUPSDataArrayRef, GET_UPSID(srcId));
    upsDataRef = (UPSDataRef)CFDataGetMutableBytePtr(data);
    
    if (!upsDataRef || !upsDataRef->upsPlugInInterface)
        return kIOReturnBadArgument;
    
    res = (*upsDataRef->upsPlugInInterface)->getCapabilities(upsDataRef->upsPlugInInterface,
                                                             &capabilities);
    
    if ((res != kIOReturnSuccess) || !capabilities)
        return kIOReturnError;
    
    
    serializedData = (CFDataRef)IOCFSerialize(capabilities, kNilOptions);
    
    if (!serializedData)
        return kIOReturnError;
    
    *capabilitiesBufferSizePtr = CFDataGetLength(serializedData);
    
    vm_allocate(mach_task_self(), (vm_address_t *)capabilitiesBufferPtr,
                *capabilitiesBufferSizePtr, TRUE);
    
    if (*capabilitiesBufferPtr)
        memcpy(*capabilitiesBufferPtr, CFDataGetBytePtr(serializedData),
               *capabilitiesBufferSizePtr);
    
    CFRelease(serializedData);
    
    return res;
}

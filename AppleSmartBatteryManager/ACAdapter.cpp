//
//  ACAdapter.cpp
//  ACPIBatteryManager
//
//  Created by RehabMan on 11/16/13.
//
//  Code/ideas liberally borrowed from VoodooBattery...
//

#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <IOKit/IOCommandGate.h>
#include "IOPMPrivate.h"
#include "ACAdapter.h"

static IOPMPowerState myTwoStates[2] = {
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

#define super IOService

OSDefineMetaClassAndStructors(ACPIACAdapter, IOService)

bool ACPIACAdapter::init(OSDictionary* dict)
{
    if (!IOService::init(dict))
        return false;

    fProvider = NULL;
    fACConnected = false;
    
    return true;
}

bool ACPIACAdapter::start(IOService* provider)
{
    if (!super::start(provider))
    {
        AlwaysLog("ACPIACAdapter: IOService::start failed!\n");
        return false;
    }
    
    fProvider = OSDynamicCast(IOACPIPlatformDevice, provider);
    if (!fProvider)
    {
        AlwaysLog("ACPIACAdapter: provider not IOACPIPlatformDevice\n");
        return false;
    }
    fProvider->retain();
    
    IOWorkLoop *fWorkloop = getWorkLoop();
    if (!fWorkloop) {
        return false;
    }

    // Command gate for ACPIACAdapter
    fCommandGate = IOCommandGate::commandGate(this);
    if (!fCommandGate) {
        return false;
    }
    fWorkloop->addEventSource(fCommandGate);
    
    fLock = IORecursiveLockAlloc();
    fBatteryServices = OSSet::withCapacity(1);

    OSDictionary * serviceMatch = serviceMatching("AppleSmartBattery");

    IOServiceMatchingNotificationHandler notificationHandler = OSMemberFunctionCast(IOServiceMatchingNotificationHandler, this, &ACPIACAdapter::notificationHandler);
    
    //
    // Register notifications for availability of any AppleSmartBattery objects to notify of AC connection state changes
    //
    fPublishNotify = addMatchingNotification(gIOFirstPublishNotification,
                                             serviceMatch,
                                             notificationHandler,
                                             this,
                                             0, 10000);
    
    fTerminateNotify = addMatchingNotification(gIOTerminatedNotification,
                                               serviceMatch,
                                               notificationHandler,
                                               this,
                                               0, 10000);
    
    serviceMatch->release();
    
    // Join power management so that we can get a notification early during
    // wakeup to re-sample our battery data. We don't actually power manage
    // any devices.
    PMinit();
    registerPowerDriver(this, myTwoStates, 2);
    provider->joinPMtree(this);
    
	DebugLog("starting ACPIACAdapter.\n");

    return true;
}

void ACPIACAdapter::stop(IOService* provider)
{
    // Free device matching notifiers
    fPublishNotify->remove();
    fTerminateNotify->remove();
    OSSafeReleaseNULL(fPublishNotify);
    OSSafeReleaseNULL(fTerminateNotify);

    fBatteryServices->flushCollection();
    OSSafeReleaseNULL(fBatteryServices);
    
    fWorkloop->removeEventSource(fCommandGate);
    OSSafeReleaseNULL(fCommandGate);
    
    if (NULL != fLock)
    {
        IORecursiveLockFree(fLock);
        fLock = NULL;
    }

    OSSafeReleaseNULL(fProvider);
    
    IOService::stop(provider);
}

IOReturn ACPIACAdapter::setPowerState(unsigned long state, IOService* device)
{
    DebugLog("ACPIACAdapter::setPowerState: state: %u, device: %s\n", (unsigned int) state, device->getName());

    // System wake-up
    if (state)
        pollState();

    return kIOPMAckImplied;
}

IOReturn ACPIACAdapter::message(UInt32 type, IOService* provider, void* argument)
{
    DebugLog("ACPIACAdapter::message: type: %08X provider: %s\n", (unsigned int)type, provider->getName());

    if (type == kIOACPIMessageDeviceNotification)
        pollState();

    return kIOReturnSuccess;
}

void ACPIACAdapter::gatedHandler(IOService* newService, IONotifier * notifier)
{
    AppleSmartBattery*  battery = OSDynamicCast(AppleSmartBattery, newService);
    
    if (battery != NULL) {
        if (notifier == fPublishNotify) {
            DebugLog("%s: Notification consumer published: %s\n", getName(), battery->getName());
            
            fBatteryServices->setObject(battery);
            
            // Ensure the newly registered battery is updated with the latest AC connection state
            pollState();
        }
        
        if (notifier == fTerminateNotify) {
            DebugLog("%s: Notification consumer terminated: %s\n", getName(), battery->getName());
            fBatteryServices->removeObject(battery);
        }
    }
}

bool ACPIACAdapter::notificationHandler(void * refCon, IOService * newService, IONotifier * notifier)
{
    if (!fCommandGate)
        return false;

    fCommandGate->runAction((IOCommandGate::Action)OSMemberFunctionCast(IOCommandGate::Action, this, &ACPIACAdapter::gatedHandler), newService, notifier, NULL, NULL);
    return true;
}

void ACPIACAdapter::pollState()
{
    UInt32 acpi = 0;
    
    IORecursiveLockLock(fLock);
    
    if (fProvider && kIOReturnSuccess == fProvider->evaluateInteger("_PSR", &acpi))
    {
        DebugLog("ACPIACAdapter::message setting AC %s\n", (acpi ? "connected" : "disconnected"));
        
        fACConnected = acpi;
        
        // notify battery managers of change in AC state first
        OSCollectionIterator* i = OSCollectionIterator::withCollection(fBatteryServices);
        
        if (i != NULL) {
            while (AppleSmartBattery* battery = OSDynamicCast(AppleSmartBattery, i->getNextObject()))  {
                //fCommandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, battery, &AppleSmartBattery::notifyConnectedState), (void*)fACConnected);
                battery->notifyConnectedState(fACConnected);
            }
        }

        i->release();
        
        // notify system of change in AC state
        if (IOPMrootDomain* root = getPMRootDomain())
            root->receivePowerNotification((kIOPMSetACAdaptorConnected | (fACConnected ? kIOPMSetValue : 0)));
        else
            DebugLog("ACPIACAdapter::message could not notify OS about AC status\n");
    }
    else
    {
        AlwaysLog("ACPIACAdapter: ACPI method _PSR failed\n");
    }

    IORecursiveLockUnlock(fLock);
}


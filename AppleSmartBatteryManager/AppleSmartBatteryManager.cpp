/*
 * Copyright (c) 2005 Apple Computer, Inc. All rights reserved.
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

#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/version.h>

#include "AppleSmartBatteryManager.h"
#include "AppleSmartBattery.h"

//REVIEW: avoids problem with Xcode 5.1.0 where -dead_strip eliminates these required symbols
#include <libkern/OSKextLib.h>
void* _org_rehabman_dontstrip_[] =
{
    (void*)&OSKextGetCurrentIdentifier,
    (void*)&OSKextGetCurrentLoadTag,
    (void*)&OSKextGetCurrentVersionString,
};

enum {
    kMyOnPowerState = 1
};

enum {
    kInhibitChargingCmd = 0,
    kDisableInflowCmd
};

static IOPMPowerState myTwoStates[2] = {
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

OSDefineMetaClassAndStructors(AppleSmartBatteryManager, IOService)

#ifdef DEBUG

/******************************************************************************
 * AppleSmartBatteryManager::init
 *
 ******************************************************************************/

bool AppleSmartBatteryManager::init(OSDictionary *dict)
{
    bool result = super::init(dict);
    DebugLog("AppleSmartBatteryManager::init: Initializing\n");

    return result;
}

/******************************************************************************
 * AppleSmartBatteryManager::free
 *
 ******************************************************************************/

void AppleSmartBatteryManager::free(void)
{
    DebugLog("AppleSmartBatteryManager::free: Freeing\n");
    super::free();
}

/******************************************************************************
 * AppleSmartBatteryManager::probe
 *
 ******************************************************************************/

IOService *AppleSmartBatteryManager::probe(IOService *provider,
                                                SInt32 *score)
{
    IOService *result = super::probe(provider, score);
    DebugLog("AppleSmartBatteryManager::probe: Probing\n");
    return result;
}

#endif // DEBUG



/******************************************************************************
 * AppleSmartBatteryManager::start
 *
 ******************************************************************************/

bool AppleSmartBatteryManager::start(IOService *provider)
{
    DebugLog("AppleSmartBatteryManager::start: called\n");
    
    fProvider = OSDynamicCast(IOACPIPlatformDevice, provider);

    if (!fProvider || !super::start(provider)) {
        return false;
    }

    IOWorkLoop *wl = getWorkLoop();
    if (!wl) {
        return false;
    }
    
    fBatteryServices = OSSet::withCapacity(1);
    
    OSDictionary * serviceMatch = serviceMatching("AppleSmartBattery");
    
    IOServiceMatchingNotificationHandler notificationHandler = OSMemberFunctionCast(
            IOServiceMatchingNotificationHandler, this, &AppleSmartBatteryManager::notificationHandler);
    
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

    // maybe waiting for SMC to load avoids startup problems on 10.13.x
    if (RunningKernel() >= MakeKernelVersion(17,0,0)) {
        waitForService(serviceMatching("IODisplayWrangler"));
        //waitForService(serviceMatching("AppleSMC"));
        //IOSleep(15000);
    }

    // Join power management so that we can get a notification early during
    // wakeup to re-sample our battery data. We don't actually power manage
    // any devices.
    
    PMinit();
    registerPowerDriver(this, myTwoStates, 2);
    provider->joinPMtree(this);

    // announce version
    extern kmod_info_t kmod_info;
    AlwaysLog("Version %s starting on OS X Darwin %d.%d.\n", kmod_info.version, version_major, version_minor);

    // place version/build info in ioreg properties RM,Build and RM,Version
    char buf[128];
    snprintf(buf, sizeof(buf), "%s %s", kmod_info.name, kmod_info.version);
    setProperty("RM,Version", buf);
#ifdef DEBUG
    setProperty("RM,Build", "Debug-" LOGNAME);
#else
    setProperty("RM,Build", "Release-" LOGNAME);
#endif

#if 0
    //REVIEW_REHABMAN: I don't think this makes any sense...
    int value = getPlatform()->numBatteriesSupported();
    DebugLog("Battery Supported Count(s) %d.\n", value);

    // TODO: Create battery array to hold battery objects if more than one battery in the system
    if (value > 1)
    {
        if (kIOReturnSuccess == fProvider->evaluateInteger("_STA", &fBatterySTA)) {
            if (fBatterySTA & BATTERY_PRESENT) {
                goto populateBattery;
            } else {
                goto skipBattery;
            }
        }
    }
#endif

populateBattery:

    fBattery = AppleSmartBattery::smartBattery();

    if(!fBattery)
        return false;

    // Command gate for ACPIBattery
    fBatteryGate = IOCommandGate::commandGate(fBattery);
    if (!fBatteryGate) {
        return false;
    }
    wl->addEventSource(fBatteryGate);

    fBattery->attach(this);
    fBattery->start(this);

skipBattery:

    this->setName("AppleSmartBatteryManager");
    this->registerService(0);

    return true;
}

/******************************************************************************
 * AppleSmartBatteryManager::stop
 ******************************************************************************/

void AppleSmartBatteryManager::stop(IOService *provider)
{
    DebugLog("AppleSmartBatteryManager::stop: called\n");

    fBattery->detach(this);
    
    // Free device matching notifiers
    fPublishNotify->remove();
    fTerminateNotify->remove();
    OSSafeReleaseNULL(fPublishNotify);
    OSSafeReleaseNULL(fTerminateNotify);
    
    fBatteryServices->flushCollection();
    OSSafeReleaseNULL(fBatteryServices);
    
    fBattery->free();
    fBattery->stop(this);
    fBattery->terminate();
    fBattery = NULL;
    
    IOWorkLoop *wl = getWorkLoop();
    if (wl) {
        wl->removeEventSource(fBatteryGate);
    }

    fBatteryGate->free();
    fBatteryGate = NULL;
    
    PMstop();
    
    super::stop(provider);
}

/******************************************************************************
 * AppleSmartBattery::gatedHandler
 *
 ******************************************************************************/

void AppleSmartBatteryManager::gatedHandler(IOService* newService, IONotifier * notifier)
{
    AppleSmartBattery*  battery = OSDynamicCast(AppleSmartBattery, newService);
    
    if (battery != NULL) {
        if (notifier == fPublishNotify) {
            DebugLog("%s: Notification consumer published: %s\n", getName(), battery->getName());
            fBatteryServices->setObject(battery);
        }

        if (notifier == fTerminateNotify) {
            DebugLog("%s: Notification consumer terminated: %s\n", getName(), battery->getName());
            fBatteryServices->removeObject(battery);
        }
    }
}

/******************************************************************************
 * AppleSmartBattery::notificationHandler
 *
 ******************************************************************************/

bool AppleSmartBatteryManager::notificationHandler(void * refCon, IOService * newService, IONotifier * notifier)
{
    if (!fBatteryGate)
        return false;

    fBatteryGate->runAction((IOCommandGate::Action)OSMemberFunctionCast(IOCommandGate::Action, this, &AppleSmartBatteryManager::gatedHandler), newService, notifier, NULL, NULL);
    return true;
}

/******************************************************************************
 * AppleSmartBattery::areBatteriesDischarging
 *
 ******************************************************************************/

bool AppleSmartBatteryManager::areBatteriesDischarging(AppleSmartBattery * except)
{
    // Query if any batteries are discharging
    OSCollectionIterator* i = OSCollectionIterator::withCollection(fBatteryServices);
    
    if (i != NULL) {
        while (AppleSmartBattery* battery = OSDynamicCast(AppleSmartBattery, i->getNextObject()))  {
            if (battery->fBatteryPresent && !battery->fACConnected) {
                return true;
                break;
            }
        }
    }

    i->release();

    return false;
}


/******************************************************************************
 * AppleSmartBatteryManager::setPollingInterval
 *
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::setPollingInterval(int milliSeconds)
{
    DebugLog("setPollingInterval: interval = %d ms\n", milliSeconds);
    
    // Discard any negatize or zero arguments
    if (milliSeconds <= 0) return kIOReturnBadArgument;

    if (fBattery)
        fBattery->setPollingInterval(milliSeconds);

    setProperty("PollingInterval_msec", milliSeconds, 32);

    return kIOReturnSuccess;
}

/******************************************************************************
 * AppleSmartBatteryManager::setPowerState
 *
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::setPowerState(unsigned long which, IOService *whom)
{
    IOReturn ret = IOPMAckImplied;
    
    DebugLog("AppleSmartBatteryManager::setPowerState: which = 0x%lx\n", which);
    
    if(fBatteryGate)
    {
        // We are waking from sleep - kick off a battery read to make sure
        // our battery concept is in line with reality.
        ret = fBatteryGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, fBattery, &AppleSmartBattery::handleSystemSleepWake), (void*)this, (void*)!which);
    }

    return ret;
}

/******************************************************************************
 * AppleSmartBatteryManager::message
 *
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::message(UInt32 type, IOService *provider, void *argument)
{
    UInt32 batterySTA;

    if( (kIOACPIMessageDeviceNotification == type)
        && (kIOReturnSuccess == fProvider->evaluateInteger("_STA", &batterySTA))
        && fBatteryGate )
    {
        if (batterySTA ^ fBatterySTA)
        {
            if (batterySTA & BATTERY_PRESENT)
            {
                // Battery inserted
                DebugLog("battery inserted\n");
                fBatteryGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, fBattery, &AppleSmartBattery::handleBatteryInserted));
            }
            else
            {
                // Battery removed
                DebugLog("battery removed\n");
                fBatteryGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, fBattery, &AppleSmartBattery::handleBatteryRemoved));
            }
        }
        else
        {
            // Just an alarm; re-read battery state.
            DebugLog("polling battery state\n");
            fBatteryGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, fBattery, &AppleSmartBattery::pollBatteryState), (void*)kExistingBatteryPath);
        }
    }

    return kIOReturnSuccess;
}

/******************************************************************************
 * AppleSmartBatteryManager::validateBatteryBIX
 * Verify that DSDT _BIX method exists
 ******************************************************************************/
IOReturn AppleSmartBatteryManager::validateBatteryBIX(void)
{
    return fProvider->validateObject("_BIX");
}

/******************************************************************************
 * AppleSmartBatteryManager::validateBatteryBBIX
 * Verify that DSDT _BBIX method exists
 ******************************************************************************/
IOReturn AppleSmartBatteryManager::validateBatteryBBIX(void)
{
    return fProvider->validateObject("_BBIX");
}

/******************************************************************************
 * AppleSmartBatteryManager::getBatterySTA
 * Call DSDT _STA method to return battery device status
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::getBatterySTA(void)
{
    DebugLog("getBatterySTA called\n");
    
    IOReturn evaluateStatus = fProvider->evaluateInteger("_STA", &fBatterySTA);
    if (evaluateStatus == kIOReturnSuccess)
    {
        return fBattery->setBatterySTA(fBatterySTA);
    }
    else
    {
        DebugLog("evaluateObject error 0x%x\n", evaluateStatus);
        return kIOReturnError;
    }
}

/******************************************************************************
 * AppleSmartBatteryManager::getBatteryBIF
 * Call DSDT _BIF method to return ACPI 3.x battery info
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::getBatteryBIF(void)
{
    DebugLog("getBatteryBIF called\n");
    
    OSObject *fBatteryBIF = NULL;
    IOReturn evaluateStatus = fProvider->validateObject("_BIF");
    DebugLog("validateObject return 0x%x\n", evaluateStatus);
    
    evaluateStatus = fProvider->evaluateObject("_BIF", &fBatteryBIF);
    if (evaluateStatus == kIOReturnSuccess)
    {
        IOReturn value = kIOReturnError;
        if (OSArray* acpibat_bif = OSDynamicCast(OSArray, fBatteryBIF))
        {
            setProperty("Battery Information", acpibat_bif);
            value = fBattery->setBatteryBIF(acpibat_bif);
        }
        OSSafeReleaseNULL(fBatteryBIF);
        return value;
    }
    else
    {
        DebugLog("evaluateObject error 0x%x\n", evaluateStatus);
        return kIOReturnError;
    }
}

/******************************************************************************
 * AppleSmartBatteryManager::getBatteryBIX
 * Call DSDT _BIX method to return ACPI 4.x battery info
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::getBatteryBIX(void)
{
    DebugLog("getBatteryBIX called\n");
    
    OSObject *fBatteryBIX = NULL;
    IOReturn evaluateStatus = fProvider->evaluateObject("_BIX", &fBatteryBIX);
    if (evaluateStatus == kIOReturnSuccess)
    {
        IOReturn value = kIOReturnError;
        if (OSArray* acpibat_bix = OSDynamicCast(OSArray, fBatteryBIX))
        {
            setProperty("Battery Extended Information", acpibat_bix);
            value = fBattery->setBatteryBIX(acpibat_bix);
        }
        OSSafeReleaseNULL(fBatteryBIX);
        return value;
    }
    else
    {
        DebugLog("evaluateObject error 0x%x\n", evaluateStatus);
        return kIOReturnError;
    }
}

/******************************************************************************
 * AppleSmartBatteryManager::getBatteryBBIX
 * Call DSDT BBIX method to return all battery info (non-standard)
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::getBatteryBBIX(void)
{
    DebugLog("getBatteryBBIX called\n");

    OSObject * fBatteryBBIX = NULL;
    IOReturn evaluateStatus = fProvider->evaluateObject("BBIX", &fBatteryBBIX);
    if (evaluateStatus == kIOReturnSuccess)
    {
        IOReturn value = kIOReturnError;
        if (OSArray* acpibat_bbix = OSDynamicCast(OSArray, fBatteryBBIX))
        {
            setProperty("Battery Extra Information", acpibat_bbix);
            value = fBattery->setBatteryBBIX(acpibat_bbix);
        }
        OSSafeReleaseNULL(fBatteryBBIX);
        return value;
    }
    else
    {
        DebugLog("evaluateObject error 0x%x\n", evaluateStatus);
        return kIOReturnError;
    }
}

/******************************************************************************
 * AppleSmartBatteryManager::getBatteryBST
 * Call DSDT _BST method to return the battery state
 ******************************************************************************/

IOReturn AppleSmartBatteryManager::getBatteryBST(void)
{
    DebugLog("getBatteryBST called\n");

    OSObject *fBatteryBST = NULL;
    IOReturn evaluateStatus = fProvider->evaluateObject("_BST", &fBatteryBST);
    if (evaluateStatus == kIOReturnSuccess)
    {
        IOReturn value = kIOReturnError;
        if (OSArray* acpibat_bst = OSDynamicCast(OSArray, fBatteryBST))
        {
            setProperty("Battery Status", acpibat_bst);
            value = fBattery->setBatteryBST(acpibat_bst);
        }
        OSSafeReleaseNULL(fBatteryBST);
        return value;
    }
    else
    {
        DebugLog("evaluateObject error 0x%x\n", evaluateStatus);
        return kIOReturnError;
    }
}

/*****************************************************************************
 * ACPI-based configuration override
 ******************************************************************************/

OSObject* AppleSmartBatteryManager::translateEntry(OSObject* obj)
{
    // Note: non-NULL result is retained...

    // if object is another array, translate it
    if (OSArray* array = OSDynamicCast(OSArray, obj))
        return translateArray(array);

    // if object is a string, may be translated to boolean
    if (OSString* string = OSDynamicCast(OSString, obj))
    {
        // object is string, translate special boolean values
        const char* sz = string->getCStringNoCopy();
        if (sz[0] == '>')
        {
            // boolean types true/false
            if (sz[1] == 'y' && !sz[2])
                return OSBoolean::withBoolean(true);
            else if (sz[1] == 'n' && !sz[2])
                return OSBoolean::withBoolean(false);
            // escape case ('>>n' '>>y'), replace with just string '>n' '>y'
            else if (sz[1] == '>' && (sz[2] == 'y' || sz[2] == 'n') && !sz[3])
                return OSString::withCString(&sz[1]);
        }
    }
    return NULL; // no translation
}

OSObject* AppleSmartBatteryManager::translateArray(OSArray* array)
{
    // may return either OSArray* or OSDictionary*

    int count = array->getCount();
    if (!count)
        return NULL;

    OSObject* result = array;

    // if first entry is an empty array, process as array, else dictionary
    OSArray* test = OSDynamicCast(OSArray, array->getObject(0));
    if (test && test->getCount() == 0)
    {
        // using same array, but translating it...
        array->retain();

        // remove bogus first entry
        array->removeObject(0);
        --count;

        // translate entries in the array
        for (int i = 0; i < count; ++i)
        {
            if (OSObject* obj = translateEntry(array->getObject(i)))
            {
                array->replaceObject(i, obj);
                obj->release();
            }
        }
    }
    else
    {
        // array is key/value pairs, so must be even
        if (count & 1)
            return NULL;

        // dictionary constructed to accomodate all pairs
        int size = count >> 1;
        if (!size) size = 1;
        OSDictionary* dict = OSDictionary::withCapacity(size);
        if (!dict)
            return NULL;

        // go through each entry two at a time, building the dictionary
        for (int i = 0; i < count; i += 2)
        {
            OSString* key = OSDynamicCast(OSString, array->getObject(i));
            if (!key)
            {
                dict->release();
                return NULL;
            }
            // get value, use translated value if translated
            OSObject* obj = array->getObject(i+1);
            OSObject* trans = translateEntry(obj);
            if (trans)
                obj = trans;
            dict->setObject(key, obj);
            OSSafeReleaseNULL(trans);
        }
        result = dict;
    }

    // Note: result is retained when returned...
    return result;
}

OSDictionary* AppleSmartBatteryManager::getConfigurationOverride(const char* method)
{
    // attempt to get configuration data from provider
    OSObject* r = NULL;
    if (kIOReturnSuccess != (fProvider->evaluateObject(method, &r)))
        return NULL;

    // for translation method must return array
    OSObject* obj = NULL;
    OSArray* array = OSDynamicCast(OSArray, r);
    if (array)
        obj = translateArray(array);
    OSSafeReleaseNULL(r);

    // must be dictionary after translation, even though array is possible
    OSDictionary* result = OSDynamicCast(OSDictionary, obj);
    if (!result)
    {
        OSSafeReleaseNULL(obj);
        return NULL;
    }
    return result;
}


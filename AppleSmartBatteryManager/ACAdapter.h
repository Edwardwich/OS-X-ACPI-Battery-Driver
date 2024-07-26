//
//  ACAdapter.h
//  ACPIBatteryManager
//
//  Created by RehabMan on 11/16/13.
//
//

#ifndef ACPIBatteryManager_ACAdapter_h
#define ACPIBatteryManager_ACAdapter_h

#include <IOKit/IOService.h>
#include <IOKit/IOReportTypes.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOTimerEventSource.h>
#include "AppleSmartBatteryManager.h"

class EXPORT ACPIACAdapter : public IOService
{
    OSDeclareDefaultStructors(ACPIACAdapter)
    
private:
    IOACPIPlatformDevice*   fProvider;
    IOWorkLoop*             fWorkloop;
    IOCommandGate*          fCommandGate;
    IORecursiveLock*        fLock;
    IOACPIPlatformDevice    *device;
    IOSimpleLock            *lock;
    IOACPIAddress           *info;
    IONotifier*             fPublishNotify;
    IONotifier*             fTerminateNotify;
    
    OSSet*                  fBatteryServices;
    
    bool                    fACConnected;
     
    void                    gatedHandler(IOService* newService, IONotifier * notifier);
    bool                    notificationHandler(void * refCon, IOService * newService, IONotifier * notifier);
    void                    pollState();
public:
    ACPIACAdapter(IOACPIPlatformDevice *device, int32_t id, IOSimpleLock *lock, IOACPIAddress *info) ;
    virtual bool            init(OSDictionary* dict);
    virtual bool            start(IOService* provider);
    virtual void            stop(IOService* provider);
    virtual IOReturn        setPowerState(unsigned long state, IOService* device );
    virtual IOReturn        message(UInt32 type, IOService* provider, void* argument);
};

#endif

//
//  WatchPointDriver.cpp
//
//
//  Created by Milind Chabbi on 2/21/17.
//
//
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <asm/unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <linux/kernel.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <strings.h>
#include <asm/prctl.h>
#include <sys/prctl.h>

#include "common.h"
#include <hpcrun/main.h>
#include <hpcrun/hpcrun_options.h>
#include <hpcrun/write_data.h>
#include <hpcrun/safe-sampling.h>
#include <hpcrun/hpcrun_stats.h>
#include <hpcrun/memory/mmap.h>

#include <hpcrun/cct/cct.h>
#include <hpcrun/metrics.h>
#include <hpcrun/sample_event.h>
#include <hpcrun/sample_sources_registered.h>
#include <hpcrun/thread_data.h>
#include <hpcrun/trace.h>
#include <hpcrun/env.h>

#include <lush/lush-backtrace.h>
#include <messages/messages.h>

#include <utilities/tokenize.h>
#include <utilities/arch/context-pc.h>

#include <unwind/common/unwind.h>

#include "watchpoint_support.h"
#include <unwind/x86-family/x86-misc.h>
#include "matrix.h"
#include <adm_init_fini.h>

extern int init_adamant;

#define MAX_WP_SLOTS (5)
#define IS_ALIGNED(address, alignment) (! ((size_t)(address) & (alignment-1)))
#define ADDRESSES_OVERLAP(addr1, len1, addr2, len2) (((addr1)+(len1) > (addr2)) && ((addr2)+(len2) > (addr1) ))
#define CACHE_LINE_SIZE (64)
//#define ALT_STACK_SZ (4 * SIGSTKSZ)
#define ALT_STACK_SZ ((1L<<20) > 4 * SIGSTKSZ? (1L<<20): 4* SIGSTKSZ)

//#define TEST
#ifdef TEST
#define EMSG(...) fprintf(stderr, __VA_ARGS__)
#define hpcrun_abort() abort()
#define hpcrun_safe_exit() (1)
#define hpcrun_safe_enter() (1)
#define hpcrun_context_pc(context) (0)
#define get_previous_instruction(ip, pip) (0)
#define get_mem_access_length_and_type(a, b, c) (0)
#endif


#if defined(PERF_EVENT_IOC_UPDATE_BREAKPOINT)
#define FAST_BP_IOC_FLAG (PERF_EVENT_IOC_UPDATE_BREAKPOINT)
#elif defined(PERF_EVENT_IOC_MODIFY_ATTRIBUTES)
#define FAST_BP_IOC_FLAG (PERF_EVENT_IOC_MODIFY_ATTRIBUTES)
#else
#endif


#define CHECK(x) ({int err = (x); \
if (err) { \
EMSG("%s: Failed with %d on line %d of file %s\n", strerror(errno), err, __LINE__, __FILE__); \
monitor_real_abort(); }\
err;})


#define HANDLE_ERROR_IF_ANY(val, expected, errstr) {if (val != expected) {perror(errstr); abort();}}
#define SAMPLES_POST_FULL_RESET_VAL (1)


WPConfig_t wpConfig;

//const WatchPointInfo_t dummyWPInfo = {.sample = {}, .startTime =0, .fileHandle= -1, .isActive= false, .mmapBuffer=0};
//const struct DUMMY_WATCHPOINT dummyWP[MAX_WP_SLOTS];


// Data structure that is given by clients to set a WP
typedef struct ThreadData{
    int lbrDummyFD __attribute__((aligned(CACHE_LINE_SZ)));
    stack_t ss;
    void * fs_reg_val;
    void * gs_reg_val;
    uint64_t samplePostFull;
    long numWatchpointTriggers;
    long numWatchpointImpreciseIP;
    long numWatchpointImpreciseAddressArbitraryLength;
    long numWatchpointImpreciseAddress8ByteLength;
    long numSampleTriggeringWatchpoints;
    long numWatchpointDropped;
    long numInsaneIP;
    struct drand48_data randBuffer;
    WatchPointInfo_t watchPointArray[MAX_WP_SLOTS];
    WatchPointUpCall_t fptr;
    char dummy[CACHE_LINE_SZ];
} ThreadData_t;

static __thread ThreadData_t tData;

bool IsAltStackAddress(void *addr){
    if((addr >= tData.ss.ss_sp) && (addr < tData.ss.ss_sp + tData.ss.ss_size))
        return true;
    return false;
}

bool IsFSorGS(void * addr) {
    if (tData.fs_reg_val == (void *) -1) {
        syscall(SYS_arch_prctl, ARCH_GET_FS, &tData.fs_reg_val);
        syscall(SYS_arch_prctl, ARCH_GET_GS, &tData.gs_reg_val);
    }
    // 4096 smallest one page size
    if ( (tData.fs_reg_val <= addr) && (addr < tData.fs_reg_val + 4096))
	return true;
    if ( (tData.gs_reg_val  <= addr) && (addr < tData.gs_reg_val  + 4096))
	return true;
    return false;
}


/********* OS SUPPORT ****************/

// perf-util.h has it
//static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags) {
//    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
//}

static pid_t gettid() {
    return syscall(__NR_gettid);
}


static inline void EnableWatchpoint(int fd) {
    // Start the event
    CHECK(ioctl(fd, PERF_EVENT_IOC_ENABLE, 0));
}

static inline void DisableWatchpoint(WatchPointInfo_t *wpi) {
    // Stop the event
    assert(wpi->fileHandle != -1);
    CHECK(ioctl(wpi->fileHandle, PERF_EVENT_IOC_DISABLE, 0));
    wpi->isActive = false;
}


static void * MAPWPMBuffer(int fd){
    void * buf = mmap(0, 2 * wpConfig.pgsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) {
        EMSG("Failed to mmap : %s\n", strerror(errno));
        monitor_real_abort();
    }
    return buf;
}

static void UNMAPWPMBuffer(void * buf){
    CHECK(munmap(buf, 2 * wpConfig.pgsz));
}

static int OnWatchPoint(int signum, siginfo_t *info, void *context);

__attribute__((constructor))
static void InitConfig(){
    if(!init_adamant) {
	init_adamant = 1;
    	adm_initialize();
    }
    tData.fptr = NULL;
    
    volatile int dummyWP[MAX_WP_SLOTS];
    wpConfig.isLBREnabled = true;
    
    struct perf_event_attr peLBR = {
        .type                   = PERF_TYPE_BREAKPOINT,
        .size                   = sizeof(struct perf_event_attr),
        .bp_type                = HW_BREAKPOINT_W,
        .bp_len                 = HW_BREAKPOINT_LEN_1,
        .bp_addr                = (uintptr_t)&dummyWP[0],
        .sample_period          = 1,
        .precise_ip             = 0 /* arbitraty skid */,
        .sample_type            = 0,
        .exclude_user           = 0,
        .exclude_kernel         = 1,
        .exclude_hv             = 1,
        .disabled               = 0, /* enabled */
    };
    int fd =  perf_event_open(&peLBR, 0, -1, -1 /*group*/, 0);
    if (fd != -1) {
        wpConfig.isLBREnabled = true;
    } else {
        wpConfig.isLBREnabled = false;
    }
    CHECK(close(fd));
    
    
#if defined(FAST_BP_IOC_FLAG)
    wpConfig.isWPModifyEnabled = true;
#else
    wpConfig.isWPModifyEnabled = false;
#endif
    //wpConfig.signalDelivered = SIGTRAP;
    //wpConfig.signalDelivered = SIGIO;
    //wpConfig.signalDelivered = SIGUSR1;
    wpConfig.signalDelivered = SIGRTMIN + 3;
    
    // Setup the signal handler
    sigset_t block_mask;
    sigfillset(&block_mask);
    // Set a signal handler for SIGUSR1
    struct sigaction sa1 = {
        .sa_sigaction = OnWatchPoint,
        .sa_mask = block_mask,
        .sa_flags = SA_SIGINFO | SA_RESTART | SA_NODEFER | SA_ONSTACK
    };
    
    if(monitor_sigaction(wpConfig.signalDelivered, OnWatchPoint, 0 /*flags*/, &sa1) == -1) {
        fprintf(stderr, "Failed to set WHICH_SIG handler: %s\n", strerror(errno));
        monitor_real_abort();
    }
    
    
    
    
    
    wpConfig.pgsz = sysconf(_SC_PAGESIZE);
    
    // identify max WP supported by the architecture
    volatile int wpHandles[MAX_WP_SLOTS];
    int i = 0;
    for(; i < MAX_WP_SLOTS; i++){
        struct perf_event_attr pe = {
            .type                   = PERF_TYPE_BREAKPOINT,
            .size                   = sizeof(struct perf_event_attr),
            .bp_type                = HW_BREAKPOINT_W,
            .bp_len                 = HW_BREAKPOINT_LEN_1,
            .bp_addr                = (uintptr_t)&dummyWP[i],
            .sample_period          = 1,
            .precise_ip             = 0 /* arbitraty skid */,
            .sample_type            = 0,
            .exclude_user           = 0,
            .exclude_kernel         = 1,
            .exclude_hv             = 1,
            .disabled               = 0, /* enabled */
        };
        wpHandles[i] =  perf_event_open(&pe, 0, -1, -1 /*group*/, 0);
        if (wpHandles[i] == -1) {
            break;
        }
    }
    
    if(i == 0) {
        fprintf(stderr, "Cannot create a single watch point\n");
        monitor_real_abort();
    }
    for (int j = 0 ; j < i; j ++) {
        CHECK(close(wpHandles[j]));
    }
    if(atoi(getenv(WATCHPOINT_SIZE)) < i)
	wpConfig.maxWP = atoi(getenv(WATCHPOINT_SIZE));
    else
    	wpConfig.maxWP = i;
    printf("number of watchpoints is %d\n", wpConfig.maxWP);
    
   //wpConfig.maxWP = 1;
    
    // Should we get the floating point type in an access?
    wpConfig.getFloatType = false;
    
    // Get the replacement scheme
    char * replacementScheme = getenv("HPCRUN_WP_REPLACEMENT_SCHEME");
    if(replacementScheme){
        if(0 == strcasecmp(replacementScheme, "AUTO")) {
            wpConfig.replacementPolicy = AUTO;
        } if (0 == strcasecmp(replacementScheme, "OLDEST")) {
            wpConfig.replacementPolicy = OLDEST;
        } if (0 == strcasecmp(replacementScheme, "NEWEST")) {
            wpConfig.replacementPolicy = NEWEST;
        } else {
            // default;
            wpConfig.replacementPolicy = AUTO;
        }
    } else {
        // default;
        wpConfig.replacementPolicy = AUTO;
    }
    
    // Should we fix IP off by one?
    char * fixIP = getenv("HPCRUN_WP_DONT_FIX_IP");
    if(fixIP){
        if(0 == strcasecmp(fixIP, "1")) {
            wpConfig.dontFixIP = true;
        } if (0 == strcasecmp(fixIP, "true")) {
            wpConfig.dontFixIP = true;
        } else {
            // default;
            wpConfig.dontFixIP = false;
        }
    } else {
        // default;
        wpConfig.dontFixIP = false;
    }
    
    // Should we get the address in a WP trigger?
    char * disassembleWPAddress = getenv("HPCRUN_WP_DONT_DISASSEMBLE_TRIGGER_ADDRESS");
    if(disassembleWPAddress){
        if(0 == strcasecmp(disassembleWPAddress, "1")) {
            wpConfig.dontDisassembleWPAddress = true;
        } if (0 == strcasecmp(disassembleWPAddress, "true")) {
            wpConfig.dontDisassembleWPAddress = true;
        } else {
            // default;
            wpConfig.dontDisassembleWPAddress = false;
        }
    } else {
        // default;
        wpConfig.dontDisassembleWPAddress = false;
    }
}

void RedSpyWPConfigOverride(void *v){
    wpConfig.getFloatType = true;
}

void LoadSpyWPConfigOverride(void *v){
    wpConfig.getFloatType = true;
}


void FalseSharingWPConfigOverride(void *v){
    // replacement policy is OLDEST forced.
    wpConfig.replacementPolicy = OLDEST;
}

void ComDetectiveWPConfigOverride(void *v){
    // replacement policy is OLDEST forced.
    wpConfig.replacementPolicy = OLDEST;
}

void TrueSharingWPConfigOverride(void *v){
    // replacement policy is OLDEST forced.
    wpConfig.replacementPolicy = OLDEST;
}

void AllSharingWPConfigOverride(void *v){
    // replacement policy is OLDEST forced.
    wpConfig.replacementPolicy = OLDEST;
}

void IPCFalseSharingWPConfigOverride(void *v){
    // replacement policy is OLDEST forced.
    wpConfig.replacementPolicy = OLDEST;
}

void IPCTrueSharingWPConfigOverride(void *v){
    // replacement policy is OLDEST forced.
    wpConfig.replacementPolicy = OLDEST;
}

void IPCAllSharingWPConfigOverride(void *v){
    // replacement policy is OLDEST forced.
    wpConfig.replacementPolicy = OLDEST;
}


void TemporalReuseWPConfigOverride(void *v){
    // dont fix IP
    wpConfig.dontFixIP = true;
    wpConfig.dontDisassembleWPAddress = true;
}

void SpatialReuseWPConfigOverride(void *v){
    // dont fix IP
    wpConfig.dontFixIP = true;
    wpConfig.dontDisassembleWPAddress = true;
}

static void CreateWatchPoint(WatchPointInfo_t * wpi, SampleData_t * sampleData, bool modify) {
    // Perf event settings
    struct perf_event_attr pe = {
        .type                   = PERF_TYPE_BREAKPOINT,
        .size                   = sizeof(struct perf_event_attr),
        //        .bp_type                = HW_BREAKPOINT_W,
        //        .bp_len                 = HW_BREAKPOINT_LEN_4,
        .sample_period          = 1,
        .precise_ip             = wpConfig.isLBREnabled? 2 /*precise_ip 0 skid*/ : 0 /* arbitraty skid */,
        .sample_type            = (PERF_SAMPLE_IP),
        .exclude_user           = 0,
        .exclude_kernel         = 1,
        .exclude_hv             = 1,
        .disabled               = 0, /* enabled */
    };
    
    switch (sampleData->wpLength) {
        case 1: pe.bp_len = HW_BREAKPOINT_LEN_1; break;
        case 2: pe.bp_len = HW_BREAKPOINT_LEN_2; break;
        case 4: pe.bp_len = HW_BREAKPOINT_LEN_4; break;
        case 8: pe.bp_len = HW_BREAKPOINT_LEN_8; break;
        default:
            EMSG("Unsupported .bp_len %d: %s\n", wpi->sample.wpLength,strerror(errno));
            monitor_real_abort();
    }
    pe.bp_addr = (uintptr_t)sampleData->va;
    
    switch (sampleData->type) {
        case WP_READ: pe.bp_type = HW_BREAKPOINT_R; break;
        case WP_WRITE: pe.bp_type = HW_BREAKPOINT_W; break;
        default: pe.bp_type = HW_BREAKPOINT_W | HW_BREAKPOINT_R;
    }
    
#if defined(FAST_BP_IOC_FLAG)
    if(modify) {
        // modification
        assert(wpi->fileHandle != -1);
        assert(wpi->mmapBuffer != 0);
        //DisableWatchpoint(wpi);
        CHECK(ioctl(wpi->fileHandle, FAST_BP_IOC_FLAG, (unsigned long) (&pe)));
        //if(wpi->isActive == false) {
        //EnableWatchpoint(wpi->fileHandle);
        //}
    } else
#endif
    {
        // fresh creation
        // Create the perf_event for this thread on all CPUs with no event group
        int perf_fd = perf_event_open(&pe, 0, -1, -1 /*group*/, 0);
        if (perf_fd == -1) {
            EMSG("Failed to open perf event file: %s\n",strerror(errno));
            monitor_real_abort();
        }
        // Set the perf_event file to async mode
        CHECK(fcntl(perf_fd, F_SETFL, fcntl(perf_fd, F_GETFL, 0) | O_ASYNC));
        
        // Tell the file to send a signal when an event occurs
        CHECK(fcntl(perf_fd, F_SETSIG, wpConfig.signalDelivered));
        
        // Deliver the signal to this thread
        struct f_owner_ex fown_ex;
        fown_ex.type = F_OWNER_TID;
        fown_ex.pid  = gettid();
        int ret = fcntl(perf_fd, F_SETOWN_EX, &fown_ex);
        if (ret == -1){
            EMSG("Failed to set the owner of the perf event file: %s\n", strerror(errno));
            return;
        }
        
        
        //       CHECK(fcntl(perf_fd, F_SETOWN, gettid()));
        
        wpi->fileHandle = perf_fd;
        // mmap the file if lbr is enabled
        if(wpConfig.isLBREnabled) {
            wpi->mmapBuffer = MAPWPMBuffer(perf_fd);
        }
    }
    
    wpi->isActive = true;
    wpi->va = (void *) pe.bp_addr;
    wpi->sample = *sampleData;
    wpi->startTime = rdtsc();
    wpi->bulletinBoardTimestamp = sampleData->bulletinBoardTimestamp;
}


/* create a dummy PERF_TYPE_HARDWARE event that will never fire */
static void CreateDummyHardwareEvent(void) {
    // Perf event settings
    struct perf_event_attr pe = {
        .type                   = PERF_TYPE_HARDWARE,
        .size                   = sizeof(struct perf_event_attr),
        .config                 = PERF_COUNT_HW_CACHE_MISSES,
        .sample_period          = 0x7fffffffffffffff, /* some insanely large sample period */
        .precise_ip             = 2,
        .sample_type            = PERF_SAMPLE_BRANCH_STACK,
        .exclude_user           = 0,
        .exclude_kernel         = 1,
        .exclude_hv             = 1,
        .branch_sample_type     = PERF_SAMPLE_BRANCH_ANY,
    };
    
    // Create the perf_event for this thread on all CPUs with no event group
    int perf_fd = perf_event_open(&pe, 0, -1, -1, 0);
    if (perf_fd == -1) {
        EMSG("Failed to open perf event file: %s\n", strerror(errno));
        monitor_real_abort();
    }
    tData.lbrDummyFD = perf_fd;
}

static void CloseDummyHardwareEvent(int perf_fd){
    CHECK(close(perf_fd));
}


/*********** Client interfaces *******/

static void DisArm(WatchPointInfo_t * wpi){
    
    //    assert(wpi->isActive);
    assert(wpi->fileHandle != -1);
    
    if(wpi->mmapBuffer)
        UNMAPWPMBuffer(wpi->mmapBuffer);
    wpi->mmapBuffer = 0;
    
    CHECK(close(wpi->fileHandle));
    wpi->fileHandle = -1;
    wpi->isActive = false;
}

static bool ArmWatchPoint(WatchPointInfo_t * wpi, SampleData_t * sampleData) {
    // if WP modification is suppoted use it
    //void * cacheLineBaseAddress = (void *) ((uint64_t)((size_t)sampleData->va) & (~(64-1)));

    if(wpConfig.isWPModifyEnabled){
        // Does not matter whether it was active or not.
        // If it was not active, enable it.
        if(wpi->fileHandle != -1) {
            CreateWatchPoint(wpi, sampleData, true);
            return true;
        }
    }
    // disable the old WP if active
    if(wpi->isActive) {
        DisArm(wpi);
    }
    CreateWatchPoint(wpi, sampleData, false);
    return true;
}

// Per thread initialization

void WatchpointThreadInit(WatchPointUpCall_t func){
    tData.ss.ss_sp = malloc(ALT_STACK_SZ);
    if (tData.ss.ss_sp == NULL){
        EMSG("Failed to malloc ALT_STACK_SZ");
        monitor_real_abort();
    }
    tData.ss.ss_size = ALT_STACK_SZ;
    tData.ss.ss_flags = 0;
    if (sigaltstack(&tData.ss, NULL) == -1){
        EMSG("Failed sigaltstack");
        monitor_real_abort();
    }
    
    tData.lbrDummyFD = -1;
    tData.fptr = func;
    tData.fs_reg_val = (void*)-1;
    tData.gs_reg_val = (void*)-1;
    srand48_r(time(NULL), &tData.randBuffer);
    tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
    tData.numWatchpointTriggers = 0;
    tData.numWatchpointImpreciseIP = 0;
    tData.numWatchpointImpreciseAddressArbitraryLength = 0;
    tData.numWatchpointImpreciseAddress8ByteLength = 0;
    tData.numWatchpointDropped = 0;
    tData.numSampleTriggeringWatchpoints = 0;
    tData.numInsaneIP = 0;
    
    
    for (int i=0; i<wpConfig.maxWP; i++) {
        tData.watchPointArray[i].isActive = false;
        tData.watchPointArray[i].fileHandle = -1;
        tData.watchPointArray[i].startTime = 0;
    }
    
    //if LBR is supported create a dummy PERF_TYPE_HARDWARE for Linux workaround
    if(wpConfig.isLBREnabled) {
        CreateDummyHardwareEvent();
    }
}

void WatchpointThreadTerminate(){
    for (int i = 0; i < wpConfig.maxWP; i++) {
        if(tData.watchPointArray[i].fileHandle != -1) {
            DisArm(&tData.watchPointArray[i]);
        }
    }
    
    if(tData.lbrDummyFD != -1) {
        CloseDummyHardwareEvent(tData.lbrDummyFD);
        tData.lbrDummyFD = -1;
    }
    tData.fs_reg_val = (void*)-1;
    tData.gs_reg_val = (void*)-1;
    
    hpcrun_stats_num_watchpoints_triggered_inc(tData.numWatchpointTriggers);
    hpcrun_stats_num_watchpoints_imprecise_inc(tData.numWatchpointImpreciseIP);
    hpcrun_stats_num_watchpoints_imprecise_address_inc(tData.numWatchpointImpreciseAddressArbitraryLength);
    hpcrun_stats_num_watchpoints_imprecise_address_8_byte_inc(tData.numWatchpointImpreciseAddress8ByteLength);
    hpcrun_stats_num_insane_ip_inc(tData.numInsaneIP);
    hpcrun_stats_num_watchpoints_dropped_inc(tData.numWatchpointDropped);
    hpcrun_stats_num_sample_triggering_watchpoints_inc(tData.numSampleTriggeringWatchpoints);
#if 0
    tData.ss.ss_flags = SS_DISABLE;
    if (sigaltstack(&tData.ss, NULL) == -1){
        EMSG("Failed sigaltstack WatchpointThreadTerminate");
        // no need to abort , just leak the memory
        // monitor_real_abort();
    } else {
        if(tData.ss.ss_sp)
            free(tData.ss.ss_sp);
    }
#endif
}



// Finds a victim slot to set a new WP
static VictimType GetVictim(int * location, ReplacementPolicy policy){
    // If any WP slot is inactive, return it;
    for(int i = 0; i < wpConfig.maxWP; i++){
        if(!tData.watchPointArray[i].isActive) {
            *location = i;
            return EMPTY_SLOT;
        }
    }
    switch (policy) {
        case AUTO:{
            // Equal probability for any data access
            
            
            // Randomly pick a slot to victimize.
            long int tmpVal;
            lrand48_r(&tData.randBuffer, &tmpVal);
            int rSlot = tmpVal % wpConfig.maxWP;
            *location = rSlot;
            
            // if it is the first sample after full, use wpConfig.maxWP/(wpConfig.maxWP+1) probability to replace.
            // if it is the second sample after full, use wpConfig.maxWP/(wpConfig.maxWP+2) probability to replace.
            // if it is the third sample after full, use wpConfig.maxWP/(wpConfig.maxWP+3) probability replace.
            
            double probabilityToReplace =  wpConfig.maxWP/((double)wpConfig.maxWP+tData.samplePostFull);
            double randValue;
            drand48_r(&tData.randBuffer, &randValue);
            
            // update tData.samplePostFull
            tData.samplePostFull++;
            
            if(randValue <= probabilityToReplace) {
                return NON_EMPTY_SLOT;
            }
            // this is an indication not to replace, but if the client chooses to force, they can
            return NONE_AVAILABLE;
        }
            break;
            
        case NEWEST:{
            // Always replace the newest
            
            int64_t newestTime = 0;
            for(int i = 0; i < wpConfig.maxWP; i++){
                if(newestTime < tData.watchPointArray[i].startTime) {
                    *location = i;
                    newestTime = tData.watchPointArray[i].startTime;
                }
            }
            return NON_EMPTY_SLOT;
        }
            break;
            
        case OLDEST:{
            // Always replace the oldest
            
            int64_t oldestTime = INT64_MAX;
            for(int i = 0; i < wpConfig.maxWP; i++){
                if(oldestTime > tData.watchPointArray[i].startTime) {
                    *location = i;
                    oldestTime = tData.watchPointArray[i].startTime;
                }
            }
            return NON_EMPTY_SLOT;
        }
            break;
            
        case EMPTY_SLOT_ONLY:{
            return NONE_AVAILABLE;
        }
            break;
        default:
            return NONE_AVAILABLE;
    }
    // No unarmed WP slot found.
}

static inline void
rmb(void) {
    asm volatile("lfence":::"memory");
}

static void ConsumeAllRingBufferData(void  *mbuf) {
    struct perf_event_mmap_page *hdr = (struct perf_event_mmap_page *)mbuf;
    unsigned long tail;
    size_t avail_sz;
    size_t pgmsk = wpConfig.pgsz - 1;
    /*
     * data points to beginning of buffer payload
     */
    void * data = ((void *)hdr) + wpConfig.pgsz;
    
    /*
     * position of tail within the buffer payload
     */
    tail = hdr->data_tail & pgmsk;
    
    /*
     * size of what is available
     *
     * data_head, data_tail never wrap around
     */
    avail_sz = hdr->data_head - hdr->data_tail;
    rmb();
#if 0
    if(avail_sz == 0 )
        EMSG("\n avail_sz = %d\n", avail_sz);
    else
        EMSG("\n EEavail_sz = %d\n", avail_sz);
#endif
    // reset tail to head
    hdr->data_tail = hdr->data_head;
}



static int ReadMampBuffer(void  *mbuf, void *buf, size_t sz) {
    struct perf_event_mmap_page *hdr = (struct perf_event_mmap_page *)mbuf;
    void *data;
    unsigned long tail;
    size_t avail_sz, m, c;
    size_t pgmsk = wpConfig.pgsz - 1;
    /*
     * data points to beginning of buffer payload
     */
    data = ((void *)hdr) + wpConfig.pgsz;
    
    /*
     * position of tail within the buffer payload
     */
    tail = hdr->data_tail & pgmsk;
    
    /*
     * size of what is available
     *
     * data_head, data_tail never wrap around
     */
    avail_sz = hdr->data_head - hdr->data_tail;
    if (sz > avail_sz) {
        printf("\n sz > avail_sz: sz = %lu, avail_sz = %lu\n", sz, avail_sz);
        rmb();
        return -1;
    }
    
    /* From perf_event_open() manpage */
    rmb();
    
    
    /*
     * sz <= avail_sz, we can satisfy the request
     */
    
    /*
     * c = size till end of buffer
     *
     * buffer payload size is necessarily
     * a power of two, so we can do:
     */
    c = pgmsk + 1 -  tail;
    
    /*
     * min with requested size
     */
    m = c < sz ? c : sz;
    
    /* copy beginning */
    memcpy(buf, data + tail, m);
    
    /*
     * copy wrapped around leftover
     */
    if (sz > m)
        memcpy(buf + m, data, sz - m);
    
    hdr->data_tail += sz;
    
    return 0;
}


void
SkipBuffer(struct perf_event_mmap_page *hdr, size_t sz){
    if ((hdr->data_tail + sz) > hdr->data_head)
        sz = hdr->data_head - hdr->data_tail;
    rmb();
    hdr->data_tail += sz;
}

static inline bool IsPCSane(void * contextPC, void *possiblePC){
    if( (possiblePC==0) || ((possiblePC > contextPC) ||  (contextPC-possiblePC > 15))){
        return false;
    }
    return true;
}


double ProportionOfWatchpointAmongOthersSharingTheSameContext(WatchPointInfo_t *wpi){
#if 0
    int share = 0;
    for(int i = 0; i < wpConfig.maxWP; i++) {
        if(tData.watchPointArray[i].isActive && tData.watchPointArray[i].sample.node == wpi->sample.node) {
            share ++;
        }
    }
    assert(share > 0);
    return 1.0/share;
#else
    return 1.0;
#endif
}

static inline void *  GetPatchedIP(void *  contextIP) {
    void * patchedIP;
    void * excludeList[MAX_WP_SLOTS] = {0};
    int numExcludes = 0;
    for(int idx = 0; idx < wpConfig.maxWP; idx++){
        if(tData.watchPointArray[idx].isActive) {
            excludeList[numExcludes]=tData.watchPointArray[idx].va;
            numExcludes++;
        }
    }
    get_previous_instruction(contextIP, &patchedIP, excludeList, numExcludes);
    return patchedIP;
}

// Gather all useful data when a WP triggers
static bool CollectWatchPointTriggerInfo(WatchPointInfo_t  * wpi, WatchPointTrigger_t *wpt, void * context){
    //struct perf_event_mmap_page * b = wpi->mmapBuffer;
    struct perf_event_header hdr;
    
    if (ReadMampBuffer(wpi->mmapBuffer, &hdr, sizeof(struct perf_event_header)) < 0) {
        EMSG("Failed to ReadMampBuffer: %s\n", strerror(errno));
        monitor_real_abort();
    }
    switch(hdr.type) {
        case PERF_RECORD_SAMPLE:
            assert (hdr.type & PERF_SAMPLE_IP);
            void *  contextIP = hpcrun_context_pc(context);
            void *  preciseIP = (void *)-1;
            void *  patchedIP = (void *)-1;
            void *  reliableIP = (void *)-1;
            void *  addr = (void *)-1;
            if (hdr.type & PERF_SAMPLE_IP){
                if (ReadMampBuffer(wpi->mmapBuffer, &preciseIP, sizeof(uint64_t)) < 0) {
                    EMSG("Failed to ReadMampBuffer: %s\n", strerror(errno));
                    monitor_real_abort();
                }
                
                if(! (hdr.misc & PERF_RECORD_MISC_EXACT_IP)){
                    //EMSG("PERF_SAMPLE_IP imprecise\n");
                    tData.numWatchpointImpreciseIP ++;
                    if(wpConfig.dontFixIP == false) {
                        patchedIP = GetPatchedIP(contextIP);
                        if(!IsPCSane(contextIP, patchedIP)) {
                            //EMSG("get_previous_instruction  failed \n");
                            tData.numInsaneIP ++;
                            goto ErrExit;
                        }
                        reliableIP = patchedIP;
                    } else {
                        // Fake as requested by Xu for reuse clients
                        reliableIP = contextIP-1;
                    }
                    //EMSG("PERF_SAMPLE_IP imprecise: %p patched to %p in WP handler\n", tmpIP, patchedIP);
                } else {
#if 0 // Precise PC can be far away in jump/call instructions.
                    // Ensure the "precise" PC is within one instruction from context pc
                    if(!IsPCSane(contextIP, preciseIP)) {
                        tData.numInsaneIP ++;
                        //EMSG("get_previous_instruction failed \n");
                        goto ErrExit;
                    }
#endif
                    reliableIP = preciseIP;
                    //if(! ((ip <= tmpIP) && (tmpIP-ip < 20))) ConsumeAllRingBufferData(wpi->mmapBuffer);
                    //assert( (ip <= tmpIP) && (tmpIP-ip < 20));
                }
            } else {
                // Should happen only for wpConfig.isLBREnabled==false
                assert(wpConfig.isLBREnabled==false);
                // Fall back to old scheme of disassembling and capturing the info
                if(wpConfig.dontFixIP == false) {
                    patchedIP = GetPatchedIP(contextIP);
                    if(!IsPCSane(contextIP, patchedIP)) {
                        tData.numInsaneIP ++;
                        //EMSG("PERF_SAMPLE_IP imprecise: %p failed to patch in  WP handler, WP dropped\n", tmpIP);
                        goto ErrExit;
                    }
                    reliableIP = patchedIP;
                }else {
                    // Fake as requested by Xu for reuse clients
                    reliableIP = contextIP-1;
                }
            }
            
            wpt->pc = reliableIP;
            
            if(wpConfig.dontDisassembleWPAddress == false){
                FloatType * floatType = wpConfig.getFloatType? &wpt->floatType : 0;
                if(false == get_mem_access_length_and_type_address(wpt->pc, (uint32_t*) &(wpt->accessLength), &(wpt->accessType), floatType, context, &addr)){
                    //EMSG("WP triggered on a non Load/Store add = %p\n", wpt->pc);
                    goto ErrExit;
                }
                if (wpt->accessLength == 0) {
                    //EMSG("WP triggered 0 access length! at pc=%p\n", wpt->pc);
                    goto ErrExit;
                }
                
                
                void * patchedAddr = (void *)-1;
                // Stack affecting addresses will be off by 8
                // Some instructions affect the address computing register: mov    (%rax),%eax
                // Hence, if the addresses do NOT overlap, merely use the Sample address!
                if(false == ADDRESSES_OVERLAP(addr, wpt->accessLength, wpi->va, wpi->sample.wpLength)) {
                    if ((wpt->accessLength == sizeof(void *)) && (wpt->accessLength == wpi->sample.wpLength) &&  (((addr - wpi->va) == sizeof(void *)) || ((wpi->va - addr) == sizeof(void *))))
                        tData.numWatchpointImpreciseAddress8ByteLength ++;
                    else
                        tData.numWatchpointImpreciseAddressArbitraryLength ++;

                    
                    tData.numWatchpointImpreciseAddressArbitraryLength ++;
                    patchedAddr = wpi->va;
                } else {
                    patchedAddr = addr;
                }
                wpt->va = patchedAddr;
            } else {
                wpt->va = (void *)-1;
            }
            wpt->ctxt = context;
            // We must cleanup the mmap buffer if there is any data left
            ConsumeAllRingBufferData(wpi->mmapBuffer);
            return true;
        case PERF_RECORD_EXIT:
            EMSG("PERF_RECORD_EXIT sample type %d sz=%d\n", hdr.type, hdr.size);
            //SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
            goto ErrExit;
        case PERF_RECORD_LOST:
            EMSG("PERF_RECORD_LOST sample type %d sz=%d\n", hdr.type, hdr.size);
            //SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
            goto ErrExit;
        case PERF_RECORD_THROTTLE:
            EMSG("PERF_RECORD_THROTTLE sample type %d sz=%d\n", hdr.type, hdr.size);
            //SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
            goto ErrExit;
        case PERF_RECORD_UNTHROTTLE:
            EMSG("PERF_RECORD_UNTHROTTLE sample type %d sz=%d\n", hdr.type, hdr.size);
            //SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
            goto ErrExit;
        default:
            EMSG("unknown sample type %d sz=%d\n", hdr.type, hdr.size);
            //SkipBuffer(wpi->mmapBuffer , hdr.size - sizeof(hdr));
            goto ErrExit;
    }
    
ErrExit:
    // We must cleanup the mmap buffer if there is any data left
    ConsumeAllRingBufferData(wpi->mmapBuffer);
    return false;
}

void DisableWatchpointWrapper(WatchPointInfo_t *wpi){
    if(wpConfig.isWPModifyEnabled) {
        DisableWatchpoint(wpi);
    } else {
        DisArm(wpi);
    }
}

static int OnWatchPoint(int signum, siginfo_t *info, void *context){
//volatile int x;
//fprintf(stderr, "OnWatchPoint=%p\n", &x);
    //printf("OnWatchPoint is executed\n");
    // Disable HPCRUN sampling
    // if the trap is already in hpcrun, return
    // If the interrupt came from inside our code, then drop the sample
    // and return and avoid any MSG.
    void* pc = hpcrun_context_pc(context);
    if (!hpcrun_safe_enter_async(pc)) return 0;
    
    linux_perf_events_pause();
    
    tData.numWatchpointTriggers++;
    //fprintf(stderr, " numWatchpointTriggers = %lu, \n", tData.numWatchpointTriggers);
    
    //find which watchpoint fired
    int location = -1;
    for(int i = 0 ; i < wpConfig.maxWP; i++) {
        if((tData.watchPointArray[i].isActive) && (info->si_fd == tData.watchPointArray[i].fileHandle)) {
            location = i;
            break;
        }
    }
    
    // Ensure it is an active WP
    if(location == -1) {
        EMSG("\n WP trigger did not match any known active WP\n");
        //monitor_real_abort();
        hpcrun_safe_exit();
        linux_perf_events_resume();
        //fprintf("\n WP trigger did not match any known active WP\n");
        return 0;
    }
    
    WatchPointTrigger_t wpt;
    WPTriggerActionType retVal;
    WatchPointInfo_t *wpi = &tData.watchPointArray[location];
    // Perform Pre watchpoint action
    switch (wpi->sample.preWPAction) {
        case DISABLE_WP:
            DisableWatchpointWrapper(wpi);
            break;
        case DISABLE_ALL_WP:
            for(int i = 0; i < wpConfig.maxWP; i++) {
                if(tData.watchPointArray[i].isActive){
                    DisableWatchpointWrapper(&tData.watchPointArray[i]);
                }
            }
            break;
        default:
            assert(0 && "NYI");
            monitor_real_abort();
            break;
    }
    
    
    if( false == CollectWatchPointTriggerInfo(wpi, &wpt, context)) {
        tData.numWatchpointDropped++;
        retVal = DISABLE_WP; // disable if unable to collect any info.
    } else {
        retVal = tData.fptr(wpi, 0, wpt.accessLength/* invalid*/,  &wpt);
    }
    
    // Let the client take action.
    switch (retVal) {
        case DISABLE_WP: {
            if(wpi->isActive){
                DisableWatchpointWrapper(wpi);
            }
            //reset to tData.samplePostFull
            tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
        }
        break;
        case DISABLE_ALL_WP: {
            for(int i = 0; i < wpConfig.maxWP; i++) {
                if(tData.watchPointArray[i].isActive){
                    DisableWatchpointWrapper(&tData.watchPointArray[i]);
                }
            }
            //reset to tData.samplePostFull to SAMPLES_POST_FULL_RESET_VAL
            tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
        }
        break;
        case ALREADY_DISABLED: { // Already disabled, perhaps in pre-WP action
            assert(wpi->isActive == false);
            tData.samplePostFull = SAMPLES_POST_FULL_RESET_VAL;
        }
        break;
        case RETAIN_WP: { // resurrect this wp
            if(!wpi->isActive){
                EnableWatchpoint(wpi->fileHandle);
                wpi->isActive = true;
            }
        }
        break;
        default: // Retain the state
            break;
    }
    //    hpcrun_all_sources_start();
    linux_perf_events_resume();
    hpcrun_safe_exit();
    return 0;
}

static bool ValidateWPData(SampleData_t * sampleData){
    // Check alignment
#if defined(__x86_64__) || defined(__amd64__) || defined(__x86_64) || defined(__amd64)
    switch (sampleData->wpLength) {
        case 0: EMSG("\nValidateWPData: 0 length WP never allowed"); monitor_real_abort();
        case 1:
        case 2:
        case 4:
        case 8:
            if(IS_ALIGNED(sampleData->va, sampleData->wpLength))
                return true; // unaligned
            else
                return false;
            break;
            
        default:
            EMSG("Unsuppported WP length %d", sampleData->wpLength);
            monitor_real_abort();
            return false; // unsupported alignment
    }
#else
#error "unknown architecture"
#endif
}

static bool IsOveralpped(SampleData_t * sampleData){
    // Is a WP with the same/overlapping address active?
    for (int i = 0;  i < wpConfig.maxWP; i++) {
        if(tData.watchPointArray[i].isActive){
            if(ADDRESSES_OVERLAP(tData.watchPointArray[i].sample.va, tData.watchPointArray[i].sample.wpLength, sampleData->va, sampleData->wpLength)){
                return true;
            }
        }
    }
    return false;
}


void CaptureValue(SampleData_t * sampleData, WatchPointInfo_t * wpi){
    void * valLoc = & (wpi->value[0]);
    switch(sampleData->wpLength) {
        default: // force 1 length
        case 1: *((uint8_t*)valLoc) = *(uint8_t*)(sampleData->va); break;
        case 2: *((uint16_t*)valLoc) = *(uint16_t*)(sampleData->va); break;
        case 4: *((uint32_t*)valLoc) = *(uint32_t*)(sampleData->va); break;
        case 8: *((uint64_t*)valLoc) = *(uint64_t*)(sampleData->va); break;
    }
}


bool SubscribeWatchpoint(SampleData_t * sampleData, OverwritePolicy overwritePolicy, bool captureValue){
    if(ValidateWPData(sampleData) == false) {
        return false;
    }
    if(IsOveralpped(sampleData)){
        return false; // drop the sample if it overlaps an existing address
    }
    
    // No overlap, look for a victim slot
    int victimLocation = -1;
    // Find a slot to install WP
    VictimType r = GetVictim(&victimLocation, wpConfig.replacementPolicy);
    
    if(r != NONE_AVAILABLE) {
        // VV IMP: Capture value before arming the WP.
        if(captureValue) {
            CaptureValue(sampleData, &tData.watchPointArray[victimLocation]);
	}
        // I know the error case that we have captured the value but ArmWatchPoint fails.
        // I am not handling that corner case because ArmWatchPoint() will fail with a monitor_real_abort().
        //printf("and this region\n");
	//printf("arming watchpoints\n");
        if(ArmWatchPoint(&tData.watchPointArray[victimLocation], sampleData) == false){
            //LOG to hpcrun log
            EMSG("ArmWatchPoint failed for address %p", sampleData->va);
            return false;
        }
        return true;
    }
    return false;
}

bool SubscribeWatchpointWithTime(SampleData_t * sampleData, OverwritePolicy overwritePolicy, bool captureValue, uint64_t curTime, uint64_t lastTime){
    if(ValidateWPData(sampleData) == false) {
        return false;
    }
    if(IsOveralpped(sampleData)){
        return false; // drop the sample if it overlaps an existing address
    }
    
    // No overlap, look for a victim slot
    int victimLocation = -1;
    // Find a slot to install WP
    VictimType r = GetVictim(&victimLocation, wpConfig.replacementPolicy);
    
    if(r != NONE_AVAILABLE) {
        // VV IMP: Capture value before arming the WP.
        if(captureValue) {
            CaptureValue(sampleData, &tData.watchPointArray[victimLocation]);
	}
        // I know the error case that we have captured the value but ArmWatchPoint fails.
        // I am not handling that corner case because ArmWatchPoint() will fail with a monitor_real_abort().
        //printf("and this region\n");
	//printf("arming watchpoints\n");
	if((sampleData->bulletinBoardTimestamp - tData.watchPointArray[victimLocation].bulletinBoardTimestamp) > (curTime - lastTime)) {
		//printf("watchpoints are armed on address %lx, length: %d\n", sampleData->va, sampleData->accessLength);
        	if(ArmWatchPoint(&tData.watchPointArray[victimLocation], sampleData) == false){
            		//LOG to hpcrun log
            		EMSG("ArmWatchPoint failed for address %p", sampleData->va);
            		return false;
        	}
	} /*else {
		printf("watchpoints are not armed because they are still new\n");
	}*/
        return true;
    }
    return false;
}

#ifdef TEST
#include<omp.h>


__thread volatile int cnt;
WPUpCallTRetType Test1UpCall(WatchPointInfo_t * wp, WatchPointTrigger_t * wt) {
    printf("\n Test1UpCall %p\n", wt->va);
    if(wpConfig.isLBREnabled)
        assert(wp->sample.va == wt->va);
    
    cnt ++;
    return DISABLE;
}

void TestBasic(){
    tData.fptr = Test1UpCall;
    
    sigset_t block_mask;
    sigemptyset (&block_mask);
    // Set a signal handler for SIGUSR1
    struct sigaction sa1 = {
        .sa_sigaction = OnWatchPoint,
        //        .sa_mask = block_mask,
        .sa_flags = SA_SIGINFO | SA_RESTART | SA_NODEFER
    };
    
    if(sigaction(wpConfig.signalDelivered, &sa1, NULL) == -1) {
        fprintf(stderr, "Failed to set WHICH_SIG handler: %s\n", strerror(errno));
        monitor_real_abort();
    }
    
    
    WatchpointThreadInit();
    int N = 10000;
    volatile int dummyWPLocation[10000];
    cnt = 0;
    
    for(int i = 0 ; i < N; i++) {
        SampleData_t s = {.va = &dummyWPLocation[i], .wpLength = sizeof(int), .type = WP_WRITE};
        SubscribeWatchpoint(&s, AUTO);
    }
    for(int i = 0 ; i < N; i++) {
        dummyWPLocation[i]++;
    }
    printf("\n cnt = %d\n", cnt);
    assert(cnt == wpConfig.maxWP);
    WatchpointThreadTerminate();
}

int main() {
    printf("\n Test 1: single threaded");
    while(1) {
#pragma omp parallel
        {
            TestBasic();
        }
    }
    return 0;
}
#endif

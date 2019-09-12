// -*-Mode: C++;-*- // technically C99

// * BeginRiceCopyright *****************************************************
//
// $HeadURL: https://outreach.scidac.gov/svn/hpctoolkit/trunk/src/tool/hpcrun/sample-sources/papi.c $
// $Id: papi.c 4027 2012-11-28 20:03:03Z krentel $
//
// --------------------------------------------------------------------------
// Part of HPCToolkit (hpctoolkit.org)
//
// Information about sources of support for research and development of
// HPCToolkit is at 'hpctoolkit.org' and in 'README.Acknowledgments'.
// --------------------------------------------------------------------------
//
// Copyright ((c)) 2002-2014, Rice University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
// * Neither the name of Rice University (RICE) nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// This software is provided by RICE and contributors "as is" and any
// express or implied warranties, including, but not limited to, the
// implied warranties of merchantability and fitness for a particular
// purpose are disclaimed. In no event shall RICE or contributors be
// liable for any direct, indirect, incidental, special, exemplary, or
// consequential damages (including, but not limited to, procurement of
// substitute goods or services; loss of use, data, or profits; or
// business interruption) however caused and on any theory of liability,
// whether in contract, strict liability, or tort (including negligence
// or otherwise) arising in any way out of the use of this software, even
// if advised of the possibility of such damage.
//
// ******************************************************* EndRiceCopyright *

//
// WATCHPOINT sample source oo interface
//


/******************************************************************************
 * system includes
 *****************************************************************************/
#include <alloca.h>
#include <assert.h>
#include <ctype.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ucontext.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <linux/perf_event.h>
/******************************************************************************
 * libmonitor
 *****************************************************************************/
#include <monitor.h>

/******************************************************************************
 * local includes
 *****************************************************************************/

#include "simple_oo.h"
#include "sample_source_obj.h"
#include "common.h"

#include <hpcrun/hpcrun_options.h>
#include <hpcrun/hpcrun_stats.h>
#include <hpcrun/metrics.h>
#include <hpcrun/safe-sampling.h>
#include <hpcrun/sample_sources_registered.h>
#include <hpcrun/sample_event.h>
#include <hpcrun/thread_data.h>
#include <hpcrun/threadmgr.h>
#include <hpcrun/files.h>
#include <hpcrun/env.h>

#include <sample-sources/blame-shift/blame-shift.h>
#include <utilities/tokenize.h>
#include <messages/messages.h>
#include <lush/lush-backtrace.h>
#include <lib/prof-lean/hpcrun-fmt.h>
#include <lib/prof-lean/splay-macros.h>


// necessary for breakpoints
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <asm/unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>
#include <xmmintrin.h>
#include <immintrin.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>

#include "datacentric.h"
#include <hpcrun/unwind/x86-family/x86-move.h>
#include <utilities/arch/context-pc.h>
#include "watchpoint_support.h"
#include <unwind/x86-family/x86-misc.h>
#include "perf/perf-util.h"
#include <hpcrun/handling_sample.h>
#include <adm_init_fini.h>
#include "matrix.h"

int red_metric_id = -1;
int redApprox_metric_id = -1;
int load_metric_id = -1;
int dead_metric_id = -1;
int measured_metric_id = -1;
int latency_metric_id = -1;
int temporal_metric_id = -1;
int spatial_metric_id = -1;
int false_ww_metric_id = -1;
int false_rw_metric_id = -1;
int false_wr_metric_id = -1;
int true_ww_metric_id = -1;
int true_rw_metric_id = -1;
int true_wr_metric_id = -1;

#define NUM_WATERMARK_METRICS (4)
int curWatermarkId = 0;
int watermark_metric_id[NUM_WATERMARK_METRICS] = {-1, -1, -1, -1};
int pebs_metric_id[NUM_WATERMARK_METRICS] = {-1, -1, -1, -1};

void SetupWatermarkMetric(int metricId){
    if (curWatermarkId == NUM_WATERMARK_METRICS) {
        EEMSG("curWatermarkId == NUM_WATERMARK_METRICS = %d", NUM_WATERMARK_METRICS);
        monitor_real_abort();
    }
    pebs_metric_id[curWatermarkId]=metricId;
    watermark_metric_id[curWatermarkId] = hpcrun_new_metric();
    char metricName [1000] = "IGNORE_ME";
    sprintf(metricName,"%s_%d","IGNORE_ME", metricId);
    hpcrun_set_metric_info_and_period(watermark_metric_id[curWatermarkId], strdup(metricName) /*never freed */, MetricFlags_ValFmt_Real, 1, metric_property_none);
    curWatermarkId++;
}

static dso_info_t * hpcrunLM;
static dso_info_t * libmonitorLM;

typedef struct WPStats{
    long numImpreciseSamples __attribute__((aligned(CACHE_LINE_SZ)));
    long numWatchpointsSet;
    char dummy[CACHE_LINE_SZ];
}WPStats_t;

__thread WPStats_t wpStats;

/******************************************************************************
 * macros
 *****************************************************************************/

#define OVERFLOW_MODE 0
#define WEIGHT_METRIC 0
#define DEFAULT_THRESHOLD  2000000L
#define APPROX_RATE (0.01)
#define WP_DEADSPY_EVENT_NAME "WP_DEADSPY"
#define WP_REDSPY_EVENT_NAME "WP_REDSPY"
#define WP_LOADSPY_EVENT_NAME "WP_LOADSPY"
#define WP_TEMPORAL_REUSE_EVENT_NAME "WP_TEMPORAL_REUSE"
#define WP_SPATIAL_REUSE_EVENT_NAME "WP_SPATIAL_REUSE"
#define WP_FALSE_SHARING_EVENT_NAME "WP_FALSE_SHARING"
#define WP_TRUE_SHARING_EVENT_NAME "WP_TRUE_SHARING"
#define WP_ALL_SHARING_EVENT_NAME "WP_ALL_SHARING"
#define WP_COMDETECTIVE_EVENT_NAME "WP_COMDETECTIVE"
#define WP_IPC_FALSE_SHARING_EVENT_NAME "WP_IPC_FALSE_SHARING"
#define WP_IPC_TRUE_SHARING_EVENT_NAME "WP_IPC_TRUE_SHARING"
#define WP_IPC_ALL_SHARING_EVENT_NAME "WP_IPC_ALL_SHARING"


typedef enum WP_CLIENT_ID{
    WP_DEADSPY,
    WP_REDSPY,
    WP_LOADSPY,
    WP_TEMPORAL_REUSE,
    WP_SPATIAL_REUSE,
    WP_FALSE_SHARING,
    WP_COMDETECTIVE,
    WP_ALL_SHARING,
    WP_TRUE_SHARING,
    WP_IPC_FALSE_SHARING,
    WP_IPC_TRUE_SHARING,
    WP_IPC_ALL_SHARING,
    WP_MAX_CLIENTS }WP_CLIENT_ID;

typedef struct WpClientConfig{
    WP_CLIENT_ID id;
    char * name;
    WatchPointUpCall_t wpCallback;
    WPTriggerActionType preWPAction;
    ClientConfigOverrideCall_t configOverrideCallback;
}WpClientConfig_t;
/*
SharedData_t wBulletinBoard = {.counter = 0, .time=0, .wpType = -1, .accessType = UNKNOWN, .sampleType = UNKNOWN_SAMPLE_TYPE, .tid = -1, .expiration_period = 0, .address = 0, .matrix_counter = 0};

HashTable_t bulletinBoard = {.counter = 0};
*/
typedef struct SharedData{
    volatile uint64_t counter __attribute__((aligned(CACHE_LINE_SZ)));
    uint64_t time __attribute__((aligned(CACHE_LINE_SZ)));
    int tid;
    WatchPointType wpType;
    AccessType accessType;
    void *address;
    int accessLen;
    cct_node_t * node;
    char dummy[CACHE_LINE_SZ];
} SharedData_t;

SharedData_t gSharedData = {.counter = 0, .time=0, .wpType = -1, .accessType = UNKNOWN, .tid = -1, .address = 0};

HashTable_t bulletinBoard = {.counter = 0};

__thread uint64_t prev_timestamp = -1;

// before
/*
typedef struct SharedItem{
    uint64_t time __attribute__((aligned(CACHE_LINE_SZ)));
    int tid;
    WatchPointType wpType;
    AccessType accessType;
    SampleType sampleType;
    void *address;
    int accessLen;
    char dummy[CACHE_LINE_SZ];
} SharedItem_t;

typedef struct hashTableStruct{
	volatile uint64_t counter __attribute__((aligned(CACHE_LINE_SZ)));
	struct SharedItem *hashTable[HASHTABLESIZE];
} HashTable_t;

HashTable_t bulletinBoard = {.counter = 0};
*/

// after


__thread int64_t lastTime = 0;
__thread int64_t storeLastTime = 0;
__thread uint64_t writtenBytes = 0;
__thread uint64_t loadedBytes = 0;
__thread uint64_t usedBytes = 0;
__thread uint64_t deadBytes = 0;
__thread uint64_t oldBytes = 0;
__thread uint64_t oldAppxBytes = 0;
__thread uint64_t newBytes = 0;
__thread uint64_t accessedIns = 0;
__thread uint64_t falseWWIns = 0;
__thread uint64_t falseWRIns = 0;
__thread uint64_t falseRWIns = 0;
__thread uint64_t trueWWIns = 0;
__thread uint64_t trueWRIns = 0;
__thread uint64_t trueRWIns = 0;
__thread uint64_t reuse = 0;

// Some stats
__thread long int correct=0;
__thread long int incorrect=0;
__thread long int st1=0;
__thread long int ld1=0;
__thread long int mix1=0;
__thread long int unk1=0;
__thread long int st2=0;
__thread long int ld2=0;
__thread long int mix2=0;
__thread long int unk2=0;
__thread long difffunc=0;
__thread long samefunc=0;
__thread long unknwfunc=0;
__thread long ipSame=0;
__thread long ipDiff=0;

// stats for consecutiveness fixup

__thread void * last_trapped_cacheline;
__thread bool did_last_sample_trap;
__thread long consecutive_access_count = 0;
__thread int consecutive_wasted_trap_count = 0;
__thread int ts_unique_consecutive_communication_count = 0;
__thread int fs_unique_consecutive_communication_count = 0;
__thread int unique_consecutive_communication_count = 0;

// before
__thread double ts_prev_values[50];
__thread double fs_prev_values[50];
__thread double prev_values[50];
__thread double consecutive_new_values_untweaked[50];
__thread double ts_consecutive_new_values_untweaked[50];
__thread double fs_consecutive_new_values_untweaked[50];
__thread int consecutive_indices_unique[100];
__thread int ts_consecutive_indices_unique[100];
__thread int fs_consecutive_indices_unique[100];
// after

/******************************************************************************
 * sample source registration
 *****************************************************************************/

// Support for derived events (proxy sampling).
//static int derived[MAX_EVENTS];
//static int some_overflow;


/******************************************************************************
 * method functions
 *****************************************************************************/

static WPTriggerActionType DeadStoreWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt);
static WPTriggerActionType RedStoreWPCallback(WatchPointInfo_t *wpi, int startOffseti, int safeAccessLen, WatchPointTrigger_t * wt);
static WPTriggerActionType TemporalReuseWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt);
static WPTriggerActionType SpatialReuseWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt);
static WPTriggerActionType LoadLoadWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt);
static WPTriggerActionType FalseSharingWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt);
static WPTriggerActionType AllSharingWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt);
static WPTriggerActionType TrueSharingWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt);
static WPTriggerActionType IPCFalseSharingWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt);
static WPTriggerActionType IPCAllSharingWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt);
static WPTriggerActionType IPCTrueSharingWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt);


static WpClientConfig_t wpClientConfig[] = {
    /**** DeadSpy ***/
    {
        .id = WP_DEADSPY,
        .name = WP_DEADSPY_EVENT_NAME,
        .wpCallback = DeadStoreWPCallback,
        .preWPAction = DISABLE_WP,
        .configOverrideCallback = NULL
    },
    /**** RedSpy ***/
    {
        .id = WP_REDSPY,
        .name = WP_REDSPY_EVENT_NAME,
        .wpCallback = RedStoreWPCallback,
        .preWPAction = DISABLE_WP,
        .configOverrideCallback = RedSpyWPConfigOverride
    },
    /**** LoadSpy ***/
    {
        .id = WP_LOADSPY,
        .name = WP_LOADSPY_EVENT_NAME,
        .wpCallback = LoadLoadWPCallback,
        .preWPAction = DISABLE_WP,
        .configOverrideCallback = LoadSpyWPConfigOverride
    },
    /**** Temporal Reuse ***/
    {
        .id = WP_TEMPORAL_REUSE,
        .name = WP_TEMPORAL_REUSE_EVENT_NAME,
        .wpCallback = TemporalReuseWPCallback,
        .preWPAction = DISABLE_WP,
        .configOverrideCallback = TemporalReuseWPConfigOverride
    },
    /**** Spatial Reuse ***/
    {
        .id = WP_SPATIAL_REUSE,
        .name = WP_SPATIAL_REUSE_EVENT_NAME,
        .wpCallback = SpatialReuseWPCallback,
        .preWPAction = DISABLE_WP,
        .configOverrideCallback = SpatialReuseWPConfigOverride
    },
    /**** False Sharing ***/
    {
        .id = WP_FALSE_SHARING,
        .name = WP_FALSE_SHARING_EVENT_NAME,
        .wpCallback = FalseSharingWPCallback,
        .preWPAction = DISABLE_ALL_WP,
        .configOverrideCallback = FalseSharingWPConfigOverride
    },
    /**** All Sharing ***/
    {
        .id = WP_ALL_SHARING,
        .name = WP_ALL_SHARING_EVENT_NAME,
        .wpCallback = AllSharingWPCallback,
        .preWPAction = DISABLE_ALL_WP,
        .configOverrideCallback = AllSharingWPConfigOverride
    },
    {    
        .id = WP_COMDETECTIVE,
        .name = WP_COMDETECTIVE_EVENT_NAME,
        .wpCallback = ComDetectiveWPCallback,
        .preWPAction = DISABLE_ALL_WP,
        .configOverrideCallback = ComDetectiveWPConfigOverride
    },
    /**** Contention ***/
    {
        .id = WP_TRUE_SHARING,
        .name = WP_TRUE_SHARING_EVENT_NAME,
        .wpCallback = TrueSharingWPCallback,
        .preWPAction = DISABLE_WP,
        .configOverrideCallback = TrueSharingWPConfigOverride
    },
    /**** IPC False Sharing ***/
    {
        .id = WP_IPC_FALSE_SHARING,
        .name = WP_IPC_FALSE_SHARING_EVENT_NAME,
        .wpCallback = IPCFalseSharingWPCallback,
        .preWPAction = DISABLE_ALL_WP,
        .configOverrideCallback = IPCFalseSharingWPConfigOverride
    },
    /**** IPC All Sharing ***/
    {
        .id = WP_IPC_ALL_SHARING,
        .name = WP_IPC_ALL_SHARING_EVENT_NAME,
        .wpCallback = IPCAllSharingWPCallback,
        .preWPAction = DISABLE_ALL_WP,
        .configOverrideCallback = IPCAllSharingWPConfigOverride
    },
    /**** IPC Contention ***/
    {
        .id = WP_IPC_TRUE_SHARING,
        .name = WP_IPC_TRUE_SHARING_EVENT_NAME,
        .wpCallback = IPCTrueSharingWPCallback,
        .preWPAction = DISABLE_WP,
        .configOverrideCallback = IPCTrueSharingWPConfigOverride
    }
    
};


static WpClientConfig_t * theWPConfig = NULL;

bool WatchpointClientActive(){
    return theWPConfig != NULL;
}

#define MAX_BLACK_LIST_ADDRESS (1024)

typedef struct BlackListAddressRange{
    void * startAddr;
    void * endAddr;
}BlackListAddressRange_t;
static BlackListAddressRange_t blackListAddresses [MAX_BLACK_LIST_ADDRESS];
static uint16_t numBlackListAddresses = 0;

static const char * blackListedModules[] = {"libmonitor.so", "libhpcrun.so", "libpfm.so", "libxed.so", "libpapi.so", "anon_inode:[perf_event]"};
static const int  numblackListedModules = 6;
static spinlock_t blackListLock = SPINLOCK_UNLOCKED;



static void PopulateBlackListAddresses() {
    spinlock_lock(&blackListLock);
    if(numBlackListAddresses == 0) {
        FILE* loadmap = fopen("/proc/self/maps", "r");
        if (! loadmap) {
            EMSG("Could not open /proc/self/maps");
            return;
        }
        char linebuf[1024 + 1];
        char tmpname[PATH_MAX];
        char* addr = NULL;
        for(;;) {
            char* l = fgets(linebuf, sizeof(linebuf), loadmap);
            if (feof(loadmap)) break;
            char* save = NULL;
            const char delim[] = " \n";
            addr = strtok_r(l, delim, &save);
            char* perms = strtok_r(NULL, delim, &save);
            // skip 3 tokens
            for (int i=0; i < 3; i++) { (void) strtok_r(NULL, delim, &save);}
            char* name = strtok_r(NULL, delim, &save);
            realpath(name, tmpname);
            for(int i = 0; i < numblackListedModules; i++) {
                if (strstr(tmpname, blackListedModules[i])){
                    char* save = NULL;
                    const char dash[] = "-";
                    char* start_str = strtok_r(addr, dash, &save);
                    char* end_str   = strtok_r(NULL, dash, &save);
                    void *start = (void*) (uintptr_t) strtol(start_str, NULL, 16);
                    void *end   = (void*) (uintptr_t) strtol(end_str, NULL, 16);
                    blackListAddresses[numBlackListAddresses].startAddr = start;
                    blackListAddresses[numBlackListAddresses].endAddr = end;
                    numBlackListAddresses++;
                }
            }
        }
        fclose(loadmap);
        // No TLS
        extern void * __tls_get_addr (void *);
        blackListAddresses[numBlackListAddresses].startAddr = ((void *)__tls_get_addr) - 1000 ;
        blackListAddresses[numBlackListAddresses].endAddr = ((void *)__tls_get_addr) + 1000;
        numBlackListAddresses++;

        // No first page
        blackListAddresses[numBlackListAddresses].startAddr = 0 ;
        blackListAddresses[numBlackListAddresses].endAddr = (void*) sysconf(_SC_PAGESIZE);
        numBlackListAddresses++;
    }
    spinlock_unlock(&blackListLock);
}


static void
METHOD_FN(init)
{
    self->state = INIT;
}

static void
METHOD_FN(thread_init)
{
    TMSG(WATCHPOINT, "thread init");
    TMSG(WATCHPOINT, "thread init OK");
}

static void
METHOD_FN(thread_init_action)
{
    TMSG(WATCHPOINT, "register thread");
    wpStats.numImpreciseSamples = 0;
    wpStats.numWatchpointsSet = 0;
    WatchpointThreadInit(theWPConfig->wpCallback);
    TMSG(WATCHPOINT, "register thread ok");
}

static void
METHOD_FN(start)
{
    thread_data_t* td = hpcrun_get_thread_data();
    source_state_t my_state = TD_GET(ss_state)[self->sel_idx];
    
    if (my_state == START) {
        TMSG(WATCHPOINT,"*NOTE* WATCHPOINT start called when already in state START");
        return;
    }
    td->ss_state[self->sel_idx] = START;
}

static void ClientTermination(){
    // Cleanup the watchpoint data
    hpcrun_stats_num_samples_imprecise_inc(wpStats.numImpreciseSamples);
    hpcrun_stats_num_watchpoints_set_inc(wpStats.numWatchpointsSet);
    WatchpointThreadTerminate();
    switch (theWPConfig->id) {
        case WP_DEADSPY:
            hpcrun_stats_num_writtenBytes_inc(writtenBytes);
            hpcrun_stats_num_usedBytes_inc(usedBytes);
            hpcrun_stats_num_deadBytes_inc(deadBytes);
            break;
        case WP_REDSPY:
            hpcrun_stats_num_writtenBytes_inc(writtenBytes);
            hpcrun_stats_num_newBytes_inc(newBytes);
            hpcrun_stats_num_oldBytes_inc(oldBytes);
            hpcrun_stats_num_oldAppxBytes_inc(oldAppxBytes);
            break;
        case WP_LOADSPY:
            hpcrun_stats_num_loadedBytes_inc(loadedBytes);
            hpcrun_stats_num_newBytes_inc(newBytes);
            hpcrun_stats_num_oldBytes_inc(oldBytes);
            hpcrun_stats_num_oldAppxBytes_inc(oldAppxBytes);
            break;
        case WP_TEMPORAL_REUSE:
            hpcrun_stats_num_accessedIns_inc(accessedIns);
            hpcrun_stats_num_reuse_inc(reuse);
            break;
        case WP_SPATIAL_REUSE:
            hpcrun_stats_num_accessedIns_inc(accessedIns);
            hpcrun_stats_num_reuse_inc(reuse);
            break;
        case WP_FALSE_SHARING:
	//printf("terminated?\n");
	    /*printf("number of samples: %d, in thread: %d\n",number_of_sample, TD_GET(core_profile_trace_data.id));
	    printf("number of load samples: %d, in thread: %d\n",number_of_load_sample, TD_GET(core_profile_trace_data.id));
	    printf("number of store samples: %d, in thread: %d\n",number_of_store_sample, TD_GET(core_profile_trace_data.id));
	    printf("number of load_and_store samples: %d, in thread: %d\n",number_of_load_store_sample, TD_GET(core_profile_trace_data.id));
	    printf("number of load_and_store samples by all loads: %ld, in thread: %d\n", number_of_load_store_sample_all_loads, TD_GET(core_profile_trace_data.id));
	    printf("number of load_and_store samples by all stores: %ld, in thread: %d\n", number_of_load_store_sample_all_stores, TD_GET(core_profile_trace_data.id));
	    printf("number of arming: %d, in thread: %d\n",number_of_arming, TD_GET(core_profile_trace_data.id));
	    printf("number of residues: %d, in thread: %d\n",number_of_residues, TD_GET(core_profile_trace_data.id));
	    printf("number of caught traps: %d, in thread: %d\n",number_of_caught_traps, TD_GET(core_profile_trace_data.id));
	    printf("number of caught read traps: %d, in thread: %d\n",number_of_caught_read_traps, TD_GET(core_profile_trace_data.id));
	    printf("number of caught write traps: %d, in thread: %d\n",number_of_caught_write_traps, TD_GET(core_profile_trace_data.id));
	    printf("number of caught read write traps: %d, in thread: %d\n",number_of_caught_read_write_traps, TD_GET(core_profile_trace_data.id));
	    printf("number of bulletin board updates: %d, in thread: %d\n", number_of_bulletin_board_updates, TD_GET(core_profile_trace_data.id));
	    printf("number of bulletin board updates before: %d, in thread: %d\n", number_of_bulletin_board_updates_before, TD_GET(core_profile_trace_data.id));*/
        case WP_IPC_FALSE_SHARING:
            hpcrun_stats_num_accessedIns_inc(accessedIns);
            hpcrun_stats_num_falseWRIns_inc(falseWWIns);
            hpcrun_stats_num_falseRWIns_inc(falseRWIns);
            hpcrun_stats_num_falseWRIns_inc(falseWRIns);
            break;
        case WP_TRUE_SHARING:
        case WP_IPC_TRUE_SHARING:
            hpcrun_stats_num_accessedIns_inc(accessedIns);
            hpcrun_stats_num_trueWWIns_inc(trueWWIns);
            hpcrun_stats_num_trueRWIns_inc(trueRWIns);
            hpcrun_stats_num_trueWRIns_inc(trueWRIns);
            break;
        case WP_ALL_SHARING:
	case WP_COMDETECTIVE:
        case WP_IPC_ALL_SHARING: /*assert(0);
            assert(0);*/
            hpcrun_stats_num_accessedIns_inc(accessedIns);
            hpcrun_stats_num_falseWRIns_inc(falseWWIns);
            hpcrun_stats_num_falseRWIns_inc(falseRWIns);
            hpcrun_stats_num_falseWRIns_inc(falseWRIns);
            hpcrun_stats_num_trueWWIns_inc(trueWWIns);
            hpcrun_stats_num_trueRWIns_inc(trueRWIns);
            hpcrun_stats_num_trueWRIns_inc(trueWRIns);
        default:
            break;
    }
}

static void
METHOD_FN(thread_fini_action)
{
    TMSG(WATCHPOINT, "unregister thread");
}

#define N 100000
cct_node_t *topNNode[N]={NULL};

static void
TopN(cct_node_t* node, cct_op_arg_t arg, size_t level)
{
    int i, t;
    uint64_t min;
    int metricID = (int)arg;
    if (node) {
        metric_set_t *set = hpcrun_get_metric_set(node);
        if (!set) return;
        hpcrun_metricVal_t *loc = hpcrun_metric_set_loc(set, metricID);
        if (!loc) return;
        
        uint64_t val = loc->i;
        if (val == 0) return;
        
        for (i=0; i<N; i++) {
            if (!topNNode[i]) {
                topNNode[i] = node;
                break;
            }
        }
        // if no empty slot
        if (i == N) {
            min = ULLONG_MAX;
            for (i=0; i<N; i++) {
                metric_set_t *seti = hpcrun_get_metric_set(topNNode[i]);
                hpcrun_metricVal_t *loci = hpcrun_metric_set_loc(seti, metricID);
                if (loci->i < min) {
                    t = i;
                    min = loci->i;
                }
            }
            if (val > min) topNNode[t] = node;
        }
    }
}

static void
PrintTopN(int metricID)
{
    FILE *fd;
    char default_path[PATH_MAX];
    thread_data_t *td = hpcrun_get_thread_data();
    cct_node_t *root = td->core_profile_trace_data.epoch->csdata.tree_root;
    //TODO: partial? cct_node_t *partial = td->core_profile_trace_data.epoch->csdata.partial_unw_root;
    
    // trave root first and then partial second
    hpcrun_cct_walk_node_1st(root, TopN, (void *) metricID);
    
    int i, j;
    for (i=0; i<N; i++) {
        cct_node_t *node1 = topNNode[i];
        if (!node1) goto end;
        metric_set_t *set1 = hpcrun_get_metric_set(node1);
        hpcrun_metricVal_t *loc1 = hpcrun_metric_set_loc(set1, metricID);
        uint64_t val1 = loc1->i;
        for (j = i+1; j<N; j++) {
            cct_node_t *node2 = topNNode[j];
            if (!node2) break;
            metric_set_t *set2 = hpcrun_get_metric_set(node2);
            hpcrun_metricVal_t *loc2 = hpcrun_metric_set_loc(set2, metricID);
            uint64_t val2 = loc2->i;
            
            if (val2 > val1) {
                cct_node_t *tmp = topNNode[i];
                topNNode[i] = topNNode[j];
                topNNode[j] = tmp;
                val1 = val2;
            }
        }
    }
end:
    ;
    char *path = getenv(HPCRUN_OUT_PATH);
    if (path == NULL || strlen(path) == 0) {
        sprintf(default_path, "./hpctoolkit-%s-measurements", hpcrun_files_executable_name());
        path = default_path;
    }
    sprintf(path, "%s/%s", path, "topN.log");
    
    fd = fopen(path, "a+");
    
    int libmonitorId, libhpcrunId;
    // print loadmodule info first
    fprintf (fd, "<LOADMODULES>\n");
    hpcrun_loadmap_t *current_loadmap = td->core_profile_trace_data.epoch->loadmap;
    for (load_module_t* lm_src = current_loadmap->lm_end;
         (lm_src); lm_src = lm_src->prev)
    {
        if (strstr(lm_src->name, "libmonitor")) libmonitorId = lm_src->id;
        if (strstr(lm_src->name, "libhpcrun")) libhpcrunId = lm_src->id;
        fprintf(fd, "%d:%p:%s\n", lm_src->id, (void*) lm_src->dso_info->start_to_ref_dist, lm_src->name);
    }
    fprintf (fd, "</LOADMODULES>\n");
    fprintf (fd, "<TOPN>\n");
    // sort the top N from high to low
    for (i=0; i<N; i++) {
        cct_node_t *node = topNNode[i];
        if (!node) break;
        metric_set_t *set = hpcrun_get_metric_set(node);
        hpcrun_metricVal_t *loc = hpcrun_metric_set_loc(set, metricID);
        uint64_t val = loc->i;
        fprintf(fd, "%lu:%lf:", val, (double)val/deadBytes);
        //FIXME: +1 not needed
        fprintf(fd, "%d-%p", hpcrun_cct_addr(node)->ip_norm.lm_id, (void*) (hpcrun_cct_addr(node)->ip_norm.lm_ip+1));
        node = hpcrun_cct_parent(node);
        bool lastWasSeparator=false;
        while (node) {
            if (hpcrun_cct_addr(node)->ip_norm.lm_id == 0){
                break;
            }
            if (hpcrun_cct_addr(node)->ip_norm.lm_id !=  libmonitorId && hpcrun_cct_addr(node)->ip_norm.lm_id != libhpcrunId) {
                // if last node was a separator, +1 here
                if( lastWasSeparator ) {
                    //FIXME: +1 not needed
                    fprintf(fd, ",%d-%p", hpcrun_cct_addr(node)->ip_norm.lm_id, (void*)(hpcrun_cct_addr(node)->ip_norm.lm_ip+1));
                    lastWasSeparator = false;
                } else
                    fprintf(fd, ",%d-%p", hpcrun_cct_addr(node)->ip_norm.lm_id, (void*) hpcrun_cct_addr(node)->ip_norm.lm_ip);
            }
            else if (hpcrun_cct_addr(node)->ip_norm.lm_id == libhpcrunId) {
                fprintf(fd, ",SEP");
                lastWasSeparator = true;
            } else ;
            node = hpcrun_cct_parent(node);
        }
        fprintf(fd, "\n");
    }
    fprintf (fd, "</TOPN>\n");
    fclose (fd);
}

static void
METHOD_FN(stop)
{
    TMSG(WATCHPOINT, "stop");
    //thread_data_t *td = hpcrun_get_thread_data();
    //int nevents = self->evl.nevents;
    source_state_t my_state = TD_GET(ss_state)[self->sel_idx];
    
    if (my_state == STOP) {
        TMSG(WATCHPOINT,"*NOTE* WATCHPOINT stop called when already in state STOP");
        return;
    }
    
    if (my_state != START) {
        TMSG(WATCHPOINT,"*WARNING* WATCHPOINT stop called when not in state START");
        return;
    }
    
    ClientTermination();
    
    if (ENABLED(PRINTTOPN))
        PrintTopN(dead_metric_id);
    
    TD_GET(ss_state)[self->sel_idx] = STOP;
}

static void
METHOD_FN(shutdown)
{
    TMSG(WATCHPOINT, "shutdown");
    
    METHOD_CALL(self, stop); // make sure stop has been called
    self->state = UNINIT;
}

// Return true if WATCHPOINT recognizes the name, whether supported or not.
// We'll handle unsupported events later.
static bool
METHOD_FN(supports_event, const char *ev_str)
{
    for(int i = 0; i < WP_MAX_CLIENTS; i++) {
        if (hpcrun_ev_is(ev_str, wpClientConfig[i].name))
            return true;
    }
    return false;
}

static inline void SetUpFalseSharingMetrics(){
    false_ww_metric_id = hpcrun_new_metric();
    hpcrun_set_metric_info_and_period(false_ww_metric_id, "FALSE_WW_CONFLICT", MetricFlags_ValFmt_Int, 1, metric_property_none);
    false_rw_metric_id = hpcrun_new_metric();
    hpcrun_set_metric_info_and_period(false_rw_metric_id, "FALSE_RW_CONFLICT", MetricFlags_ValFmt_Int, 1, metric_property_none);
    false_wr_metric_id = hpcrun_new_metric();
    hpcrun_set_metric_info_and_period(false_wr_metric_id, "FALSE_WR_CONFLICT", MetricFlags_ValFmt_Int, 1, metric_property_none);
}
static inline void SetUpTrueSharingMetrics(){
    true_ww_metric_id = hpcrun_new_metric();
    hpcrun_set_metric_info_and_period(true_ww_metric_id, "TRUE_WW_CONFLICT", MetricFlags_ValFmt_Int, 1, metric_property_none);
    true_rw_metric_id = hpcrun_new_metric();
    hpcrun_set_metric_info_and_period(true_rw_metric_id, "TRUE_RW_CONFLICT", MetricFlags_ValFmt_Int, 1, metric_property_none);
    true_wr_metric_id = hpcrun_new_metric();
    hpcrun_set_metric_info_and_period(true_wr_metric_id, "TRUE_WR_CONFLICT", MetricFlags_ValFmt_Int, 1, metric_property_none);
}

static void
METHOD_FN(process_event_list, int lush_metrics)
{
    // Only one WP client can be active at a time
    if (theWPConfig) {
        EEMSG("Only one watchpoint client can be active at a time \n");
        monitor_real_abort();
    }
    char* evlist = METHOD_CALL(self, get_event_str);
    char* event = start_tok(evlist);
    
    // only one supported
    for(int i = 0; i < WP_MAX_CLIENTS; i++) {
        if (hpcrun_ev_is(event, wpClientConfig[i].name)) {
            theWPConfig  = &wpClientConfig[i];
            break;
        }
    }
    
    wpStats.numImpreciseSamples = 0;
    wpStats.numWatchpointsSet = 0;
    WatchpointThreadInit(theWPConfig->wpCallback);
    
    if(theWPConfig->configOverrideCallback){
        theWPConfig->configOverrideCallback(0);
    }
    
    PopulateBlackListAddresses();
    
    switch (theWPConfig->id) {
        case WP_DEADSPY:
            measured_metric_id = hpcrun_new_metric();
            hpcrun_set_metric_info_and_period(measured_metric_id, "BYTES_USED", MetricFlags_ValFmt_Int, 1, metric_property_none);
            dead_metric_id = hpcrun_new_metric();
            hpcrun_set_metric_info_and_period(dead_metric_id, "BYTES_DEAD", MetricFlags_ValFmt_Int, 1, metric_property_none);
            break;
            
        case WP_REDSPY:
        case WP_LOADSPY:
            measured_metric_id = hpcrun_new_metric();
            hpcrun_set_metric_info_and_period(measured_metric_id, "BYTES_NEW", MetricFlags_ValFmt_Int, 1, metric_property_none);
            red_metric_id = hpcrun_new_metric();
            hpcrun_set_metric_info_and_period(red_metric_id, "BYTES_RED", MetricFlags_ValFmt_Int, 1, metric_property_none);
            redApprox_metric_id = hpcrun_new_metric();
            hpcrun_set_metric_info_and_period(redApprox_metric_id, "BYTES_RED_APPROX", MetricFlags_ValFmt_Int, 1, metric_property_none);
            break;
            
        case WP_TEMPORAL_REUSE:
            temporal_metric_id = hpcrun_new_metric();
            hpcrun_set_metric_info_and_period(temporal_metric_id, "TEMPORAL", MetricFlags_ValFmt_Int, 1, metric_property_none);
            break;
            
        case WP_SPATIAL_REUSE:
            spatial_metric_id = hpcrun_new_metric();
            hpcrun_set_metric_info_and_period(spatial_metric_id, "SPATIAL", MetricFlags_ValFmt_Int, 1, metric_property_none);
            break;
            
        case WP_ALL_SHARING:
        case WP_COMDETECTIVE:
	case WP_IPC_ALL_SHARING:
            // must have a canonical load map across processes
            hpcrun_set_ipc_load_map(true);
            measured_metric_id = hpcrun_new_metric();
            hpcrun_set_metric_info_and_period(measured_metric_id, "MONITORED", MetricFlags_ValFmt_Int, 1, metric_property_none);
            SetUpFalseSharingMetrics();
            SetUpTrueSharingMetrics();
            break;
            
        case WP_FALSE_SHARING:
        case WP_IPC_FALSE_SHARING:
            // must have a canonical load map across processes
            hpcrun_set_ipc_load_map(true);
            measured_metric_id = hpcrun_new_metric();
            hpcrun_set_metric_info_and_period(measured_metric_id, "MONITORED", MetricFlags_ValFmt_Int, 1, metric_property_none);
            SetUpFalseSharingMetrics();
            break;
            
        case WP_TRUE_SHARING:
        case WP_IPC_TRUE_SHARING:
            // must have a canonical load map across processes
            hpcrun_set_ipc_load_map(true);
            measured_metric_id = hpcrun_new_metric();
            hpcrun_set_metric_info_and_period(measured_metric_id, "MONITORED", MetricFlags_ValFmt_Int, 1, metric_property_none);
            SetUpTrueSharingMetrics();
            break;
            
        default:
            break;
    }
}

static void
METHOD_FN(gen_event_set, int lush_metrics)
{
}

static void
METHOD_FN(display_events)
{
    printf("===========================================================================\n");
    printf("Watchpoint events\n");
    printf("---------------------------------------------------------------------------\n");
    printf("%s\n", WP_DEADSPY_EVENT_NAME);
    printf("---------------------------------------------------------------------------\n");
    printf("%s\n", WP_REDSPY_EVENT_NAME);
    printf("---------------------------------------------------------------------------\n");
    printf("%s\n", WP_LOADSPY_EVENT_NAME);
    printf("---------------------------------------------------------------------------\n");
    printf("%s\n", WP_TEMPORAL_REUSE_EVENT_NAME);
    printf("---------------------------------------------------------------------------\n");
    printf("%s\n", WP_SPATIAL_REUSE_EVENT_NAME);
    printf("---------------------------------------------------------------------------\n");
    printf("%s\n", WP_FALSE_SHARING_EVENT_NAME);
    printf("---------------------------------------------------------------------------\n");
    printf("%s\n", WP_TRUE_SHARING_EVENT_NAME);
    printf("---------------------------------------------------------------------------\n");
    printf("%s\n", WP_ALL_SHARING_EVENT_NAME);
    printf("---------------------------------------------------------------------------\n");
    printf("%s\n", WP_COMDETECTIVE_EVENT_NAME);
    printf("---------------------------------------------------------------------------\n");
    printf("%s\n", WP_IPC_FALSE_SHARING_EVENT_NAME);
    printf("---------------------------------------------------------------------------\n");
    printf("%s\n", WP_IPC_TRUE_SHARING_EVENT_NAME);
    printf("---------------------------------------------------------------------------\n");
    printf("%s\n", WP_IPC_ALL_SHARING_EVENT_NAME);
    printf("===========================================================================\n");
    printf("\n");
}


/***************************************************************************
 * object
 ***************************************************************************/

#define ss_name witch
#define ss_cls SS_HARDWARE

#include "ss_obj.h"

// **************************************************************************
// * public operations
// **************************************************************************

/******************************************************************************
 * private operations
 *****************************************************************************/

enum JoinNodeType {
    E_KILLED=0,
    E_USED,
    E_NEW_VAL,
    E_TEPORALLY_REUSED,
    E_SPATIALLY_REUSED,
    E_TRUE_WW_SHARE,
    E_TRUE_WR_SHARE,
    E_TRUE_RW_SHARE,
    E_FALSE_WW_SHARE,
    E_FALSE_WR_SHARE,
    E_FALSE_RW_SHARE,
    E_IPC_TRUE_WW_SHARE,
    E_IPC_TRUE_WR_SHARE,
    E_IPC_TRUE_RW_SHARE,
    E_IPC_FALSE_WW_SHARE,
    E_IPC_FALSE_WR_SHARE,
    E_IPC_FALSE_RW_SHARE,
    E_INVALID_JOIN_NODE_TYPE
};

enum JoinNodeIdx {
    E_ACCURATE_JOIN_NODE_IDX=0,
    E_INACCURATE_JOIN_NODE_IDX=1,
};

static void KILLED_BY(void) {}
static void KILLED_BY_INACCURATE_PC(void) {}

static void USED_BY(void) {}
static void USED_BY_INACCURATE_PC(void) {}

static void NEW_VAL_BY(void) {}
static void NEW_VAL_BY_INACCURATE_PC(void) {}

static void TEPORALLY_REUSED_BY(void) {}
static void TEPORALLY_REUSED_BY_INACCURATE_PC(void) {}

static void SPATIALLY_REUSED_BY(void) {}
static void SPATIALLY_REUSED_BY_INACCURATE_PC(void) {}

static void TRUE_WW_SHARE(void) {}
static void TRUE_WW_SHARE_INACCURATE_PC(void) {}

static void TRUE_WR_SHARE(void) {}
static void TRUE_WR_SHARE_INACCURATE_PC(void) {}

static void TRUE_RW_SHARE(void) {}
static void TRUE_RW_SHARE_INACCURATE_PC(void) {}

static void FALSE_WW_SHARE(void) {}
static void FALSE_WW_SHARE_INACCURATE_PC(void) {}

static void FALSE_WR_SHARE(void) {}
static void FALSE_WR_SHARE_INACCURATE_PC(void) {}

static void FALSE_RW_SHARE(void) {}
static void FALSE_RW_SHARE_INACCURATE_PC(void) {}

static void IPC_TRUE_WW_SHARE(void) {}
static void IPC_TRUE_WW_SHARE_INACCURATE_PC(void) {}

static void IPC_TRUE_WR_SHARE(void) {}
static void IPC_TRUE_WR_SHARE_INACCURATE_PC(void) {}

static void IPC_TRUE_RW_SHARE(void) {}
static void IPC_TRUE_RW_SHARE_INACCURATE_PC(void) {}

static void IPC_FALSE_WW_SHARE(void) {}
static void IPC_FALSE_WW_SHARE_INACCURATE_PC(void) {}

static void IPC_FALSE_WR_SHARE(void) {}
static void IPC_FALSE_WR_SHARE_INACCURATE_PC(void) {}

static void IPC_FALSE_RW_SHARE(void) {}
static void IPC_FALSE_RW_SHARE_INACCURATE_PC(void) {}

// Create a 2D array of Join node functions
#define GET_FUN_ADDR(a) {(&a + 1), (&(a ## _INACCURATE_PC) +1)}

static const void * joinNodes[][2] = {
    [E_KILLED] = GET_FUN_ADDR(KILLED_BY),
    [E_USED] = GET_FUN_ADDR(USED_BY),
    [E_NEW_VAL] = GET_FUN_ADDR(NEW_VAL_BY),
    [E_TEPORALLY_REUSED] = GET_FUN_ADDR(TEPORALLY_REUSED_BY),
    [E_SPATIALLY_REUSED] = GET_FUN_ADDR(SPATIALLY_REUSED_BY),
    [E_TRUE_WW_SHARE] = GET_FUN_ADDR(TRUE_WW_SHARE),
    [E_TRUE_WR_SHARE] = GET_FUN_ADDR(TRUE_WR_SHARE),
    [E_TRUE_RW_SHARE] = GET_FUN_ADDR(TRUE_RW_SHARE),
    [E_FALSE_WW_SHARE] = GET_FUN_ADDR(FALSE_WW_SHARE),
    [E_FALSE_WR_SHARE] = GET_FUN_ADDR(FALSE_WR_SHARE),
    [E_FALSE_RW_SHARE] = GET_FUN_ADDR(FALSE_RW_SHARE),
    [E_IPC_TRUE_WW_SHARE] = GET_FUN_ADDR(IPC_TRUE_WW_SHARE),
    [E_IPC_TRUE_WR_SHARE] = GET_FUN_ADDR(IPC_TRUE_WR_SHARE),
    [E_IPC_TRUE_RW_SHARE] = GET_FUN_ADDR(IPC_TRUE_RW_SHARE),
    [E_IPC_FALSE_WW_SHARE] = GET_FUN_ADDR(IPC_FALSE_WW_SHARE),
    [E_IPC_FALSE_WR_SHARE] = GET_FUN_ADDR(IPC_FALSE_WR_SHARE),
    [E_IPC_FALSE_RW_SHARE] = GET_FUN_ADDR(IPC_FALSE_RW_SHARE)
};

static inline int GetMatchingWatermarkId(int pebsMetricId){
    // Get the correct watermark_metric_id
    for (int i=0; i<NUM_WATERMARK_METRICS; i++) {
        if(pebs_metric_id[i] == pebsMetricId) {
            return watermark_metric_id[i];
        }
    }
    assert(0);
}

static inline uint64_t GetWeightedMetricDiffAndReset(cct_node_t * ctxtNode, int pebsMetricId, double proportion){
    assert(ctxtNode);
    metric_set_t* set = hpcrun_get_metric_set(ctxtNode);
    cct_metric_data_t diffWithPeriod;
    cct_metric_data_t diff;
    int catchUpMetricId = GetMatchingWatermarkId(pebsMetricId);
    hpcrun_get_weighted_metric_diff(pebsMetricId, catchUpMetricId, set, &diff, &diffWithPeriod);
    // catch up metric: up catchUpMetricId to macth pebsMetricId proportionally
    diff.r = diff.r * proportion;
    cct_metric_data_increment(catchUpMetricId, ctxtNode, diff);
    return (uint64_t) (diffWithPeriod.r * proportion);
}

static void UpdateFoundMetrics(cct_node_t * ctxtNode, cct_node_t * oldNode, void * joinNode, int foundMetric, int foundMetricInc){
    // insert a special node
    cct_node_t *node = hpcrun_insert_special_node(oldNode, joinNode);
    // concatenate call paths
    node = hpcrun_cct_insert_path_return_leaf(ctxtNode, node);
    // update the foundMetric
    cct_metric_data_increment(foundMetric, node, (cct_metric_data_t){.i = foundMetricInc});
}


#define SAMPLE_NO_INC ((hpcrun_metricVal_t){.i=0})
#define SAMPLE_UNIT_INC ((hpcrun_metricVal_t){.i=1})

static cct_node_t * UpdateMetrics(void *ctxt, cct_node_t * oldNode, void * joinNode, int checkedMetric, int foundMetric, int checkedMetricInc, int foundMetricInc){
    // unwind call stack once
    sample_val_t v = hpcrun_sample_callpath(ctxt, checkedMetric, (hpcrun_metricVal_t){.i=checkedMetricInc}, 0/*skipInner*/, 1/*isSync*/, NULL);
    if(foundMetricInc) {
        UpdateFoundMetrics(v.sample_node, oldNode, joinNode, foundMetric, foundMetricInc);
    }
    return v.sample_node;
}

static inline void UpdateConcatenatedPathPair(void *ctxt, cct_node_t * oldNode, const void * joinNode, int metricId, uint64_t metricInc){
    // unwind call stack once
    sample_val_t v = hpcrun_sample_callpath(ctxt, metricId, SAMPLE_NO_INC, 0/*skipInner*/, 1/*isSync*/, NULL);
    // insert a special node
    cct_node_t *node = hpcrun_insert_special_node(oldNode, joinNode);
    // concatenate call paths
    node = hpcrun_cct_insert_path_return_leaf(v.sample_node, node);
    // update the foundMetric
    cct_metric_data_increment(metricId, node, (cct_metric_data_t){.i = metricInc});
}


static WPTriggerActionType DeadStoreWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt){
    if(!wt->pc) {
        // if the ip is 0, let's drop the WP
        return ALREADY_DISABLED;
    }
    
    // This is a approximation.
    // If we took N samples at wpi->sample.node since the last time a WP triggered here,
    // If this a dead write, we'll update the dead_writes metric at the call path <wpi->sample.node:KILLED_BY:curctxt>
    // Otherwise (not dead), we'll update the used_writes metric at the call path <wpi->sample.node:USED_BY:curctxt>
    // In either case, the increment will be (N * overlapBytes)
    // Bump up watermark_metric_id to match sampledMetricId
    
    double myProportion = ProportionOfWatchpointAmongOthersSharingTheSameContext(wpi);
    uint64_t numDiffSamples = GetWeightedMetricDiffAndReset(wpi->sample.node, wpi->sample.sampledMetricId, myProportion);
    int overlapBytes = GET_OVERLAP_BYTES(wpi->sample.va, wpi->sample.wpLength, wt->va, wt->accessLength);
    if(overlapBytes <= 0){
        fprintf(stderr, "\n wpi->sample.va=%p, wpi->sample.wpLength = %d,  wt->va = %p, wt->accessLength=%d\n", wpi->sample.va, wpi->sample.wpLength, wt->va, wt->accessLength);
        monitor_real_abort();
    }
    
    // Now increment dead_metric_id by numDiffSamples * wpi->sample.accessLength
    // I could have done numDiffSamples * overlapBytes, but it will cause misattribution when access sizes are not same at dead and kill sites.
    // Basically, we are assuming that whatever happened in the observed watchpoints is applicable to the entire access length
    uint64_t inc = numDiffSamples * wpi->sample.accessLength;
    int joinNodeIdx = wpi->sample.isSamplePointAccurate? E_ACCURATE_JOIN_NODE_IDX : E_INACCURATE_JOIN_NODE_IDX;

    // if the access is a LOAD/LOAD_AND_STORE we are done! not a dead write :)
    if(wt->accessType == LOAD || wt->accessType == LOAD_AND_STORE) {
        // update the measured (i.e. not dead)
        usedBytes += inc;
        UpdateConcatenatedPathPair(wt->ctxt, wpi->sample.node /* oldNode*/, joinNodes[E_USED][joinNodeIdx] /* joinNode*/, measured_metric_id /* checkedMetric */, inc);
    } else {
        deadBytes += inc;
        UpdateConcatenatedPathPair(wt->ctxt, wpi->sample.node /* oldNode*/, joinNodes[E_KILLED][joinNodeIdx] /* joinNode*/, dead_metric_id /* checkedMetric */, inc);
    }
    return ALREADY_DISABLED;
}

static inline bool IsAddressReadable(void * addr){
    bool retVal = true;
    thread_data_t * td =  hpcrun_get_thread_data();
    hpcrun_set_handling_sample(td);
    sigjmp_buf_t* it = &(td->bad_unwind);
    int ljmp = sigsetjmp(it->jb, 1);
    if (ljmp == 0){
        volatile char i = *(char*)(addr);
    } else {
        // longjmp here
        retVal = false;
    }
    hpcrun_clear_handling_sample(td);
    return retVal;
}

static WPTriggerActionType RedStoreWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt){
    void *pip = wt->pc;
    if(!pip) {
        // if the ip is 0, let's drop the WP
        return ALREADY_DISABLED;
    }
    
    bool isFloatOperation = wt->floatType == ELEM_TYPE_UNKNOWN? false: true;
    bool redBytes = 0;
    
    // check integer instructions
    int overlapLen = GET_OVERLAP_BYTES(wt->va, safeAccessLen, wpi->sample.va, wpi->sample.wpLength);
    if(overlapLen <= 0){
        fprintf(stderr, "\n wpi->sample.va=%p, wpi->sample.wpLength = %d,  wt->va = %p, wt->accessLength=%d\n", wpi->sample.va, wpi->sample.wpLength, wt->va, wt->accessLength);
        monitor_real_abort();
    }

    int joinNodeIdx = wpi->sample.isSamplePointAccurate? E_ACCURATE_JOIN_NODE_IDX : E_INACCURATE_JOIN_NODE_IDX;
    int firstOffest = FIRST_OVERLAPPED_BYTE_OFFSET_IN_FIRST(wt->va, safeAccessLen, wpi->sample.va, wpi->sample.wpLength);
    int secondOffest = FIRST_OVERLAPPED_BYTE_OFFSET_IN_FIRST(wt->va, safeAccessLen, wpi->sample.va, wpi->sample.wpLength);
    
    void * wpiStartByte = wpi->sample.va + secondOffest;
    void * wtStartByte = wt->va + firstOffest;
    // if the overlapLen is not 4 or 8, we cannot do any FP, DP approximation.
    //wpiStartByte and wtStartByte are not 4 or 8 byte aligned, we cannot do any FP, DP approximation.
    
    // If we got an insane address that cannot be read, return silently
    if(!IsAddressReadable(wtStartByte)){
        return ALREADY_DISABLED;
    }
    
    if(isFloatOperation){
        switch (wt->floatType) {
            case ELEM_TYPE_SINGLE:{
                if(overlapLen < sizeof(float)){
                    goto TreatLikeInteger;
                }
                if(!IS_4_BYTE_ALIGNED(wpiStartByte)){
                    goto TreatLikeInteger;
                }
                if(!IS_4_BYTE_ALIGNED(wtStartByte)){
                    goto TreatLikeInteger;
                }
                // Sanity passed, now we can compare approximate equality.
                // the value of old is present in wpi->value[secondOffest]
                //float old = *((float*)(wpiStartByte));
                float old = *((float*)(wpi->value + secondOffest));
                float new = *((float*)(wtStartByte));
                if(old != new){
                    float rate = (old-new)/old;/////shasha:in case old may be 0
                    if(rate > APPROX_RATE || rate < -APPROX_RATE)
                        redBytes = 0;
                    else
                        // Amplify the observation by the access length
                        redBytes = sizeof(float);
                } else {
                    redBytes = sizeof(float);
                }
            }
                break;
                
            case ELEM_TYPE_DOUBLE:{
                if(overlapLen < sizeof(double)){
                    goto TreatLikeInteger;
                }
                if(!IS_8_BYTE_ALIGNED(wpiStartByte)){
                    goto TreatLikeInteger;
                }
                if(!IS_8_BYTE_ALIGNED(wtStartByte)){
                    goto TreatLikeInteger;
                }
                // Sanity passed, now we can compare approximate equality.
                //double old = *((double*)(wpiStartByte));
                double old = *((double*)(wpi->value + secondOffest));
                double new = *((double*)(wtStartByte));
                if(old != new){
                    double rate = (old-new)/old;/////shasha:in case old may be 0
                    if(rate > APPROX_RATE || rate < -APPROX_RATE)
                        redBytes = 0;
                    else
                        redBytes = sizeof(double);
                } else {
                    redBytes = sizeof(double);
                }
            }
                break;
                
            default: // unhandled!!
                goto TreatLikeInteger;
                break;
        }
        double myProportion = ProportionOfWatchpointAmongOthersSharingTheSameContext(wpi);
        uint64_t numDiffSamples = GetWeightedMetricDiffAndReset(wpi->sample.node, wpi->sample.sampledMetricId, myProportion);
        if(redBytes != 0) {
            // Now increment metric by numDiffSamples * wpi->sample.accessLength
            // This is an approximation of what might have happened.
            // If I observe that 4 bytes are redundant out of 128 accessed bytes, I amplify it to 128 bytes.
            uint64_t inc = numDiffSamples * wpi->sample.accessLength;
            oldAppxBytes += inc;
            UpdateConcatenatedPathPair(wt->ctxt, wpi->sample.node /* oldNode*/, joinNodes[E_KILLED][joinNodeIdx] /* joinNode*/, redApprox_metric_id /* checkedMetric */, inc);
        } else {
            // Now increment metric by numDiffSamples * wpi->sample.accessLength
            // This is an approximation of what might have happened.
            // If I observe that 4 bytes are NOT redundant out of 128 accessed bytes, I amplify it to 128 bytes.
            uint64_t inc = numDiffSamples * wpi->sample.accessLength;
            newBytes += inc;
            UpdateConcatenatedPathPair(wt->ctxt, wpi->sample.node /* oldNode*/, joinNodes[E_NEW_VAL][joinNodeIdx] /* joinNode*/, measured_metric_id /* checkedMetric */, inc);
        }
    }else /* non float */{
        
    TreatLikeInteger:
        ;
        
        for(int i = firstOffest, k = secondOffest ; i < firstOffest + overlapLen; i++, k++){
            if(((uint8_t*)(wt->va))[i] == wpi->value[k]) {
                redBytes ++;
            } else{
                redBytes = 0;
                break;
            }
        }
        double myProportion = ProportionOfWatchpointAmongOthersSharingTheSameContext(wpi);
        uint64_t numDiffSamples = GetWeightedMetricDiffAndReset(wpi->sample.node, wpi->sample.sampledMetricId, myProportion);
        
        if(redBytes != 0) {
            // Now increment metric: if the entire overlap is redundant, amplify to numDiffSamples * wpi->sample.accessLength
            // This is an approximation of what might have happened.
            // If I observe that 4 bytes are redundant out of 128 accessed bytes, I amplify it to 128 bytes.
            uint64_t inc = numDiffSamples * wpi->sample.accessLength;
            oldBytes += inc;
            UpdateConcatenatedPathPair(wt->ctxt, wpi->sample.node /* oldNode*/, joinNodes[E_KILLED][joinNodeIdx] /* joinNode*/, red_metric_id /* checkedMetric */, inc);
        } else {
            // Now increment metric: if the entire overlap is redundant, amplify to numDiffSamples * wpi->sample.accessLength
            // This is an approximation of what might have happened.
            // If I observe that 4 bytes are NOT redundant out of 128 accessed bytes, I amplify it to 128 bytes.
            uint64_t inc = numDiffSamples * wpi->sample.accessLength;
            newBytes += inc;
            UpdateConcatenatedPathPair(wt->ctxt, wpi->sample.node /* oldNode*/,  joinNodes[E_NEW_VAL][joinNodeIdx] /* joinNode*/, measured_metric_id /* checkedMetric */, inc);
        }
    }
    return ALREADY_DISABLED;
}

static WPTriggerActionType TemporalReuseWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt){
    if(!wt->pc) {
        // if the ip is 0, let's retain the WP
        return RETAIN_WP;
    }
    // Report a reuse
    double myProportion = ProportionOfWatchpointAmongOthersSharingTheSameContext(wpi);
    uint64_t numDiffSamples = GetWeightedMetricDiffAndReset(wpi->sample.node, wpi->sample.sampledMetricId, myProportion);
    int joinNodeIdx = wpi->sample.isSamplePointAccurate? E_ACCURATE_JOIN_NODE_IDX : E_INACCURATE_JOIN_NODE_IDX;

    // Now increment temporal_metric_id by numDiffSamples * overlapBytes
    uint64_t inc = numDiffSamples;
    reuse += inc;
    UpdateConcatenatedPathPair(wt->ctxt, wpi->sample.node /* oldNode*/, joinNodes[E_TEPORALLY_REUSED][joinNodeIdx] /* joinNode*/, temporal_metric_id /* checkedMetric */, inc);
    return ALREADY_DISABLED;
}

static WPTriggerActionType SpatialReuseWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt){
    if(!wt->pc) {
        // if the ip is 0, drop the WP
        return ALREADY_DISABLED;
    }
    // Report a reuse
    double myProportion = ProportionOfWatchpointAmongOthersSharingTheSameContext(wpi);
    uint64_t numDiffSamples = GetWeightedMetricDiffAndReset(wpi->sample.node, wpi->sample.sampledMetricId, myProportion);
    int joinNodeIdx = wpi->sample.isSamplePointAccurate? E_ACCURATE_JOIN_NODE_IDX : E_INACCURATE_JOIN_NODE_IDX;
    // Now increment dead_metric_id by numDiffSamples * overlapBytes
    uint64_t inc = numDiffSamples;
    reuse += inc;
    
    UpdateConcatenatedPathPair(wt->ctxt, wpi->sample.node /* oldNode*/, joinNodes[E_SPATIALLY_REUSED][joinNodeIdx] /* joinNode*/, spatial_metric_id /* checkedMetric */, inc);
    return ALREADY_DISABLED;
}

static WPTriggerActionType LoadLoadWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt){
    void *pip = wt->pc;
    if(!pip) {
        // if the ip is 0, let's drop the WP
        return ALREADY_DISABLED;
    }
    // If  this is a STORE ignore the WP and retain it.
    if(wt->accessType == STORE){
        return RETAIN_WP;
    }
    return RedStoreWPCallback(wpi, startOffset, safeAccessLen, wt);
}

/*
double get_consecutiveness_factor(long consecutiveness) {
	return 32963*exp(-0.0433*(double) consecutiveness);
}*/

// before

static WPTriggerActionType FalseSharingWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt){
    int metricId = -1;
    const void* joinNode;
    int joinNodeIdx = wpi->sample.isSamplePointAccurate? E_ACCURATE_JOIN_NODE_IDX : E_INACCURATE_JOIN_NODE_IDX;

    if(wt->accessType == LOAD){
        falseWRIns ++;
        metricId = false_wr_metric_id;
        joinNode = joinNodes[E_FALSE_WR_SHARE][joinNodeIdx];
    } else {
        if(wpi->sample.accessType == LOAD) {
            falseRWIns ++;
            metricId = false_rw_metric_id;
            joinNode = joinNodes[E_FALSE_RW_SHARE][joinNodeIdx];
        } else{
            falseWWIns ++;
            metricId =  false_ww_metric_id;
            joinNode = joinNodes[E_FALSE_WW_SHARE][joinNodeIdx];
        }
    }
    
    sample_val_t v = hpcrun_sample_callpath(wt->ctxt, measured_metric_id, SAMPLE_UNIT_INC, 0/*skipInner*/, 1/*isSync*/, NULL);
    // insert a special node
    cct_node_t *node = hpcrun_insert_special_node(v.sample_node, joinNode);
    node = hpcrun_cct_insert_path_return_leaf(wpi->sample.node, node);
    // update the metricId
    cct_metric_data_increment(metricId, node, (cct_metric_data_t){.i = 1});
    return ALREADY_DISABLED;
}
// after

static WPTriggerActionType AllSharingWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt){
    assert(0);
}

static WPTriggerActionType TrueSharingWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt){
    int metricId = -1;
    const void* joinNode;
    int joinNodeIdx = wpi->sample.isSamplePointAccurate? E_ACCURATE_JOIN_NODE_IDX : E_INACCURATE_JOIN_NODE_IDX;

    ts_matrix[wpi->sample.first_accessing_tid][TD_GET(core_profile_trace_data.id)]++;
    ts_matrix[TD_GET(core_profile_trace_data.id)][wpi->sample.first_accessing_tid]++;

    if(wt->accessType == LOAD){
        trueWRIns ++;
        metricId = true_wr_metric_id;
        joinNode = joinNodes[E_TRUE_WR_SHARE][joinNodeIdx];
    } else {
        if(wpi->sample.accessType == LOAD) {
            trueRWIns ++;
            metricId = true_rw_metric_id;
            joinNode = joinNodes[E_TRUE_RW_SHARE][joinNodeIdx];
        } else{
            trueWWIns ++;
            metricId =  true_ww_metric_id;
            joinNode = joinNodes[E_TRUE_WW_SHARE][joinNodeIdx];
        }
    }
    
    sample_val_t v = hpcrun_sample_callpath(wt->ctxt, measured_metric_id, SAMPLE_UNIT_INC, 0/*skipInner*/, 1/*isSync*/, NULL);
    // insert a special node
    cct_node_t *node = hpcrun_insert_special_node(v.sample_node, joinNode);
    node = hpcrun_cct_insert_path_return_leaf(wpi->sample.node, node);
    // update the metricId
    cct_metric_data_increment(metricId, node, (cct_metric_data_t){.i = 1});
    return ALREADY_DISABLED;
}


static WPTriggerActionType IPCFalseSharingWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt){
    int metricId = -1;
    const void* joinNode;
    int joinNodeIdx = wpi->sample.isSamplePointAccurate? E_ACCURATE_JOIN_NODE_IDX : E_INACCURATE_JOIN_NODE_IDX;

    if(wt->accessType == LOAD){
        falseWRIns ++;
        metricId = false_wr_metric_id;
        joinNode = joinNodes[E_IPC_FALSE_WR_SHARE][joinNodeIdx];
    } else {
        if(wpi->sample.accessType == LOAD) {
            falseRWIns ++;
            metricId = false_rw_metric_id;
            joinNode = joinNodes[E_IPC_FALSE_RW_SHARE][joinNodeIdx];
        } else{
            falseWWIns ++;
            metricId =  false_ww_metric_id;
            joinNode = joinNodes[E_IPC_FALSE_WW_SHARE][joinNodeIdx];
        }
    }
    
    sample_val_t v = hpcrun_sample_callpath(wt->ctxt, measured_metric_id, SAMPLE_UNIT_INC, 0/*skipInner*/, 1/*isSync*/, NULL);
    // insert a special node
    cct_node_t *node = hpcrun_insert_special_node(v.sample_node, joinNode);
    node = hpcrun_cct_insert_array_path_return_leaf(wpi->sample.node, node);
    // update the metricId
    cct_metric_data_increment(metricId, node, (cct_metric_data_t){.i = 1});
    return ALREADY_DISABLED;
}
static WPTriggerActionType IPCTrueSharingWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt){
    int metricId = -1;
    const void* joinNode;
    int joinNodeIdx = wpi->sample.isSamplePointAccurate? E_ACCURATE_JOIN_NODE_IDX : E_INACCURATE_JOIN_NODE_IDX;

    if(wt->accessType == LOAD){
        trueWRIns ++;
        metricId = true_wr_metric_id;
        joinNode = joinNodes[E_IPC_TRUE_WR_SHARE][joinNodeIdx];
    } else {
        if(wpi->sample.accessType == LOAD) {
            trueRWIns ++;
            metricId = true_rw_metric_id;
            joinNode = joinNodes[E_IPC_TRUE_RW_SHARE][joinNodeIdx];
        } else{
            trueWWIns ++;
            metricId =  true_ww_metric_id;
            joinNode = joinNodes[E_IPC_TRUE_WW_SHARE][joinNodeIdx];
        }
    }
    
    sample_val_t v = hpcrun_sample_callpath(wt->ctxt, measured_metric_id, SAMPLE_UNIT_INC, 0/*skipInner*/, 1/*isSync*/, NULL);
    // insert a special node
    cct_node_t *node = hpcrun_insert_special_node(v.sample_node, joinNode);
    node = hpcrun_cct_insert_array_path_return_leaf(wpi->sample.node, node);
    // update the metricId
    cct_metric_data_increment(metricId, node, (cct_metric_data_t){.i = 1});
    return ALREADY_DISABLED;
}
static WPTriggerActionType IPCAllSharingWPCallback(WatchPointInfo_t *wpi, int startOffset, int safeAccessLen, WatchPointTrigger_t * wt){
    return ALREADY_DISABLED;
}


static inline bool IsLibMonitorAddress(void * addr) {
    // race is ok,
    if(!libmonitorLM){
        libmonitorLM = hpcrun_loadmap_findByName(hpcrun_loadmap_findLoadName("libmonitor.so"))->dso_info;
    }
    
    if (addr >= libmonitorLM->start_addr && addr < libmonitorLM->end_addr){
        return true;
    }
    return false;
}

static inline bool IsHPCRunAddress(void * addr) {
    if(!hpcrunLM){
        hpcrunLM = hpcrun_loadmap_findByName(hpcrun_loadmap_findLoadName("libhpcrun.so"))->dso_info;
    }
    
    if (addr >= hpcrunLM->start_addr && addr < hpcrunLM->end_addr){
        return true;
    }
    return false;
}


static inline bool isTdataAddress(void *addr) {
    void *tdata = &inside_hpcrun;
    if ((addr > tdata-100) && (addr < tdata+100)) return true;
    return false;
}

static inline bool IsBlackListedWatchpointAddress(void *addr){
    for(int i = 0; i < numBlackListAddresses; i++){
        if (addr >= blackListAddresses[i].startAddr && addr < blackListAddresses[i].endAddr){
            return true;
        }
    }
    return false;
}

// Avoids Kernel address and zeros
static inline bool IsValidAddress(void * addr, void * pc){
    thread_data_t * td =  hpcrun_get_thread_data();
    if( (addr == 0) )
        return false;
    
    if( (pc == 0) )
        return false;

    if(( (void*)(td-1) <= addr) && (addr < (void*)(td+2))) // td data
        return false;
    if(IsAltStackAddress(addr))
        return false;
    if(IsFSorGS(addr))
        return false;   
 
    if(IsBlackListedWatchpointAddress(addr) || IsBlackListedWatchpointAddress(pc)){
        return false;
    }
    
    if (isTdataAddress(addr))
        return false;
    
    if((addr && !(((unsigned long)addr) & 0xF0000000000000)) &&
       (pc && !(((unsigned long)pc) & 0xF0000000000000)))
        return true;
    return false;
}

void ReadSharedDataTransactionally(SharedData_t *localSharedData){
    // Laport's STM
    do{
        int64_t startCounter = gSharedData.counter;
        if(startCounter & 1)
            continue; // Some writer is updating
        
        __sync_synchronize();
        *localSharedData = gSharedData;
        __sync_synchronize();
        int64_t endCounter = gSharedData.counter;
        if(startCounter == endCounter)
            break;
    }while(1);
}

int static inline GetFloorWPLength(int accessLen){
    switch (accessLen) {
        default:
        case 8: return 8;
        case 7:case 6: case 5: case 4: return 4;
        case 3:case 2: return 2;
        case 1: return 1;
    }
}

int static inline GetFloorWPLengthAtAddress(void * address, int accessLen){
    uint8_t alignment = ((size_t) address) & (MAX_WP_LENGTH -1);
    
    switch (alignment) {
        case 1: case 3: case 5: case 7: /* 1-byte aligned */ return 1;
        case 2: case 6: /* 2-byte aligned */ return MIN(2, accessLen);
        case 0: /* 8-byte aligned */ return MIN(8, accessLen);
        case 4: /* 8-byte aligned */ return MIN(4, accessLen);
        default:
            assert(0 && "Should never reach here");
            return 1;
    }
}

typedef struct FalseSharingLocs{
    size_t va;
    int wpLen;
}FalseSharingLocs;

static inline void GetAllFalseSharingLocations(size_t va, int accessLen, size_t baseAddr, int maxFSLength, int * wpSizes, int curWPSizeIdx, int totalWPSizes, FalseSharingLocs * fsl, int * numFSLocs){
    int curWPSize = wpSizes[curWPSizeIdx];
    for(int i = 0; i < maxFSLength/curWPSize; i ++) {
        size_t curAddr = baseAddr + i * curWPSize;
        int overlapLen = GET_OVERLAP_BYTES(curAddr, curWPSize, va, accessLen);
        if(0 >= overlapLen) {
            fsl[*numFSLocs].va = curAddr;
            fsl[*numFSLocs].wpLen = curWPSize;
            (*numFSLocs)++;
        }else if (curWPSize != overlapLen) {
            if(curWPSizeIdx+1 < totalWPSizes) {
                GetAllFalseSharingLocations(va, accessLen, curAddr, curWPSize, wpSizes, curWPSizeIdx+1, totalWPSizes, fsl, numFSLocs);
            }
        } else {
            // Nop
        }
    }
}

#define PAGEMAP_ENTRY 8
#define GET_BIT(X,Y) (X & ((uint64_t)1<<Y)) >> Y
#define GET_PFN(X) X & 0x7FFFFFFFFFFFFF
const int __endian_bit = 1;
#define is_bigendian() ( (*(char*)&__endian_bit) == 0 )
#define MAX_BACKTRACE_LEN (1024)
#define MAX_IPC_PROCS (1024)
#define MAX_LOAD_MODULES (1024)
#define MAX_LM_PATH_LEN (1024)
#define INVALID_PHYSICAL_ADDRESS (0L)
#define INVALID_VIRUAL_ADDRESS ((void*) 0)
#define PAGE_SZ (4096)
#define PAGE_OFFSET(a) ((size_t)(a) & (PAGE_SZ-1))
#define GET_VA_PAGE(p) (void*)((size_t)(p) & ~(PAGE_SZ-1))
#define GET_PA_PAGE(p) (unsigned long)((p) & ~(PAGE_SZ-1))
#define PA_PATH "/proc/self/pagemap"
#define VA_PATH "/proc/self/maps"
#define VM_MAP_CHECK_FREQUENCY (128)


typedef enum LM_ENTRY_STATUS {UNUSED=0, TRANSIENT=1, STABLE=2} LM_ENTRY_STATUS_t;

typedef struct LM_ID_PATH{
    volatile LM_ENTRY_STATUS_t status;
//    uint16_t lmId;
    char realPath[MAX_LM_PATH_LEN];
}LM_ID_PATH;


typedef struct IPC_FSInfo{
    char dummy2[CACHE_LINE_SZ];
    uint64_t time;
    pid_t tid;
    WatchPointType wpType;
    AccessType accessType;
    unsigned long address;
    unsigned long offset;
    int accessLen;
    uint16_t btLength;
    struct cct_addr_t backtrace[MAX_BACKTRACE_LEN];
    char dummy4[CACHE_LINE_SZ];
}IPC_FSInfo;

typedef struct IPC_SharedData{
    char dummy1[CACHE_LINE_SZ];
    volatile uint32_t curThreadsSubsribed;
    char dummy2[CACHE_LINE_SZ];
    volatile uint32_t numLMIds;
    char dummy3[CACHE_LINE_SZ];
    LM_ID_PATH sharedLMInfo[MAX_LOAD_MODULES];
    char dummy4[CACHE_LINE_SZ];
    volatile uint64_t counter;
    char dummy5[CACHE_LINE_SZ];
    IPC_FSInfo fsInfo;
    char dummy6[CACHE_LINE_SZ];
}IPC_SharedData;

typedef struct VAPAMap {
    // Id given by us
    void * virtualAddress;
    unsigned long physicalAddress;
    struct VAPAMap *left;
    struct VAPAMap *right;
} VAPAMap_t;

__thread VAPAMap_t * vaToPAMap = NULL;
__thread VAPAMap_t * paToVAMap = NULL;
__thread pid_t myTid = -1;
__thread bool ipcDataInited = false;
__thread IPC_FSInfo localIPCInfo;
__thread time_t lastMapChangeTime;
__thread struct stat mapsStat;
__thread uint64_t lastVMMAPCheck = 0;


IPC_SharedData * ipcSharedData;

static const char * shared_key = "/FALSE_SHARING_KEY";





void ReadIPCSharedDataTransactionally(IPC_FSInfo *ipcFSInfo){
    // Laport's STM
    do{
        int64_t startCounter = ipcSharedData->counter;
        if(startCounter & 1)
            continue; // Some writer is updating
        
        __sync_synchronize();
        *ipcFSInfo = ipcSharedData->fsInfo;
        __sync_synchronize();
        int64_t endCounter = ipcSharedData->counter;
        if(startCounter == endCounter)
            break;
    }while(1);
}

static void destroy_shared_memory(void * p) {
    // we should munmap, but I will not do since we dont do it in so many other places in hpcrun
    // munmap(ipc_data);
    shm_unlink((char *)shared_key);
}

static inline void create_shared_memory() {
    int fd ;
    if ( (fd = shm_open(shared_key, O_RDWR | O_CREAT, 0666)) < 0 ) {
        EEMSG("Failed to shm_open (%s), retval = %d", shared_key, fd);
        monitor_real_abort();
    }
    if ( ftruncate(fd, sizeof(IPC_SharedData)) < 0 ) {
        EEMSG("Failed to ftruncate()");
        monitor_real_abort();
    }
    void * ptr = mmap(NULL, sizeof(IPC_SharedData), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    if(ptr == MAP_FAILED ) {
        EEMSG("Failed to mmap() IPC_SharedData");
        monitor_real_abort();
    }
    if(__sync_bool_compare_and_swap(&ipcSharedData, 0, ptr)){
        hpcrun_process_aux_cleanup_add(destroy_shared_memory, NULL);
    }
       
}

uint16_t GetOrCreateIPCSharedLMEntry(const char * realPath){
    
    if(ipcSharedData == NULL)
        create_shared_memory();
    // start from 1; leave 0 out;
    for(uint16_t i = 1 ; i < MAX_LOAD_MODULES; i++){
        switch (ipcSharedData->sharedLMInfo[i].status) {
            case STABLE:
                if(0==strncmp(realPath, ipcSharedData->sharedLMInfo[i].realPath, MAX_LM_PATH_LEN))
                    return i;
                break;
            case TRANSIENT:
            TRANSIENT_CASE:
                while(ipcSharedData->sharedLMInfo[i].status != STABLE) ; // spin
                if(0==strncmp(realPath, ipcSharedData->sharedLMInfo[i].realPath, MAX_LM_PATH_LEN))
                    return i;
                break;
            case UNUSED:
                // Attempt to install
                if(__sync_bool_compare_and_swap(&(ipcSharedData->sharedLMInfo[i].status), UNUSED, TRANSIENT))  {
                    strncpy(ipcSharedData->sharedLMInfo[i].realPath, realPath, MAX_LM_PATH_LEN);
                    // need a fence on weak memory here.
                    ipcSharedData->sharedLMInfo[i].status = STABLE;
                    return i;
                } else {
                    goto TRANSIENT_CASE;
                }
            default:
                assert(0 && "SHOULD NEVER REACH HERE");
                monitor_real_abort();
        }
    }
    assert(0 && "Should never reach here");
    monitor_real_abort();
    return 0;
}

unsigned long GetPFN(unsigned long virt_addr){
    FILE * f = fopen(PA_PATH, "rb");
    if(!f){
        printf("Error! Cannot open %s\n", PA_PATH);
        goto ErrExit;
    }
    
    //Shifting by virt-addr-offset number of bytes
    //and multiplying by the size of an address (the size of an entry in pagemap file)
    uint64_t file_offset = virt_addr / getpagesize() * PAGEMAP_ENTRY;
    int status = fseek(f, file_offset, SEEK_SET);
    if(status){
        perror("Failed to do fseek!");
        goto ErrExit;
    }
    errno = 0;
    uint64_t read_val = 0;
    unsigned char c_buf[PAGEMAP_ENTRY];
    for(int i=0; i < PAGEMAP_ENTRY; i++){
        unsigned char c = getc(f);
        if(c==EOF){
            //printf("\nReached end of the file\n");
            goto ErrExit;
        }
        if(is_bigendian())
            c_buf[i] = c;
        else
            c_buf[PAGEMAP_ENTRY - i - 1] = c;
    }
    
    fclose(f);
    
    for(int i=0; i < PAGEMAP_ENTRY; i++){
        //printf("%d ",c_buf[i]);
        read_val = (read_val << 8) + c_buf[i];
    }
    //
    if(GET_BIT(read_val, 63)) {
        return (unsigned long long) GET_PFN(read_val);
    }
    //    else
    //        printf("Page not present\n");
    //    if(GET_BIT(read_val, 62))
    //        printf("Page swapped\n");
    
    return INVALID_PHYSICAL_ADDRESS;
ErrExit:
    if(f){
        fclose(f);
    }
    return INVALID_PHYSICAL_ADDRESS;
    
}



static inline struct VAPAMap* splayVAtoPAMap(struct VAPAMap* root, void* vPage) {
    REGULAR_SPLAY_TREE(VAPAMap, root, vPage, virtualAddress, left, right);
    return root;
}

static inline struct VAPAMap* splayPAtoVAMap(struct VAPAMap* root, unsigned long pPage) {
    REGULAR_SPLAY_TREE(VAPAMap, root, pPage, physicalAddress, left, right);
    return root;
}


static void InsertVAtoPAMap(void * va, unsigned long pa){
    VAPAMap_t * found    = splayVAtoPAMap(vaToPAMap, va);
    
    // Check if a trace node with traceKey already exists under this context node
    if(found && (va == found->virtualAddress)) {
        vaToPAMap = found;
        if(found->physicalAddress != pa)
            found->physicalAddress = pa;
    } else {
        VAPAMap_t* newNode = hpcrun_malloc(sizeof(VAPAMap_t));
        newNode->virtualAddress = va;
        newNode->physicalAddress = pa;
        if(!found) {
            newNode->left = NULL;
            newNode->right = NULL;
        } else if(va < found->virtualAddress) {
            newNode->left = found->left;
            newNode->right = found;
            found->left = NULL;
        } else { // addr > addr of found
            newNode->left = found;
            newNode->right = found->right;
            found->right = NULL;
        }
        vaToPAMap = newNode;
    }
}


static void InsertPAtoVAMap(unsigned long pa, void * va){
    VAPAMap_t * found    = splayPAtoVAMap(paToVAMap, pa);
    
    // Check if a trace node with traceKey already exists under this context node
    if(found && (pa == found->physicalAddress)) {
        paToVAMap = found;
        if(found->virtualAddress != va)
            found->virtualAddress = va;
    } else {
        VAPAMap_t * newNode = hpcrun_malloc(sizeof(VAPAMap_t));
        newNode->virtualAddress = va;
        newNode->physicalAddress = pa;
        if(!found) {
            newNode->left = NULL;
            newNode->right = NULL;
        } else if(pa < found->physicalAddress) {
            newNode->left = found->left;
            newNode->right = found;
            found->left = NULL;
        } else { // addr > addr of found
            newNode->left = found;
            newNode->right = found->right;
            found->right = NULL;
        }
        paToVAMap = newNode;
    }
}



unsigned long GetPAfromVA(void * va){
    void * pageBaseAddr = GET_VA_PAGE(va);
    REGULAR_SPLAY_TREE(VAPAMap, vaToPAMap, pageBaseAddr, virtualAddress, left, right);
    if(vaToPAMap && (pageBaseAddr == vaToPAMap->virtualAddress)) {
        return vaToPAMap->physicalAddress;
    }
    return INVALID_PHYSICAL_ADDRESS;
}

void * GetVAfromPA(unsigned long pa){
    unsigned long  pageBaseAddr = pa;
    REGULAR_SPLAY_TREE(VAPAMap, paToVAMap, pageBaseAddr, physicalAddress, left, right);
    if(paToVAMap && (pageBaseAddr == paToVAMap->physicalAddress)) {
        return paToVAMap->virtualAddress;
    }
    return INVALID_VIRUAL_ADDRESS;
}



static void GetAllSharedPages() {
    FILE* loadmap = fopen(VA_PATH, "r");
    if (! loadmap) {
        EMSG("Could not open /proc/self/maps");
        return;
    }
    char linebuf[1024 + 1];
    char* addr = NULL;
    for(;;) {
        char* l = fgets(linebuf, sizeof(linebuf), loadmap);
        if (feof(loadmap)) break;
        char* save = NULL;
        const char delim[] = " \n";
        addr = strtok_r(l, delim, &save);
        char* perms = strtok_r(NULL, delim, &save);
        // if write is not allowed skip
        if (perms[1] == '-') {
            continue;
        }
        if (perms[3] == 's') {
            const char dash[] = "-";
            char* start_str = strtok_r(addr, dash, &save);
            char* end_str   = strtok_r(NULL, dash, &save);
            void *start = (void*) (uintptr_t) strtol(start_str, NULL, 16);
            void *end   = (void*) (uintptr_t) strtol(end_str, NULL, 16);
            const long pgSz = sysconf(_SC_PAGESIZE);
            for(void *va = start; va < end; va += pgSz) {
                // Get PA for each VA page
                unsigned long pa =  (unsigned long) GetPFN((unsigned long)va);
                if(pa != INVALID_PHYSICAL_ADDRESS) {
                    //                    printf("\n VA:%xl = %xl", va, pa);
                    InsertVAtoPAMap(va, pa);
                    InsertPAtoVAMap(pa, va);
                }
            }
        }
    }
    fclose(loadmap);
}

static void UpdateVMMap(){
    int s = stat(VA_PATH, &mapsStat);
    if(s != 0){
        fprintf(stderr, "\n Failed to STAT %s", VA_PATH);
    }
    
    if( ((lastVMMAPCheck % VM_MAP_CHECK_FREQUENCY) == 0)
       && (lastMapChangeTime != mapsStat.st_mtime)) {
        // New mapping
        GetAllSharedPages();
    }
    lastMapChangeTime = mapsStat.st_mtime;
    lastVMMAPCheck++;
}

static void HandleIPCFalseSharing(void * data_addr, void * pc, cct_node_t *node, int accessLen, AccessType accessType, int sampledMetricId, bool isSamplePointAccurate){
    if(ipcSharedData == NULL){
        create_shared_memory();
    }
    if (ipcDataInited == false) {
        myTid = syscall(SYS_gettid);
        lastVMMAPCheck = 0;
        ipcDataInited = true;
    }
    // is address on shared page?
    unsigned long pa = GetPAfromVA(data_addr);
    // Ok, on a shared page!
    // Ok to publish new data?
    
    // Is the published address old enough (stayed for > 1 sample time span)
    int64_t curTime = rdtsc();
    volatile IPC_FSInfo * globalIPCInfo = &(ipcSharedData->fsInfo);
    
    pid_t me = myTid;
    // Get the time, tid, and counter
    // This is definately racy but benign.
    uint64_t theCounter = ipcSharedData->counter;
    if( ((curTime-globalIPCInfo->time) > 2 * (curTime-lastTime)) // Sufficient time passed since the last time somebody published
       &&
       ( (theCounter & 1) == 0) // Nobody is in the process of publishing
       && (pa != INVALID_PHYSICAL_ADDRESS) // my PA is a valid address
       ) {
        // Attempt to lockout
        
        if(__sync_bool_compare_and_swap(&(ipcSharedData->counter), theCounter, theCounter+1)){
        } else {
            // Failed to update ==> someone else succeeded ==> Fetch that address and set a WP for that
            goto SET_FS_WP;
        }
        
        
        globalIPCInfo->time = rdtsc();
        globalIPCInfo->tid = myTid;
        globalIPCInfo->wpType = accessType == LOAD ? WP_WRITE : WP_RW;
        globalIPCInfo->accessType = accessType;
        globalIPCInfo->address = pa;
        globalIPCInfo->offset = PAGE_OFFSET(data_addr);
        globalIPCInfo->accessLen = accessLen;
        
        int btLen = 0;
        for(; btLen < MAX_BACKTRACE_LEN - 1; btLen++){
            if (node == NULL)
                break;
            globalIPCInfo->backtrace[btLen] = *hpcrun_cct_addr(node);
            node = hpcrun_cct_parent(node);
        }
        
        // unlikely; if btLen == MAX_BACKTRACE_LEN; drop the WP by invalidating it
        if (btLen == MAX_BACKTRACE_LEN -1 ) {
            globalIPCInfo->tid = -1;
        } else {
            globalIPCInfo->btLength = btLen;
            // Set the last entry null
            globalIPCInfo->backtrace[btLen].ip_norm.lm_id = 0;
            globalIPCInfo->backtrace[btLen].ip_norm.lm_ip = 0;
        }
        __sync_synchronize();
        ipcSharedData->counter = theCounter + 2; // makes the counter even
    } else if ((globalIPCInfo->tid != me)  && (globalIPCInfo->tid != -1)/* dont set WP for my own accessed locations */){
        // If the data is "new" set the WP
    SET_FS_WP: ReadIPCSharedDataTransactionally(&localIPCInfo);
        // Get the VA from PA
        void * va = GetVAfromPA (localIPCInfo.address);
        if(va == INVALID_VIRUAL_ADDRESS) {
            goto ErrExit;
        }
        
        va = va + localIPCInfo.offset;
        
        long  metricThreshold = hpcrun_id2metric(sampledMetricId)->period;
        accessedIns += metricThreshold;
        switch (theWPConfig->id) {
            case WP_IPC_TRUE_SHARING:{
                // Set WP at the same address
                SampleData_t sd= {
                    .va = va,
                    .node = localIPCInfo.backtrace,
                    .accessType=localIPCInfo.accessType,
                    .type=localIPCInfo.wpType,
                    .wpLength = GetFloorWPLengthAtAddress(va, accessLen),
                    .accessLength= accessLen,
                    .sampledMetricId=sampledMetricId,
                    .isSamplePointAccurate = isSamplePointAccurate,
                    .preWPAction=theWPConfig->preWPAction,
		//.first_accessing_thread_id
                    .isBackTrace = true
                };
                SubscribeWatchpoint(&sd, OVERWRITE, false /* capture value */);
            }
                break;
            case WP_IPC_FALSE_SHARING: {
                int wpSizes[] = {8, 4, 2, 1};
                FalseSharingLocs falseSharingLocs[CACHE_LINE_SZ];
                int numFSLocs = 0;
                GetAllFalseSharingLocations((size_t) va, accessLen, ALIGN_TO_CACHE_LINE((size_t)va), CACHE_LINE_SZ, wpSizes, 0 /*curWPSizeIdx*/ , 4 /*totalWPSizes*/, falseSharingLocs, &numFSLocs);
                // Find 4 slots in the cacheline
                for(int i = 0; i < numFSLocs/2; i ++) {
                    int idx = rdtsc() % numFSLocs;
                    FalseSharingLocs tmpVal = falseSharingLocs[idx];
                    falseSharingLocs[idx] = falseSharingLocs[i];
                    falseSharingLocs[i] = tmpVal;
                }
                for(int i = 0; i < MIN(numFSLocs, wpConfig.maxWP); i ++) {
                    SampleData_t sd= {
                        .va = (void *) falseSharingLocs[i].va,
                        .node = localIPCInfo.backtrace,
                        .accessType=localIPCInfo.accessType,
                        .type=localIPCInfo.wpType,
                        .wpLength = falseSharingLocs[i].wpLen,
                        .accessLength= accessLen,
                        .sampledMetricId=sampledMetricId,
                        .isSamplePointAccurate = isSamplePointAccurate,
                        .preWPAction=theWPConfig->preWPAction,
                        .isBackTrace = true
                    };
                    SubscribeWatchpoint(&sd, OVERWRITE, false /* capture value */);
                }
            }
                break;
            case WP_IPC_ALL_SHARING: {
                assert(0);
            }
                break;
            default:
                break;
        }
    }else{
        /* dont set WP for my own accessed locations */
    }
ErrExit:
    lastTime = rdtsc();
}

#if 0
bool PrintStats(){
    extern get_access_type(void *);
    void *  contextIP = hpcrun_context_pc(context);
    int v1 = get_access_type(mmap_data->ip);
    int v2 = get_access_type(contextIP);
    
    switch(v1){
        case 0: unk1++; break;
        case 1: ld1++; break;
        case 2: st1++; break;
        case 3: mix1++; break;
        default: break;
    }
    switch(v2){
        case 0: unk2++; break;
        case 1: ld2++; break;
        case 2: st2++; break;
        case 3: mix2++; break;
        default: break;
    }
    
    float tot = unk1 + ld1 + st1 + mix1;
    fprintf(stderr, "W=%f (%f), L=%f(%f), M=%f(%f), U=%f(%f)\n", st1/tot, st2/tot, ld1/tot, ld2/tot, mix1/tot, mix2/tot, unk1/tot, unk2/tot);
    /*
     if( (mmap_data->ip > contextIP) || (contextIP-mmap_data->ip >15)) {
     incorrect++;
     fprintf(stderr, "BAD IP: contextIP=%p, precieIP=%p, data=%p %f, get_access_type=%d\n", contextIP, mmap_data->ip, mmap_data->addr, 1.0*(incorrect)/(incorrect+correct), get_access_type(mmap_data->ip));
     }  else {
     correct++;
     }
     */
    void *  contextIP = hpcrun_context_pc(context);
    extern int is_same_function(void *ins1, void* ins2);
    int samev1 = is_same_function(contextIP, mmap_data->ip);
    
    switch(samev1){
        case 0: difffunc++; break;
        case 1: samefunc++; break;
        case 2: unknwfunc++; break;
        default: break;
    }
    double tot = difffunc + samefunc + unknwfunc;
    if (mmap_data->ip==contextIP)
        ipSame ++;
    else
        ipDiff ++;
    fprintf(stderr, "Diff=%f, Same=%f, Unkn=%f, Ctxt=%p, %p, Same =%f\n", difffunc/tot, samefunc/tot, unknwfunc/tot, contextIP, mmap_data->ip, 1.0 * ipSame/ (ipSame+ipDiff));
}
#endif

SharedEntry_t getEntryRandomlyFromBulletinBoard(int tid, uint64_t cur_time, int * do_not_arm_watchpoint) {
	int hashIndex = rdtsc() % HASHTABLESIZE;
	int iter = 0;
	while(1) {
		if(iter == HASHTABLESIZE) {
			*do_not_arm_watchpoint = 1;
			break;
		}
		if((bulletinBoard.hashTable[hashIndex].cacheLineBaseAddress != -1) && (bulletinBoard.hashTable[hashIndex].tid != tid) && ((cur_time - bulletinBoard.hashTable[hashIndex].time) < bulletinBoard.hashTable[hashIndex].expiration_period))
			break;
		++hashIndex;
		hashIndex %= HASHTABLESIZE;
		iter++;
	}
	return bulletinBoard.hashTable[hashIndex];
}

int hashCode(void * key) {
   return (uint64_t) key % 54121 % HASHTABLESIZE;
}

SharedEntry_t getEntryFromBulletinBoard(void * cacheLineBaseAddress, int * item_not_found) {
	int hashIndex = hashCode(cacheLineBaseAddress);
	int iter = 0;
	while((bulletinBoard.hashTable[hashIndex].cacheLineBaseAddress != -1) && (cacheLineBaseAddress != bulletinBoard.hashTable[hashIndex].cacheLineBaseAddress) && iter < HASHTABLESIZE) {
		++hashIndex;
		hashIndex %= HASHTABLESIZE;
		iter++;
	}
	if(iter == HASHTABLESIZE)
		*item_not_found = 1;
	return bulletinBoard.hashTable[hashIndex];
}


void hashInsertwithTime(struct SharedEntry item, uint64_t cur_time, uint64_t prev_time) {
   void * cacheLineBaseAddress = item.cacheLineBaseAddress;
   int hashIndex = hashCode(cacheLineBaseAddress);

   int iter = 0;
   while(bulletinBoard.hashTable[hashIndex].cacheLineBaseAddress != -1 && cacheLineBaseAddress != bulletinBoard.hashTable[hashIndex].cacheLineBaseAddress && iter < HASHTABLESIZE) {
      ++hashIndex;
      hashIndex %= HASHTABLESIZE;
      iter++;
   }
   if (bulletinBoard.hashTable[hashIndex].cacheLineBaseAddress == -1) {
	bulletinBoard.hashTable[hashIndex] = item;
  } else if((cacheLineBaseAddress == bulletinBoard.hashTable[hashIndex].cacheLineBaseAddress) && ((item.time - bulletinBoard.hashTable[hashIndex].time) > (cur_time - prev_time))) {
	item.prev_transfer_counter = bulletinBoard.hashTable[hashIndex].prev_transfer_counter;
	bulletinBoard.hashTable[hashIndex] = item;
  } else {
        iter = 1;
	uint64_t oldest_time = bulletinBoard.hashTable[0].time;
	int targetIndex = 0;
	hashIndex = 1;
	while(iter < HASHTABLESIZE) {
		if(bulletinBoard.hashTable[hashIndex].time < oldest_time) {
			oldest_time = bulletinBoard.hashTable[hashIndex].time;
			targetIndex = hashIndex;
		}
		++hashIndex;   
      		hashIndex %= HASHTABLESIZE;
      		iter++;
	}
	if((item.time - bulletinBoard.hashTable[targetIndex].time) > (cur_time - prev_time)) {
		bulletinBoard.hashTable[targetIndex] = item;
	}
  }
}

bool OnSample(perf_mmap_data_t * mmap_data, void * contextPC, cct_node_t *node, int sampledMetricId) {
    void * data_addr = mmap_data->addr;
    void * precisePC = (mmap_data->header_misc & PERF_RECORD_MISC_EXACT_IP) ? mmap_data->ip : 0;
    // Filert out address and PC (0 or kernel address will not pass)
    if (!IsValidAddress(data_addr, precisePC)) {
        goto ErrExit; // incorrect access type
    }
   
    // do not monitor NULL CCT node
    if (node == NULL) {
        goto ErrExit; // incorrect CCT
    }
    
   // fprintf(stderr, " numWatchpointsSet=%lu\n", wpStats.numWatchpointsSet);

   int accessLen;
   AccessType accessType;

   if(false == get_mem_access_length_and_type(precisePC, (uint32_t*)(&accessLen), &accessType)){
       //EMSG("Sampled a non load store at = %p\n", precisePC);
       goto ErrExit; // incorrect access type
   }
   if(accessType == UNKNOWN || accessLen == 0){
       //EMSG("Sampled sd.accessType = %d, accessLen=%d at precisePC = %p\n", accessType, accessLen, precisePC);
       goto ErrExit; // incorrect access type
   }

   if(accessType == LOAD)
	number_of_load_sample++;
   if(accessType == STORE)
        number_of_store_sample++;
   if(accessType == LOAD_AND_STORE) {
        number_of_load_store_sample++;
	if(strncmp (hpcrun_id2metric(sampledMetricId)->name,"MEM_UOPS_RETIRED:ALL_STORES",27) == 0)
		number_of_load_store_sample_all_stores++;
	if(strncmp (hpcrun_id2metric(sampledMetricId)->name,"MEM_UOPS_RETIRED:ALL_LOADS",26) == 0)
		number_of_load_store_sample_all_loads++;
   }
    // change here


    // if the context PC and precise PC are not in the same function, then the sample point is inaccurate.
    bool isSamplePointAccurate;
    FunctionType ft = is_same_function(contextPC, precisePC);
    if (ft == SAME_FN) {
        isSamplePointAccurate = true;
    } else {
        isSamplePointAccurate = false;
    }
    
    switch (theWPConfig->id) {
        case WP_DEADSPY:{
            if(accessType == LOAD){
                //EMSG("Sampled accessType = %d\n", accessType);
                goto ErrExit; // incorrect access type
            }
            
            long  metricThreshold = hpcrun_id2metric(sampledMetricId)->period;
            writtenBytes += accessLen * metricThreshold;
            SampleData_t sd= {
                .va = data_addr,
                .node = node,
                .type=WP_RW,
                .wpLength = accessLen,
                .accessLength= accessLen,
                .accessType=accessType,
                .sampledMetricId=sampledMetricId,
                .isSamplePointAccurate = isSamplePointAccurate,
                .preWPAction=theWPConfig->preWPAction,
                .isBackTrace = false
            };
            sd.wpLength = GetFloorWPLength(accessLen);
            SubscribeWatchpoint(&sd, OVERWRITE, false /* capture value */);
        }
            break;
            
        case WP_REDSPY:{
            // If we got an insane address that cannot be read, return silently
            if(!IsAddressReadable(data_addr)){
                goto ErrExit;
            }

            long  metricThreshold = hpcrun_id2metric(sampledMetricId)->period;
            writtenBytes += accessLen * metricThreshold;
            SampleData_t sd= {
                .va = data_addr,
                .node = node,
                .type=WP_WRITE,
                .wpLength = accessLen,
                .accessLength= accessLen,
                .accessType=accessType,
                .sampledMetricId=sampledMetricId,
                .isSamplePointAccurate = isSamplePointAccurate,
                .preWPAction=theWPConfig->preWPAction,
                .isBackTrace = false
            };
            // Must have a store address
            if(accessType == STORE || sd.accessType == LOAD_AND_STORE){
                sd.wpLength = GetFloorWPLength(accessLen);
                SubscribeWatchpoint(&sd, OVERWRITE, true /* capture value */);
            } else {
                //EMSG("Sampled accessType = %d\n", accessType);
                goto ErrExit; // incorrect access type
            }
        }
            break;
        case WP_LOADSPY:{
            // If we got an insane address that cannot be read, return silently
            if(!IsAddressReadable(data_addr)){
                goto ErrExit;
            }

            long  metricThreshold = hpcrun_id2metric(sampledMetricId)->period;
            loadedBytes += accessLen * metricThreshold;
            // we use WP_RW because we cannot set WP_READ alone
            SampleData_t sd= {
                .va = data_addr,
                .node = node,
                .type=WP_RW,
                .wpLength = accessLen,
                .accessLength= accessLen,
                .accessType=accessType,
                .sampledMetricId=sampledMetricId,
                .isSamplePointAccurate = isSamplePointAccurate,
                .preWPAction=theWPConfig->preWPAction,
                .isBackTrace = false
            };
            // Must have a store address
            if(accessType == LOAD || sd.accessType == LOAD_AND_STORE){
                sd.wpLength = GetFloorWPLength(accessLen);
                SubscribeWatchpoint(&sd, OVERWRITE, true /* capture value */);
            } else {
                //EMSG("Sampled accessType = %d\n", accessType);
                goto ErrExit; // incorrect access type
            }
        }
            break;
        case WP_SPATIAL_REUSE:{
            long  metricThreshold = hpcrun_id2metric(sampledMetricId)->period;
            accessedIns += metricThreshold;
            
            SampleData_t sd= {
                .node = node,
                .type=WP_RW,
                .accessType=accessType,
                .wpLength = accessLen,
                .accessLength= accessLen,
                .sampledMetricId=sampledMetricId,
                .isSamplePointAccurate = isSamplePointAccurate,
                .preWPAction=theWPConfig->preWPAction,
                .isBackTrace = false
            };
            sd.wpLength = GetFloorWPLength(accessLen);
            // randomly protect another word in the same cache line
            uint64_t aligned_pc = ALIGN_TO_CACHE_LINE((uint64_t)data_addr);
            if ((rdtsc() & 1) == 0)
                sd.va = (void*) (aligned_pc - CACHE_LINE_SZ);
            else
                sd.va = (void *) (aligned_pc + CACHE_LINE_SZ);
#if 0
            int offset = ((uint64_t)data_addr - aligned_pc) / accessLen;
            int bound = CACHE_LINE_SZ / accessLen;
            int r = rdtsc() % bound;
            if (r == offset) r = (r+1) % bound;
            sd.va = aligned_pc + (r * accessLen);
#endif
            if (!IsValidAddress(sd.va, precisePC)) {
                goto ErrExit; // incorrect access type
            }
            SubscribeWatchpoint(&sd, OVERWRITE, false /* capture value */);
        }
            break;
        case WP_TEMPORAL_REUSE:{
            long  metricThreshold = hpcrun_id2metric(sampledMetricId)->period;
            accessedIns += metricThreshold;
            
            SampleData_t sd= {
                .va = data_addr,
                .node = node,
                .type=WP_RW,
                .accessType=accessType,
                .wpLength = accessLen,
                .accessLength= accessLen,
                .sampledMetricId=sampledMetricId,
                .isSamplePointAccurate = isSamplePointAccurate,
                .preWPAction=theWPConfig->preWPAction,
                .isBackTrace = false
            };
            sd.wpLength = GetFloorWPLength(accessLen);
            SubscribeWatchpoint(&sd, OVERWRITE, false /* capture value */);
        }
            break;
        case WP_FALSE_SHARING:
        case WP_TRUE_SHARING:
        case WP_ALL_SHARING:{
            
            // Is the published address old enough (stayed for > 1 sample time span)
            int64_t curTime = rdtsc();
            SharedData_t localSharedData;
            int me = TD_GET(core_profile_trace_data.id);
            // Get the time, tid, and counter
            // This is definately racy but benign.
            localSharedData.time = gSharedData.time;
            localSharedData.tid = gSharedData.tid;
            localSharedData.counter = gSharedData.counter;
            
            //ReadSharedDataTransactionally(&localSharedData);
            if( ((curTime-localSharedData.time) > 2 * (curTime-lastTime)) // Sufficient time passed since the last time somebody published
               &&
               ((localSharedData.counter & 1) == 0) // Nobody is in the process of publishing
               ) {
                // Attempt to replace WP with my new address
                uint64_t theCounter = localSharedData.counter;
                localSharedData.time = rdtsc();
                localSharedData.tid = me;
                localSharedData.wpType = accessType == LOAD ? WP_WRITE : WP_RW;
                localSharedData.accessType = accessType;
                localSharedData.address = data_addr;
                localSharedData.accessLen = accessLen;
                localSharedData.counter ++; // makes the counter odd
                localSharedData.node = node;
                
                if(__sync_bool_compare_and_swap(&gSharedData.counter, theCounter, theCounter+1)){
                    gSharedData = localSharedData;
                    __sync_synchronize();
                    gSharedData.counter++; // makes the counter even
                } else {
                    // Failed to update ==> someone else succeeded ==> Fetch that address and set a WP for that
                    goto SET_FS_WP;
                }
            } else if ((localSharedData.tid != me)  && (localSharedData.tid != -1)/* dont set WP for my own accessed locations */){
                // If the data is "new" set the WP
            SET_FS_WP: ReadSharedDataTransactionally(&localSharedData);
                long  metricThreshold = hpcrun_id2metric(sampledMetricId)->period;
                accessedIns += metricThreshold;
                
                switch (theWPConfig->id) {
                    case WP_TRUE_SHARING:{
                        // Set WP at the same address
                        SampleData_t sd= {
                            .va = localSharedData.address,
                            .node = localSharedData.node,
                            .accessType=localSharedData.accessType,
                            .type=localSharedData.wpType,
                            .wpLength = GetFloorWPLengthAtAddress(localSharedData.address, accessLen),
                            .accessLength= accessLen,
                            .sampledMetricId=sampledMetricId,
                            .isSamplePointAccurate = isSamplePointAccurate,
                            .preWPAction=theWPConfig->preWPAction,
                            .isBackTrace = false
                        };
                        SubscribeWatchpoint(&sd, OVERWRITE, false /* capture value */);
                    }
                        break;
                    case WP_FALSE_SHARING: {
                        int wpSizes[] = {8, 4, 2, 1};
                        FalseSharingLocs falseSharingLocs[CACHE_LINE_SZ];
                        int numFSLocs = 0;
                        GetAllFalseSharingLocations( (size_t) localSharedData.address, accessLen, ALIGN_TO_CACHE_LINE((size_t)localSharedData.address), CACHE_LINE_SZ, wpSizes, 0 /*curWPSizeIdx*/ , 4 /*totalWPSizes*/, falseSharingLocs, &numFSLocs);
                        // Find 4 slots in the cacheline
                        for(int i = 0; i < numFSLocs/2; i ++) {
                            int idx = rdtsc() % numFSLocs;
                            FalseSharingLocs tmpVal = falseSharingLocs[idx];
                            falseSharingLocs[idx] = falseSharingLocs[i];
                            falseSharingLocs[i] = tmpVal;
                        }
                        for(int i = 0; i < MIN(numFSLocs, wpConfig.maxWP); i ++) {
                            SampleData_t sd= {
                                .va = (void *) falseSharingLocs[i].va,
                                .node = localSharedData.node,
                                .accessType=localSharedData.accessType,
                                .type=localSharedData.wpType,
                                .wpLength = falseSharingLocs[i].wpLen,
                                .accessLength= accessLen,
                                .sampledMetricId=sampledMetricId,
                                .isSamplePointAccurate = isSamplePointAccurate,
                                .preWPAction=theWPConfig->preWPAction,
                                .isBackTrace = false
                            };
			    //fprintf(stderr, "watchpoint is set\n");
                            SubscribeWatchpoint(&sd, OVERWRITE, false /* capture value */);
                        }
                    }
                        break;
                    case WP_ALL_SHARING: {
                        void * cacheLineBaseAddress = (void *) ALIGN_TO_CACHE_LINE((size_t)localSharedData.address);
                        // Find 4 slots in the cacheline
			// FIXME: make dynamic
                        int shuffleNums[CACHE_LINE_SZ/MAX_WP_LENGTH] = {0, 1, 2, 3, 4, 5, 6, 7}; // hard coded
                        for(int i = 0; i < CACHE_LINE_SZ/MAX_WP_LENGTH/2; i ++) {
                            int idx = rdtsc() & (CACHE_LINE_SZ/MAX_WP_LENGTH -1);
                            int tmpVal = shuffleNums[idx];
                            shuffleNums[idx] = shuffleNums[i];
                            shuffleNums[i] = tmpVal;
                        }
                        for(int i = 0; i < wpConfig.maxWP; i ++) {
                            SampleData_t sd= {
                                .va = cacheLineBaseAddress + (shuffleNums[i] << 3),
                                .node = localSharedData.node,
                                .accessType=localSharedData.accessType,
                                .type=localSharedData.wpType,
                                .wpLength = MAX_WP_LENGTH,
                                .accessLength= accessLen,
                                .sampledMetricId=sampledMetricId,
                                .isSamplePointAccurate = isSamplePointAccurate,
                                .preWPAction=theWPConfig->preWPAction,
                                .isBackTrace = false
                            };
                            SubscribeWatchpoint(&sd, OVERWRITE, false /* capture value */);
                        }
                    }
                        break;
                    default:
                        break;
                }
            }else{
                /* dont set WP for my own accessed locations */
            }
            lastTime = curTime;
        }
            break;
            
        case WP_IPC_FALSE_SHARING:
        case WP_IPC_TRUE_SHARING: {
            UpdateVMMap();
            HandleIPCFalseSharing(data_addr, precisePC, node, accessLen, accessType, sampledMetricId, isSamplePointAccurate);
        }
            break;
	case WP_COMDETECTIVE: {

	    int sType = -1;
	    if (strncmp (hpcrun_id2metric(sampledMetricId)->name,"MEM_UOPS_RETIRED:ALL_STORES",27) == 0)
            	sType = ALL_STORE;
            else if(strncmp (hpcrun_id2metric(sampledMetricId)->name,"MEM_UOPS_RETIRED:ALL_LOADS",26) == 0)
               	sType = ALL_LOAD;
            else sType = UNKNOWN_SAMPLE_TYPE;
	    uint64_t curtime = rdtsc();

	    int64_t storeCurTime = 0;
	    if(/*sType == ALL_STORE*/ (accessType == STORE || (accessType == LOAD_AND_STORE && sType == ALL_STORE)) )
		storeCurTime = curtime;


	    int me = TD_GET(core_profile_trace_data.id);
	    int current_core = sched_getcpu();
	    void * cacheLineBaseAddressVar = (void *) ALIGN_TO_CACHE_LINE((size_t)data_addr);
	    int item_not_found = 0;
	    struct SharedEntry item;
	    do{ 
        	int64_t startCounter = bulletinBoard.counter;
        	if(startCounter & 1) {
            		continue;
        	}
        	__sync_synchronize();
        	item = getEntryFromBulletinBoard(cacheLineBaseAddressVar, &item_not_found);
        	__sync_synchronize();
        	int64_t endCounter = bulletinBoard.counter;
        	if(startCounter == endCounter) {
			//printf("retrieved successfully\n");
            		break;
		}
    	    }while(1);

	    int arm_watchpoint_flag = 0;

	    if((item.cacheLineBaseAddress == -1) || (item_not_found == 1)) {
		arm_watchpoint_flag = 1;
	    } else {
		// detect communication events
		if((me != item.tid) && (item.time > prev_timestamp)) {
			int flag = 0;
			double global_sampling_period = 0;
			if(sType == ALL_LOAD) {
                		global_sampling_period = (double) global_load_sampling_period;
                		flag = 1;
        		}
        		if(sType == ALL_STORE) {
                		global_sampling_period = (double) global_store_sampling_period;
                		flag = 1;
        		}
			int max_thread_num = item.tid;
    			if(max_thread_num < me)
    			{   
        			max_thread_num = me;
    			}
    			if(as_matrix_size < max_thread_num)
    			{   
        			matrix_size_set(max_thread_num);
        			fs_matrix_size =  max_thread_num;
        			ts_matrix_size =  max_thread_num;
        			as_matrix_size =  max_thread_num;  
    			}

			int max_core_num = item.core_id;
                	if(max_core_num < current_core)
                	{
                		max_core_num = current_core;
                	}
                	if(as_core_matrix_size < max_core_num)
                	{
                		core_matrix_size_set(max_core_num);
                		fs_core_matrix_size =  max_core_num;
                        	ts_core_matrix_size =  max_core_num;
                        	as_core_matrix_size =  max_core_num;
                	}
			if(flag == 1) {
				if(GET_OVERLAP_BYTES(item.address, item.accessLen, data_addr, accessLen) > 0) {
				
					ts_matrix[item.tid][me] = ts_matrix[item.tid][me] + global_sampling_period;
					if(item.core_id != current_core) {
                                		ts_core_matrix[item.core_id][current_core] = ts_core_matrix[item.core_id][current_core] + global_sampling_period;
					}
				} else {
					fs_matrix[item.tid][me] = fs_matrix[item.tid][me] + global_sampling_period;
					if(item.core_id != current_core) {
						fs_core_matrix[item.core_id][current_core] = fs_core_matrix[item.core_id][current_core] + global_sampling_period;
					}
				}
	    		}

	    	} else {
			// arm watchpoints
			arm_watchpoint_flag = 1;
		}
	    }
	    
	    if (arm_watchpoint_flag) {
		// begin watchpoints
		int do_not_arm_watchpoint = 0;
		struct SharedEntry localSharedData = getEntryRandomlyFromBulletinBoard(me, curtime, &do_not_arm_watchpoint);
		if((localSharedData.cacheLineBaseAddress != -1) && !do_not_arm_watchpoint) {
			long  metricThreshold = hpcrun_id2metric(sampledMetricId)->period;
                	accessedIns += metricThreshold;
			void * cacheLineBaseAddress = localSharedData.cacheLineBaseAddress;
			int shuffleNums[CACHE_LINE_SZ/MAX_WP_LENGTH] = {0, 1, 2, 3, 4, 5, 6, 7}; // hard coded
                        for(int i = 0; i < wpConfig.maxWP; i ++) {
                            int idx = rdtsc() & (CACHE_LINE_SZ/MAX_WP_LENGTH -1);
                            int tmpVal = shuffleNums[idx];
                            shuffleNums[idx] = shuffleNums[i];
                            shuffleNums[i] = tmpVal;
                        }
			number_of_arming++;

                        for(int i = 0; i < wpConfig.maxWP; i ++) {
                            SampleData_t sd= {
                                .va = cacheLineBaseAddress + (shuffleNums[i] << 3),
				.target_va = localSharedData.address,
                                .node = localSharedData.node,
				.samplerAccessType = accessType,
                                .accessType=localSharedData.accessType,
				.sampleType=sType,
                                .type=localSharedData.wpType,
                                .wpLength = MAX_WP_LENGTH,
                                .accessLength= accessLen,
                                .sampledMetricId=sampledMetricId,
                                .isSamplePointAccurate = isSamplePointAccurate,
                                .preWPAction=theWPConfig->preWPAction,
                                .isBackTrace = false,
				.first_accessing_tid = localSharedData.tid,
				.first_accessing_core_id = localSharedData.core_id,
				.bulletinBoardTimestamp = curtime
				//.expiration_period = (storeLastTime == 0 ? 0 : (storeCurTime - storeLastTime))
                            };
                            SubscribeWatchpointWithTime(&sd, OVERWRITE, false /* capture value */, curtime, lastTime);
                        }
		}
		// end watchpoints
	    }

	    // starts
	    if((sType == ALL_LOAD) || ((item.cacheLineBaseAddress != -1) && ((curtime - item.time) <= (storeCurTime - storeLastTime)))) {
		//printf("not inserted\n");
	    } else {
		uint64_t bulletinCounter = bulletinBoard.counter;
		if((bulletinCounter & 1) == 0) {
			if(__sync_bool_compare_and_swap(&bulletinBoard.counter, bulletinCounter, bulletinCounter+1)){
				struct SharedEntry inserted_item;
                		inserted_item.time = curtime;
                		inserted_item.tid = me;
				inserted_item.core_id = sched_getcpu();
                		inserted_item.wpType = WP_RW;
                		inserted_item.accessType = accessType;
                		inserted_item.sampleType = sType;
                		inserted_item.address = data_addr;
                		inserted_item.accessLen = accessLen;
				inserted_item.node = node;
				inserted_item.cacheLineBaseAddress = cacheLineBaseAddressVar;
				inserted_item.prev_transfer_counter = 0;
				inserted_item.expiration_period = (storeLastTime == 0 ? 0 : (storeCurTime - storeLastTime));
				int bb_flag = 0;
				__sync_synchronize();
                		hashInsertwithTime(inserted_item, storeCurTime, storeLastTime);
				__sync_synchronize();
                		bulletinBoard.counter++;
			}
		}
	    }
	    // ends

	    lastTime = curtime;
	    if(accessType == STORE || (accessType == LOAD_AND_STORE && sType == ALL_STORE))
		storeLastTime = storeCurTime;
	}
        default:
            break;
    }
    wpStats.numWatchpointsSet ++;
    //printf("here\n");
    return true;
    
ErrExit:
    wpStats.numImpreciseSamples ++;
    return false;
    
}


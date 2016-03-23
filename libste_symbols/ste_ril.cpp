/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* //device/libs/telephony/ril.cpp
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "RILC"

#include <hardware_legacy/power.h>
#include <telephony/ril.h>
#include <telephony/ril_cdma_sms.h>
#include <cutils/sockets.h>
#include <cutils/jstring.h>
#include <telephony/record_stream.h>
#include <utils/Log.h>
#include <utils/SystemClock.h>
#include <pthread.h>
#include <binder/Parcel.h>
#include <cutils/jstring.h>
#include <sys/types.h>
#include <sys/limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <alloca.h>
#include <sys/un.h>
#include <assert.h>
#include <netinet/in.h>
#include <cutils/properties.h>
#include <RilSapSocket.h>

extern "C" void
RIL_onRequestComplete(RIL_Token t, RIL_Errno e, void *response, size_t responselen);
namespace android {

#define PHONE_PROCESS "radio"
#define BLUETOOTH_PROCESS "bluetooth"

#define SOCKET_NAME_RIL "rild"
#define SOCKET2_NAME_RIL "rild2"
#define SOCKET3_NAME_RIL "rild3"
#define SOCKET4_NAME_RIL "rild4"

#define SOCKET_NAME_RIL_DEBUG "rild-debug"

#define ANDROID_WAKE_LOCK_NAME "radio-interface"

#define ANDROID_WAKE_LOCK_SECS 0
#define ANDROID_WAKE_LOCK_USECS 200000

#define PROPERTY_RIL_IMPL "gsm.version.ril-impl"
#define PROPERTY_QAN_ELEMENTS "ro.ril.telephony.mqanelements"

// match with constant in RIL.java
#define MAX_COMMAND_BYTES (8 * 1024)

// Basically: memset buffers that the client library
// shouldn't be using anymore in an attempt to find
// memory usage issues sooner.
#define MEMSET_FREED 1

#define NUM_ELEMS(a)     (sizeof (a) / sizeof (a)[0])

#define MIN(a,b) ((a)<(b) ? (a) : (b))

/* Constants for response types */
#define RESPONSE_SOLICITED 0
#define RESPONSE_UNSOLICITED 1

/* Negative values for private RIL errno's */
#define RIL_ERRNO_INVALID_RESPONSE -1

// request, response, and unsolicited msg print macro
#define PRINTBUF_SIZE 8096

// Enable verbose logging
#define VDBG 0

// Enable RILC log
#define RILC_LOG 0

#if RILC_LOG
    #define startRequest           sprintf(printBuf, "(")
    #define closeRequest           sprintf(printBuf, "%s)", printBuf)
    #define printRequest(token, req) \
      RLOGD("[%04d]> %s %s", token, requestToString(req), printBuf)

    #define startResponse           sprintf(printBuf, "%s {", printBuf)
    #define closeResponse           sprintf(printBuf, "%s}", printBuf)
    #define printResponse           RLOGD("%s", printBuf)

    #define clearPrintBuf           printBuf[0] = 0
    #define removeLastChar          printBuf[strlen(printBuf)-1] = 0
    #define appendPrintBuf(x...)    sprintf(printBuf, x)
#else
    #define startRequest
    #define closeRequest
    #define printRequest(token, req)
    #define startResponse
    #define closeResponse
    #define printResponse
    #define clearPrintBuf
    #define removeLastChar
    #define appendPrintBuf(x...)
#endif

enum WakeType {DONT_WAKE, WAKE_PARTIAL};

typedef struct {
    int requestNumber;
    void (*dispatchFunction) (Parcel &p, struct RequestInfo *pRI);
    int(*responseFunction) (Parcel &p, void *response, size_t responselen);
} CommandInfo;

typedef struct {
    int requestNumber;
    int (*responseFunction) (Parcel &p, void *response, size_t responselen);
    WakeType wakeType;
} UnsolResponseInfo;

typedef struct RequestInfo {
    int32_t token;      //this is not RIL_Token
    CommandInfo *pCI;
    struct RequestInfo *p_next;
    char cancelled;
    char local;         // responses to local commands do not go back to command process
    RIL_SOCKET_ID socket_id;
} RequestInfo;

typedef struct UserCallbackInfo {
    RIL_TimedCallback p_callback;
    void *userParam;
    struct ril_event event;
    struct UserCallbackInfo *p_next;
} UserCallbackInfo;

extern "C" const char * requestToString(int request);
extern "C" const char * failCauseToString(RIL_Errno);
extern "C" const char * callStateToString(RIL_CallState);
extern "C" const char * radioStateToString(RIL_RadioState);
extern "C" const char * rilSocketIdToString(RIL_SOCKET_ID socket_id);

extern "C"
char rild[MAX_SOCKET_NAME_LENGTH] = SOCKET_NAME_RIL;

#define RIL_VENDOR_COMMANDS_OFFSET 10000

/*******************************************************************/

RIL_RadioFunctions s_callbacks = {0, NULL, NULL, NULL, NULL, NULL};
static int s_registerCalled = 0;

static pthread_t s_tid_dispatch;
static pthread_t s_tid_reader;
static int s_started = 0;

static int s_fdDebug = -1;
static int s_fdDebug_socket2 = -1;

static int s_fdWakeupRead;
static int s_fdWakeupWrite;

static struct ril_event s_commands_event;
static struct ril_event s_wakeupfd_event;
static struct ril_event s_listen_event;
static SocketListenParam s_ril_param_socket;

static pthread_mutex_t s_pendingRequestsMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_writeMutex = PTHREAD_MUTEX_INITIALIZER;
static RequestInfo *s_pendingRequests = NULL;

#if (SIM_COUNT >= 2)
static struct ril_event s_commands_event_socket2;
static struct ril_event s_listen_event_socket2;
static SocketListenParam s_ril_param_socket2;

static pthread_mutex_t s_pendingRequestsMutex_socket2  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_writeMutex_socket2            = PTHREAD_MUTEX_INITIALIZER;
static RequestInfo *s_pendingRequests_socket2          = NULL;
#endif

#if (SIM_COUNT >= 3)
static struct ril_event s_commands_event_socket3;
static struct ril_event s_listen_event_socket3;
static SocketListenParam s_ril_param_socket3;

static pthread_mutex_t s_pendingRequestsMutex_socket3  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_writeMutex_socket3            = PTHREAD_MUTEX_INITIALIZER;
static RequestInfo *s_pendingRequests_socket3          = NULL;
#endif

#if (SIM_COUNT >= 4)
static struct ril_event s_commands_event_socket4;
static struct ril_event s_listen_event_socket4;
static SocketListenParam s_ril_param_socket4;

static pthread_mutex_t s_pendingRequestsMutex_socket4  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_writeMutex_socket4            = PTHREAD_MUTEX_INITIALIZER;
static RequestInfo *s_pendingRequests_socket4          = NULL;
#endif

static struct ril_event s_wake_timeout_event;
static struct ril_event s_debug_event;


static const struct timeval TIMEVAL_WAKE_TIMEOUT = {ANDROID_WAKE_LOCK_SECS,ANDROID_WAKE_LOCK_USECS};


static pthread_mutex_t s_startupMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_startupCond = PTHREAD_COND_INITIALIZER;

static pthread_mutex_t s_dispatchMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_dispatchCond = PTHREAD_COND_INITIALIZER;

static RequestInfo *s_toDispatchHead = NULL;
static RequestInfo *s_toDispatchTail = NULL;

static UserCallbackInfo *s_last_wake_timeout_info = NULL;

static void *s_lastNITZTimeData = NULL;
static size_t s_lastNITZTimeDataSize;

#if RILC_LOG
    static char printBuf[PRINTBUF_SIZE];
#endif

/*******************************************************************/
static int sendResponse (Parcel &p, RIL_SOCKET_ID socket_id);

static void dispatchVoid (Parcel& p, RequestInfo *pRI);
static void dispatchString (Parcel& p, RequestInfo *pRI);
static void dispatchStrings (Parcel& p, RequestInfo *pRI);
static void dispatchInts (Parcel& p, RequestInfo *pRI);
static void dispatchDial (Parcel& p, RequestInfo *pRI);
static void dispatchSIM_IO (Parcel& p, RequestInfo *pRI);
static void dispatchSIM_APDU (Parcel& p, RequestInfo *pRI);
static void dispatchCallForward(Parcel& p, RequestInfo *pRI);
static void dispatchRaw(Parcel& p, RequestInfo *pRI);
static void dispatchSmsWrite (Parcel &p, RequestInfo *pRI);
static void dispatchDataCall (Parcel& p, RequestInfo *pRI);
static void dispatchVoiceRadioTech (Parcel& p, RequestInfo *pRI);
static void dispatchSetInitialAttachApn (Parcel& p, RequestInfo *pRI);
static void dispatchCdmaSubscriptionSource (Parcel& p, RequestInfo *pRI);

static void dispatchCdmaSms(Parcel &p, RequestInfo *pRI);
static void dispatchImsSms(Parcel &p, RequestInfo *pRI);
static void dispatchImsCdmaSms(Parcel &p, RequestInfo *pRI, uint8_t retry, int32_t messageRef);
static void dispatchImsGsmSms(Parcel &p, RequestInfo *pRI, uint8_t retry, int32_t messageRef);
static void dispatchCdmaSmsAck(Parcel &p, RequestInfo *pRI);
static void dispatchGsmBrSmsCnf(Parcel &p, RequestInfo *pRI);
static void dispatchCdmaBrSmsCnf(Parcel &p, RequestInfo *pRI);
static void dispatchRilCdmaSmsWriteArgs(Parcel &p, RequestInfo *pRI);
static void dispatchNVReadItem(Parcel &p, RequestInfo *pRI);
static void dispatchNVWriteItem(Parcel &p, RequestInfo *pRI);
static void dispatchUiccSubscripton(Parcel &p, RequestInfo *pRI);
static void dispatchSimAuthentication(Parcel &p, RequestInfo *pRI);
static void dispatchDataProfile(Parcel &p, RequestInfo *pRI);
static void dispatchRadioCapability(Parcel &p, RequestInfo *pRI);
static int responseInts(Parcel &p, void *response, size_t responselen);
static int responseIntsGetPreferredNetworkType(Parcel &p, void *response, size_t responselen);
static int responseStrings(Parcel &p, void *response, size_t responselen);
static int responseStringsNetworks(Parcel &p, void *response, size_t responselen);
static int responseStrings(Parcel &p, void *response, size_t responselen, bool network_search);
static int responseString(Parcel &p, void *response, size_t responselen);
static int responseVoid(Parcel &p, void *response, size_t responselen);
static int responseCallList(Parcel &p, void *response, size_t responselen);
static int responseSMS(Parcel &p, void *response, size_t responselen);
static int responseSIM_IO(Parcel &p, void *response, size_t responselen);
static int responseCallForwards(Parcel &p, void *response, size_t responselen);
static int responseDataCallList(Parcel &p, void *response, size_t responselen);
static int responseSetupDataCall(Parcel &p, void *response, size_t responselen);
static int responseRaw(Parcel &p, void *response, size_t responselen);
static int responseSsn(Parcel &p, void *response, size_t responselen);
static int responseSimStatus(Parcel &p, void *response, size_t responselen);
static int responseGsmBrSmsCnf(Parcel &p, void *response, size_t responselen);
static int responseCdmaBrSmsCnf(Parcel &p, void *response, size_t responselen);
static int responseCdmaSms(Parcel &p, void *response, size_t responselen);
static int responseCellList(Parcel &p, void *response, size_t responselen);
static int responseCdmaInformationRecords(Parcel &p,void *response, size_t responselen);
static int responseRilSignalStrength(Parcel &p,void *response, size_t responselen);
static int responseCallRing(Parcel &p, void *response, size_t responselen);
static int responseCdmaSignalInfoRecord(Parcel &p,void *response, size_t responselen);
static int responseCdmaCallWaiting(Parcel &p,void *response, size_t responselen);
static int responseSimRefresh(Parcel &p, void *response, size_t responselen);
static int responseCellInfoList(Parcel &p, void *response, size_t responselen);
static int responseHardwareConfig(Parcel &p, void *response, size_t responselen);
static int responseDcRtInfo(Parcel &p, void *response, size_t responselen);
static int responseRadioCapability(Parcel &p, void *response, size_t responselen);
static int responseSSData(Parcel &p, void *response, size_t responselen);
static int responseLceStatus(Parcel &p, void *response, size_t responselen);
static int responseLceData(Parcel &p, void *response, size_t responselen);
static int responseActivityData(Parcel &p, void *response, size_t responselen);

static int decodeVoiceRadioTechnology (RIL_RadioState radioState);
static int decodeCdmaSubscriptionSource (RIL_RadioState radioState);
static RIL_RadioState processRadioState(RIL_RadioState newRadioState);

static bool isServiceTypeCfQuery(RIL_SsServiceType serType, RIL_SsRequestType reqType);

#ifdef RIL_SHLIB
#if defined(ANDROID_MULTI_SIM)
extern "C" void RIL_onUnsolicitedResponse(int unsolResponse, const void *data,
                                size_t datalen, RIL_SOCKET_ID socket_id);
#else
extern "C" void RIL_onUnsolicitedResponse(int unsolResponse, const void *data,
                                size_t datalen);
#endif
#endif

#if defined(ANDROID_MULTI_SIM)
#define RIL_UNSOL_RESPONSE(a, b, c, d) RIL_onUnsolicitedResponse((a), (b), (c), (d))
#define CALL_ONREQUEST(a, b, c, d, e) s_callbacks.onRequest((a), (b), (c), (d), (e))
#define CALL_ONSTATEREQUEST(a) s_callbacks.onStateRequest(a)
#else
#define RIL_UNSOL_RESPONSE(a, b, c, d) RIL_onUnsolicitedResponse((a), (b), (c))
#define CALL_ONREQUEST(a, b, c, d, e) s_callbacks.onRequest((a), (b), (c), (d))
#define CALL_ONSTATEREQUEST(a) s_callbacks.onStateRequest()
#endif

static UserCallbackInfo * internalRequestTimedCallback
    (RIL_TimedCallback callback, void *param,
        const struct timeval *relativeTime);

/** Index == requestNumber */
static CommandInfo s_commands[] = {
#include "ril_commands.h"
};

static CommandInfo s_commands_v[] = {
#include "ril_commands_vendor.h"
};

static UnsolResponseInfo s_unsolResponses[] = {
#include "ril_unsol_commands.h"
};

static UnsolResponseInfo s_unsolResponses_v[] = {
#include "ril_unsol_commands_vendor.h"
};

/* For older RILs that do not support new commands RIL_REQUEST_VOICE_RADIO_TECH and
   RIL_UNSOL_VOICE_RADIO_TECH_CHANGED messages, decode the voice radio tech from
   radio state message and store it. Every time there is a change in Radio State
   check to see if voice radio tech changes and notify telephony
 */
int voiceRadioTech = -1;

/* For older RILs that do not support new commands RIL_REQUEST_GET_CDMA_SUBSCRIPTION_SOURCE
   and RIL_UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED messages, decode the subscription
   source from radio state and store it. Every time there is a change in Radio State
   check to see if subscription source changed and notify telephony
 */
int cdmaSubscriptionSource = -1;

/* For older RILs that do not send RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, decode the
   SIM/RUIM state from radio state and store it. Every time there is a change in Radio State,
   check to see if SIM/RUIM status changed and notify telephony
 */
int simRuimStatus = -1;

}; // namespace android

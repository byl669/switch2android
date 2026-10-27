#pragma once
#include <cstdint>
struct AAudioStream;
struct AAudioStreamBuilder;
using aaudio_result_t=int32_t;
using aaudio_performance_mode_t=int32_t;
using aaudio_data_callback_result_t=int32_t;
using AAudioStream_dataCallback=aaudio_data_callback_result_t(*)(AAudioStream*,void*,void*,int32_t);
using AAudioStream_errorCallback=void(*)(AAudioStream*,void*,aaudio_result_t);
constexpr int AAUDIO_DIRECTION_OUTPUT=0,AAUDIO_FORMAT_PCM_I16=1,AAUDIO_SHARING_MODE_SHARED=1;
// 枚举值照抄 NDK 的 aaudio/AAudio.h：降级链按 mode 分档，档位值写错就测不出真实行为。
constexpr int AAUDIO_PERFORMANCE_MODE_NONE=10,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY=12;
constexpr aaudio_result_t AAUDIO_OK=0;
constexpr int AAUDIO_CALLBACK_RESULT_CONTINUE=0,AAUDIO_ERROR_DISCONNECTED=-899,AAUDIO_ERROR_INVALID_FORMAT=-887,AAUDIO_ERROR_INTERNAL=-897,AAUDIO_ERROR_NO_SERVICE=-881;
// 数值按 NDK 枚举顺序（INVALID_FORMAT=-887 往前数）推出来的，只给测试用。
constexpr aaudio_result_t AAUDIO_ERROR_TIMEOUT=-889;
constexpr int AAUDIO_STREAM_STATE_STARTED=4,AAUDIO_STREAM_STATE_PAUSED=6;
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder**);
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder*,int);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder*,int);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder*,int);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder*,int);
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder*,int);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder*,int);
void AAudioStreamBuilder_setBufferCapacityInFrames(AAudioStreamBuilder*,int32_t);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder*,AAudioStream_dataCallback,void*);
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder*,AAudioStream_errorCallback,void*);
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder*,AAudioStream**);
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder*);
int32_t AAudioStream_getSampleRate(AAudioStream*);
int32_t AAudioStream_getChannelCount(AAudioStream*);
int32_t AAudioStream_getState(AAudioStream*);
aaudio_result_t AAudioStream_requestStart(AAudioStream*);
aaudio_result_t AAudioStream_requestStop(AAudioStream*);
aaudio_result_t AAudioStream_requestPause(AAudioStream*);
aaudio_result_t AAudioStream_close(AAudioStream*);
const char* AAudio_convertResultToText(aaudio_result_t);

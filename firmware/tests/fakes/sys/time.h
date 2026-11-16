#pragma once
#include <stdint.h>
struct bt_test_timeval {int64_t tv_sec;long tv_usec;};
#define timeval bt_test_timeval
inline int fakeSetTimeCalls=0,fakeSetTimeResult=0;
inline int64_t fakeSetTimeEpoch=0;
inline int settimeofday(const bt_test_timeval* t,void*){++fakeSetTimeCalls;fakeSetTimeEpoch=t->tv_sec;return fakeSetTimeResult;}

#pragma once
#include <cstdint>
#include <ctime>

void EITDecodeMjd(int i_mjd, int* p_y, int* p_m, int* p_d);
struct tm EITConvertStartTime(uint64_t i_date);
int EITConvertDuration(uint32_t i_duration);

// Converts an EIT start_time field to a Unix timestamp. The field carries JST
// wall-clock, so the conversion is done with a fixed +09:00 offset instead of
// std::mktime(), which would interpret it in the host's local timezone.
// Returns false when the field is unknown or out of range; *p_unix_time is
// then left untouched.
bool EITConvertStartTimeToUnixTime(uint64_t i_date, uint64_t* p_unix_time);

// Returns false if the struct tm produced by EITConvertStartTime has out-of-range fields.
// Caller should skip constructing ts::Time when this returns false.
bool isValidEITStartTime(const struct tm& t);

// Returns false if the year/month/day produced by EITDecodeMjd are out of range.
bool isValidMjdDate(int year, int mon, int mday);
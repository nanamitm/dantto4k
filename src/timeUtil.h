#pragma once
#include <cstdint>
#include <ctime>

void EITDecodeMjd(int i_mjd, int* p_y, int* p_m, int* p_d);
struct tm EITConvertStartTime(uint64_t i_date);
int EITConvertDuration(uint32_t i_duration);

// Returns false if the struct tm produced by EITConvertStartTime has out-of-range fields.
// Caller should skip constructing ts::Time when this returns false.
bool isValidEITStartTime(const struct tm& t);

// Returns false if the year/month/day produced by EITDecodeMjd are out of range.
bool isValidMjdDate(int year, int mon, int mday);
// BonDriver_File_MMTS
//
// A minimal IBonDriver2 implementation that plays back a raw MMT-TLV dump
// file (the same byte stream a real tuner driver would hand to dantto4k via
// GetTsStream()) instead of talking to real tuner hardware. Configured via
// BonDriver_File_MMTS.ini (FilePath=, BitrateMbps=) next to the DLL.
//
// This exists purely to let dantto4k (and TVTest behind it) reprocess a
// recorded 8K/4K MMT-TLV capture for debugging - it does not understand or
// validate MMT-TLV framing at all, it just streams file bytes.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <thread>
#include <cstdarg>

#include "../../src/IBonDriver2.h"

namespace {

constexpr DWORD kChunkSize = 188 * 1024; // arbitrary, not MMT-TLV-packet-aligned
constexpr double kDefaultBitrateMbps = 100.0; // pacing cap, above typical 8K bitrates

void DLog(const wchar_t *pFormat, ...)
{
	FILE *fp = nullptr;
	if (_wfopen_s(&fp, L"C:\\Free Soft Ware\\TVTeset4k8k\\bondriver_file_mmts_debug.log", L"a, ccs=UTF-8") != 0 || fp == nullptr)
		return;
	SYSTEMTIME st;
	::GetLocalTime(&st);
	fwprintf(fp, L"%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	va_list args;
	va_start(args, pFormat);
	vfwprintf(fp, pFormat, args);
	va_end(args);
	fwprintf(fp, L"\n");
	fclose(fp);
}

std::wstring GetIniPathNextToThisDll(HMODULE hModule)
{
	wchar_t path[MAX_PATH] = {};
	::GetModuleFileNameW(hModule, path, MAX_PATH);
	std::wstring s = path;
	const size_t dot = s.find_last_of(L'.');
	if (dot != std::wstring::npos)
		s = s.substr(0, dot);
	s += L".ini";
	return s;
}

} // namespace

class CBonDriverFileMMTS : public IBonDriver2
{
public:
	explicit CBonDriverFileMMTS(HMODULE hModule) : m_hModule(hModule) {}

	// IBonDriver
	const bool OpenTuner(void) override
	{
		std::lock_guard<std::mutex> lock(m_Mutex);

		DLog(L"OpenTuner: called");

		wchar_t iniPath[MAX_PATH] = {};
		const std::wstring path = GetIniPathNextToThisDll(m_hModule);
		wcsncpy_s(iniPath, path.c_str(), _TRUNCATE);
		DLog(L"OpenTuner: iniPath=%ls", iniPath);

		wchar_t filePath[MAX_PATH] = {};
		::GetPrivateProfileStringW(L"BonDriver_File_MMTS", L"FilePath", L"", filePath, MAX_PATH, iniPath);
		DLog(L"OpenTuner: filePath=%ls", filePath);
		if (filePath[0] == L'\0') {
			::OutputDebugStringW(L"BonDriver_File_MMTS: FilePath not set in ini\n");
			DLog(L"OpenTuner: FilePath empty, returning false");
			return false;
		}

		const double bitrateMbps = static_cast<double>(::GetPrivateProfileIntW(
			L"BonDriver_File_MMTS", L"BitrateMbps", static_cast<int>(kDefaultBitrateMbps), iniPath));
		m_BytesPerSec = (bitrateMbps * 1'000'000.0) / 8.0;
		DLog(L"OpenTuner: bitrateMbps=%.1f BytesPerSec=%.0f", bitrateMbps, m_BytesPerSec);

		m_File = _wfsopen(filePath, L"rb", _SH_DENYNO);
		if (!m_File) {
			::OutputDebugStringW(L"BonDriver_File_MMTS: failed to open FilePath\n");
			DLog(L"OpenTuner: fopen FAILED");
			return false;
		}
		DLog(L"OpenTuner: fopen OK");

		_fseeki64(m_File, 0, SEEK_END);
		m_FileSize = _ftelli64(m_File);
		DLog(L"OpenTuner: m_FileSize=%lld", m_FileSize);
		_fseeki64(m_File, 0, SEEK_SET);

		m_Buffer.resize(kChunkSize);
		m_StartTick = std::chrono::steady_clock::now();
		m_BytesDelivered = 0;
		m_Eof = false;

		DLog(L"OpenTuner: returning true");
		return true;
	}

	void CloseTuner(void) override
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		DLog(L"CloseTuner: called");
		if (m_File) {
			fclose(m_File);
			m_File = nullptr;
		}
	}

	const bool SetChannel(const uint8_t bCh) override
	{
		DLog(L"SetChannel(1-arg): bCh=%u", bCh);
		return true;
	}

	const float GetSignalLevel(void) override
	{
		return 50.0f;
	}

	const uint32_t WaitTsStream(const uint32_t dwTimeOut) override
	{
		// Pace roughly to m_BytesPerSec so dantto4k/TVTest see something
		// resembling a real-time live feed instead of the whole file at once.
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_File || m_Eof)
			return 0;

		const auto now = std::chrono::steady_clock::now();
		const double elapsedSec = std::chrono::duration<double>(now - m_StartTick).count();
		const double budgetBytes = elapsedSec * m_BytesPerSec;
		if (static_cast<double>(m_BytesDelivered) >= budgetBytes)
			return 1; // not ready yet, but don't report an error
		return 0;
	}

	const uint32_t GetReadyCount(void) override
	{
		return 0;
	}

	const bool GetTsStream(uint8_t* pDst, uint32_t* pdwSize, uint32_t* pdwRemain) override
	{
		uint8_t* pSrc = nullptr;
		const bool ret = GetTsStream(&pSrc, pdwSize, pdwRemain);
		if (ret && pSrc && pDst && *pdwSize)
			memcpy(pDst, pSrc, *pdwSize);
		return ret;
	}

	const bool GetTsStream(uint8_t** ppDst, uint32_t* pdwSize, uint32_t* pdwRemain) override
	{
		std::lock_guard<std::mutex> lock(m_Mutex);

		++m_CallCount;
		const bool bLogThis = m_CallCount <= 10 || (m_CallCount % 200) == 0;
		if (bLogThis)
			DLog(L"GetTsStream: call #%llu m_File=%p m_Eof=%d", m_CallCount, m_File, m_Eof);

		if (!ppDst || !pdwSize || !pdwRemain)
			return false;

		*ppDst = nullptr;
		*pdwSize = 0;
		*pdwRemain = 0;

		if (!m_File || m_Eof)
			return false;

		// Respect the same pacing budget as WaitTsStream() so a fast poller
		// can't just blast through the whole file in one go.
		const auto now = std::chrono::steady_clock::now();
		const double elapsedSec = std::chrono::duration<double>(now - m_StartTick).count();
		const double budgetBytes = elapsedSec * m_BytesPerSec;
		double allowed = budgetBytes - static_cast<double>(m_BytesDelivered);
		if (allowed <= 0) {
			if (bLogThis)
				DLog(L"GetTsStream: no budget yet (elapsedSec=%.3f allowed=%.0f)", elapsedSec, allowed);
			return false;
		}

		const size_t toRead = static_cast<size_t>(std::min<double>(allowed, kChunkSize));
		const size_t nRead = fread(m_Buffer.data(), 1, toRead, m_File);
		if (nRead == 0) {
			m_Eof = true;
			DLog(L"GetTsStream: EOF reached, m_BytesDelivered=%zu", m_BytesDelivered);
			return false;
		}

		m_BytesDelivered += nRead;
		*ppDst = m_Buffer.data();
		*pdwSize = static_cast<uint32_t>(nRead);

		// pdwRemain tells the caller whether to poll again immediately (BonDriverSourceFilter
		// busy-loops at 0ms wait whenever this is nonzero) or back off for ~10ms. Report how
		// much of *our pacing budget* is still unspent right now, not the file's total size
		// left to read - on a multi-GB file the latter is always >0xFFFFFFFF early on, which
		// pegs a CPU core spinning GetTsStream() forever instead of pacing like a live tuner.
		const double remainingBudget = budgetBytes - static_cast<double>(m_BytesDelivered);
		*pdwRemain = remainingBudget > 0
			? static_cast<uint32_t>(std::min<double>(remainingBudget, 0xFFFFFFFF))
			: 0;

		if (bLogThis)
			DLog(L"GetTsStream: delivered nRead=%zu pdwRemain=%u", nRead, *pdwRemain);

		return true;
	}

	void PurgeTsStream(void) override
	{
	}

	void Release(void) override
	{
		CloseTuner();
		delete this;
	}

	// IBonDriver2
	const char* GetTunerName(void) override
	{
		return "BonDriver_File_MMTS";
	}

	const bool IsTunerOpening(void) override
	{
		return false;
	}

	const char* EnumTuningSpace(const uint32_t dwSpace) override
	{
		return dwSpace == 0 ? "File" : nullptr;
	}

	const char* EnumChannelName(const uint32_t dwSpace, const uint32_t dwChannel) override
	{
		return dwChannel == 0 ? "File" : nullptr;
	}

	const bool SetChannel(const uint32_t dwSpace, const uint32_t dwChannel) override
	{
		// Accept whatever TVTest asks for - there's only the one file to play
		// back regardless of what channel/transponder it thinks it's tuning.
		DLog(L"SetChannel(2-arg): dwSpace=%u dwChannel=%u", dwSpace, dwChannel);
		return true;
	}

	const uint32_t GetCurSpace(void) override
	{
		return 0;
	}

	const uint32_t GetCurChannel(void) override
	{
		return 0;
	}

private:
	HMODULE m_hModule;
	std::mutex m_Mutex;
	FILE* m_File = nullptr;
	std::vector<uint8_t> m_Buffer;
	int64_t m_FileSize = 0;
	size_t m_BytesDelivered = 0;
	double m_BytesPerSec = (kDefaultBitrateMbps * 1'000'000.0) / 8.0;
	std::chrono::steady_clock::time_point m_StartTick;
	bool m_Eof = false;
	uint64_t m_CallCount = 0;
};

static HMODULE g_hModule = nullptr;

extern "C" __declspec(dllexport) IBonDriver* CreateBonDriver()
{
	DLog(L"CreateBonDriver: called, g_hModule=%p", g_hModule);
	return new CBonDriverFileMMTS(g_hModule);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReasonForCall, LPVOID)
{
	if (ulReasonForCall == DLL_PROCESS_ATTACH) {
		g_hModule = hModule;
		DLog(L"DllMain: DLL_PROCESS_ATTACH, hModule=%p", hModule);
	}
	return TRUE;
}

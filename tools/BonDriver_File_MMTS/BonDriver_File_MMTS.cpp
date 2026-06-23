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
#include <fstream>
#include <sstream>

#include "../../src/IBonDriver2.h"

namespace {

constexpr DWORD kChunkSize = 188 * 1024; // arbitrary, not MMT-TLV-packet-aligned
constexpr double kDefaultBitrateMbps = 100.0; // pacing cap, above typical 8K bitrates

} // namespace

static HMODULE g_hModule = nullptr;

namespace {

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

// GetPrivateProfileStringW() decodes ini files using the system ANSI code
// page (Shift-JIS on a Japanese system) unless the file has a UTF-16 LE BOM.
// A plain UTF-8 ini (the default for most text editors today) containing
// non-ASCII characters - e.g. a Japanese recording filename in FilePath= -
// comes out garbled, which then fails to open as a file path. Parse the
// handful of key=value lines we care about ourselves, decoding explicitly
// as UTF-8, instead of relying on that legacy behavior.
std::wstring ReadIniStringUtf8(
	const std::wstring &iniPath, const wchar_t *pSection, const wchar_t *pKey, const wchar_t *pDefault)
{
	std::ifstream file(iniPath, std::ios::binary);
	if (!file)
		return pDefault;

	std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

	if (content.size() >= 3
			&& static_cast<unsigned char>(content[0]) == 0xEF
			&& static_cast<unsigned char>(content[1]) == 0xBB
			&& static_cast<unsigned char>(content[2]) == 0xBF) {
		content.erase(0, 3);
	}

	char sectionUtf8[256] = {};
	char keyUtf8[256] = {};
	::WideCharToMultiByte(CP_UTF8, 0, pSection, -1, sectionUtf8, sizeof(sectionUtf8), nullptr, nullptr);
	::WideCharToMultiByte(CP_UTF8, 0, pKey, -1, keyUtf8, sizeof(keyUtf8), nullptr, nullptr);
	const std::string targetSection = std::string("[") + sectionUtf8 + "]";
	const std::string targetKey = keyUtf8;

	std::istringstream stream(content);
	std::string line;
	bool inTargetSection = false;

	while (std::getline(stream, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
			line.pop_back();

		const size_t start = line.find_first_not_of(" \t");
		if (start == std::string::npos)
			continue;
		const std::string trimmed = line.substr(start);

		if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#')
			continue;

		if (trimmed.front() == '[') {
			inTargetSection = (trimmed == targetSection);
			continue;
		}

		if (!inTargetSection)
			continue;

		const size_t eq = trimmed.find('=');
		if (eq == std::string::npos)
			continue;

		std::string key = trimmed.substr(0, eq);
		while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
			key.pop_back();

		if (key != targetKey)
			continue;

		std::string value = trimmed.substr(eq + 1);
		if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
			value = value.substr(1, value.size() - 2);

		const int WLen = ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
		if (WLen <= 0)
			return pDefault;
		std::wstring result(static_cast<size_t>(WLen - 1), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), WLen);
		return result;
	}

	return pDefault;
}


// Only logs when BonDriver_File_MMTS.ini has [BonDriver_File_MMTS]
// DebugLogPath= set. When absent, the cached path stays empty and every
// subsequent call returns immediately without touching _wfopen_s.
const std::wstring &GetDebugLogPath()
{
	static const std::wstring s_Path = [] {
		return ReadIniStringUtf8(
			GetIniPathNextToThisDll(g_hModule), L"BonDriver_File_MMTS", L"DebugLogPath", L"");
	}();

	return s_Path;
}


void DLog(const wchar_t *pFormat, ...)
{
	const std::wstring &LogPath = GetDebugLogPath();
	if (LogPath.empty())
		return;

	FILE *fp = nullptr;
	if (_wfopen_s(&fp, LogPath.c_str(), L"a, ccs=UTF-8") != 0 || fp == nullptr)
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


std::wstring GetDantto4kExePathNextToThisDll(HMODULE hModule)
{
	wchar_t path[MAX_PATH] = {};
	::GetModuleFileNameW(hModule, path, MAX_PATH);
	std::wstring s = path;
	const size_t slash = s.find_last_of(L"\\/");
	s = (slash != std::wstring::npos) ? s.substr(0, slash + 1) : std::wstring();
	s += L"dantto4k.exe";
	return s;
}

// Runs `dantto4k.exe --probe-bitrate <filePath>` and parses the "BITRATE_MBPS="
// line it prints to stdout. This lets BitrateMbps be auto-detected from the
// file's own MPU presentation timestamps instead of requiring a manually
// configured (and easily wrong/stale) value in the ini - a mismatched manual
// value paces playback too fast or too slow relative to real decode speed,
// which overflows/starves TSSourceFilter's queue and looks like periodic
// multi-second video freezes. Returns 0.0 on any failure; the caller should
// fall back to a sane default in that case.
double ProbeBitrateMbps(const std::wstring &exePath, const std::wstring &filePath)
{
	SECURITY_ATTRIBUTES saAttr{};
	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;

	HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
	if (!::CreatePipe(&hReadPipe, &hWritePipe, &saAttr, 0)) {
		DLog(L"ProbeBitrateMbps: CreatePipe failed, GetLastError=%lu", ::GetLastError());
		return 0.0;
	}
	::SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.hStdOutput = hWritePipe;
	si.hStdError = hWritePipe;
	si.dwFlags |= STARTF_USESTDHANDLES;

	PROCESS_INFORMATION pi{};

	std::wstring cmdLine = L"\"" + exePath + L"\" --probe-bitrate \"" + filePath + L"\"";
	std::vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
	cmdLineBuf.push_back(L'\0');

	DLog(L"ProbeBitrateMbps: launching %ls", cmdLine.c_str());

	const BOOL created = ::CreateProcessW(
		nullptr, cmdLineBuf.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

	::CloseHandle(hWritePipe);

	if (!created) {
		DLog(L"ProbeBitrateMbps: CreateProcessW failed, GetLastError=%lu", ::GetLastError());
		::CloseHandle(hReadPipe);
		return 0.0;
	}

	std::string output;
	char buf[4096];
	DWORD bytesRead = 0;
	while (::ReadFile(hReadPipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
		output.append(buf, bytesRead);
	}
	::CloseHandle(hReadPipe);

	::WaitForSingleObject(pi.hProcess, 60000); // up to 60s for a large file scan
	DWORD exitCode = 1;
	::GetExitCodeProcess(pi.hProcess, &exitCode);
	::CloseHandle(pi.hProcess);
	::CloseHandle(pi.hThread);

	DLog(L"ProbeBitrateMbps: exitCode=%lu outputLen=%zu", exitCode, output.size());

	if (exitCode != 0)
		return 0.0;

	const std::string key = "BITRATE_MBPS=";
	const size_t pos = output.find(key);
	if (pos == std::string::npos) {
		DLog(L"ProbeBitrateMbps: BITRATE_MBPS not found in dantto4k output");
		return 0.0;
	}

	try {
		const double mbps = std::stod(output.substr(pos + key.size()));
		DLog(L"ProbeBitrateMbps: parsed mbps=%.3f", mbps);
		return mbps;
	}
	catch (...) {
		DLog(L"ProbeBitrateMbps: failed to parse mbps value");
		return 0.0;
	}
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
		wcsncpy_s(
			filePath,
			ReadIniStringUtf8(path, L"BonDriver_File_MMTS", L"FilePath", L"").c_str(),
			_TRUNCATE);
		DLog(L"OpenTuner: filePath=%ls", filePath);
		if (filePath[0] == L'\0') {
			::OutputDebugStringW(L"BonDriver_File_MMTS: FilePath not set in ini\n");
			DLog(L"OpenTuner: FilePath empty, returning false");
			return false;
		}

		double bitrateMbps = static_cast<double>(::GetPrivateProfileIntW(
			L"BonDriver_File_MMTS", L"BitrateMbps", 0, iniPath));
		if (bitrateMbps <= 0.0) {
			// No (or zero) BitrateMbps configured: auto-detect from the file's own
			// MPU presentation timestamps instead of silently falling back to a
			// guessed constant. A wrong manual value here paces playback at the
			// wrong speed relative to real decode speed, which looks like periodic
			// multi-second video freezes downstream in TVTest - this is exactly
			// the bug that motivated adding auto-detection.
			DLog(L"OpenTuner: BitrateMbps not set (or 0) in ini, auto-detecting...");
			const std::wstring exePath = GetDantto4kExePathNextToThisDll(m_hModule);
			const double detected = ProbeBitrateMbps(exePath, filePath);
			bitrateMbps = (detected > 0.0) ? detected : kDefaultBitrateMbps;
			DLog(L"OpenTuner: auto-detect result=%.3f, using bitrateMbps=%.3f", detected, bitrateMbps);
		}
		m_BytesPerSec = (bitrateMbps * 1'000'000.0) / 8.0;
		DLog(L"OpenTuner: bitrateMbps=%.3f BytesPerSec=%.0f", bitrateMbps, m_BytesPerSec);

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

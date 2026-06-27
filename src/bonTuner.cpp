#include "bonTuner.h"
#include <iostream>
#include <mutex>
#include "config.h"
#include "bonDriverContext.h"
#include "mmtsRecorder.h"

bool CBonTuner::init() {
	HINSTANCE hBonDriverDLL = LoadLibraryA(config.bondriverPath.c_str());
	if (!hBonDriverDLL) {
		std::cerr << "Failed to load BonDriver: " << std::showbase << std::hex << GetLastError() << std::endl;
		return false;
	}

	IBonDriver2* (*CreateBonDriver)();
	CreateBonDriver = (IBonDriver2 * (*)())GetProcAddress(hBonDriverDLL, "CreateBonDriver");

	if (!CreateBonDriver) {
		FreeLibrary(hBonDriverDLL);
		hBonDriverDLL = NULL;

		std::cerr << "Failed to get address CreateBonDriver()" << std::endl;
		return false;
	}
	
	pBonDriver2 = CreateBonDriver();

	if (!pBonDriver2) {
		FreeLibrary(hBonDriverDLL);
		hBonDriverDLL = NULL;

		std::cerr << "Failed to get IBonDriver" << std::endl;
		return false;
	}

	return true;
}

const bool CBonTuner::OpenTuner(void) {
	if (!pBonDriver2) return false;
	return pBonDriver2->OpenTuner();
}

void CBonTuner::CloseTuner(void) {
	if (!pBonDriver2) return;
	pBonDriver2->CloseTuner();
}

const bool CBonTuner::SetChannel(const uint8_t bCh) {
	return false;
}

const float CBonTuner::GetSignalLevel(void) {
	if (!pBonDriver2) return 0.0f;
	return pBonDriver2->GetSignalLevel();
}

const uint32_t CBonTuner::WaitTsStream(const uint32_t dwTimeOut) {
	if (!pBonDriver2) return 0;
	return pBonDriver2->WaitTsStream(dwTimeOut);
}

const uint32_t CBonTuner::GetReadyCount(void) {
	if (!pBonDriver2) return 0;
	std::lock_guard<std::mutex> lock(mutex);
	return pBonDriver2->GetReadyCount();
}

const bool CBonTuner::GetTsStream(uint8_t* pDst, uint32_t* pdwSize, uint32_t* pdwRemain) {
	uint8_t* pSrc = nullptr;
	bool ret = GetTsStream(&pSrc, pdwSize, pdwRemain);;
	if (*pdwSize) {
		memcpy(pDst, pSrc, *pdwSize);
	}

	return ret;
}

const bool CBonTuner::GetTsStream(uint8_t** ppDst, uint32_t* pdwSize, uint32_t* pdwRemain) {
	std::lock_guard<std::mutex> lock(mutex);
	if (!pBonDriver2) return false;

	bool ret;
	do {
		ret = pBonDriver2->GetTsStream(ppDst, pdwSize, pdwRemain);
		if (ret) {
			if (g_bonDriverContext.mmtsDumpFs && !config.decodeDump) {
                g_bonDriverContext.mmtsDumpFs->write((char*)*ppDst, *pdwSize);
			}
		
			inputBuffer.insert(inputBuffer.end(), *ppDst, *ppDst + *pdwSize);
		}
	} while(ret && *pdwRemain != 0);
	
	MmtTlv::Common::ReadStream input(inputBuffer);
	while (!input.isEof()) {
		MmtTlv::DemuxStatus status = g_bonDriverContext.demuxer.demux(input);

		if (status == MmtTlv::DemuxStatus::NotEnoughBuffer) {
			break;
		}
	}

	inputBuffer.erase(inputBuffer.begin(), inputBuffer.begin() + (inputBuffer.size() - input.leftBytes()));

	// pdwRemain tells the caller (e.g. TVTest's BonDriverSourceFilter) whether to poll
	// again immediately instead of waiting its normal poll interval. Previously this
	// always returned 0, which capped our effective throughput at one GetTsStream()
	// call per poll interval regardless of how much undemuxed data was still queued -
	// at 8K bitrates the underlying tuner driver's ring buffer could overflow during
	// that wait, corrupting MMT/TLV data (manifesting as HEVC decode errors downstream)
	// even though TS continuity counters stayed intact (we regenerate them on remux).
	if (g_bonDriverContext.remuxOutput.empty()) {
		*pdwRemain = static_cast<uint32_t>(inputBuffer.size());
		return false;
	}

	outputBuffer = std::move(g_bonDriverContext.remuxOutput);

	*ppDst = outputBuffer.data();
	*pdwSize = static_cast<uint32_t>(outputBuffer.size());
	*pdwRemain = static_cast<uint32_t>(inputBuffer.size());
	return true;
}

void CBonTuner::PurgeTsStream(void) {
	std::lock_guard<std::mutex> lock(mutex);

	inputBuffer.clear();
	g_bonDriverContext.remuxOutput.clear();
	g_bonDriverContext.demuxer.clear();

	if (!pBonDriver2) return;
	return pBonDriver2->PurgeTsStream();
}

const char* CBonTuner::GetTunerName(void) {
	if (!pBonDriver2) return "";
	return pBonDriver2->GetTunerName();
}

const bool CBonTuner::IsTunerOpening(void) {
	if (!pBonDriver2) return false;
	return pBonDriver2->IsTunerOpening();
}

const char* CBonTuner::EnumTuningSpace(const uint32_t dwSpace) {
	if (!pBonDriver2) return nullptr;
	return pBonDriver2->EnumTuningSpace(dwSpace);
}

const char* CBonTuner::EnumChannelName(const uint32_t dwSpace, const uint32_t dwChannel) {
	if (!pBonDriver2) return nullptr;
	return pBonDriver2->EnumChannelName(dwSpace, dwChannel);
}

const bool CBonTuner::SetChannel(const uint32_t dwSpace, const uint32_t dwChannel) {
	std::lock_guard<std::mutex> lock(mutex);

	inputBuffer.clear();
	g_bonDriverContext.remuxOutput.clear();
	g_bonDriverContext.demuxer.clear();

	if (config.mmtsDumpPath != "") {
		if (g_bonDriverContext.mmtsDumpFs) {
            g_bonDriverContext.mmtsDumpFs->close();
            g_bonDriverContext.mmtsDumpFs.reset();
		}

		g_bonDriverContext.mmtsDumpFs = std::make_unique<std::ofstream>(config.mmtsDumpPath, std::ios::binary);
		if (!g_bonDriverContext.mmtsDumpFs->is_open()) {
			std::cerr << "Failed to open mmtsDumpPath: " << config.mmtsDumpPath << std::endl;
			g_bonDriverContext.mmtsDumpFs.reset();
		}
	}

	if (!pBonDriver2) return false;
	return pBonDriver2->SetChannel(dwSpace, dwChannel);
}

const uint32_t CBonTuner::GetCurSpace(void) {
	if (!pBonDriver2) return 0;
	return pBonDriver2->GetCurSpace();
}

const uint32_t CBonTuner::GetCurChannel(void) {
	if (!pBonDriver2) return 0;
	return pBonDriver2->GetCurChannel();
}

void CBonTuner::Release(void) {
	if (!pBonDriver2) return;
	return pBonDriver2->Release();
}

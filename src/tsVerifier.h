#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <vector>

// Reads an MPEG-2 TS back and checks it independently of the code that wrote it,
// so it deliberately uses neither TSDuck nor the remuxer. Packets may arrive in
// chunks of any size. Problems are defects of the stream and decide the exit code
// of --verify. Warnings are departures from a limit that players tolerate.
class TsVerifier {
public:
	struct PidStats {
		uint64_t packets{0};
		uint64_t ccErrors{0};
		uint64_t duplicates{0};
		uint64_t sections{0};
		uint64_t crcErrors{0};
		uint64_t incompleteSections{0};
		uint64_t cutCarouselSections{0};
		uint64_t pes{0};
		uint64_t pesBytes{0};
		uint64_t pesHeaderErrors{0};
		uint64_t pesLengthErrors{0};
		uint64_t frameErrors{0};
		uint64_t missingTimestamps{0};
		uint64_t timestampsBackward{0};
		uint64_t timestampGaps{0};
		uint64_t timestampSpan90k{0};
		uint64_t largestTimestampGap90k{0};
		uint64_t pcrCount{0};
		uint64_t pcrBackward{0};
		uint64_t pcrOverLimit{0};
		uint64_t pcrOverWarning{0};
		uint64_t largestPcrInterval90k{0};
		uint64_t pcrSpan90k{0};
		uint8_t streamType{0};
		int componentTag{-1};
		bool listedInPmt{false};
	};

	struct Totals {
		uint64_t packets{0};
		uint64_t nullPackets{0};
		uint64_t syncLosses{0};
		uint64_t transportErrors{0};
		uint64_t malformedPackets{0};
		uint64_t sections{0};
	};

	static constexpr uint64_t kPcrWarningInterval90k = 9000;  // 100 ms, ISO/IEC 13818-1
	static constexpr uint64_t kPcrProblemInterval90k = 90000; // 1 s
	static constexpr uint64_t kTimestampGap90k = 90000;       // 1 s

	TsVerifier();
	~TsVerifier();

	void feed(const uint8_t* data, size_t size);

	const Totals& getTotals() const { return totals; }
	// nullptr when the PID never appeared.
	const PidStats* getPid(uint16_t pid) const;
	uint64_t countProblems() const;
	uint64_t countWarnings() const;
	void print(std::ostream& out) const;

private:
	enum class PidKind : uint8_t { Unknown, Section, Pes };

	struct PidState {
		PidStats stats;
		PidKind kind{PidKind::Unknown};
		bool haveCc{false};
		uint8_t lastCc{0};
		bool lastWasDuplicate{false};
		std::vector<uint8_t> section;
		bool sectionOpen{false};
		std::vector<uint8_t> pesHeader;
		bool pesOpen{false};
		bool pesDamaged{false};
		bool pesParsed{false};
		bool pesAligned{false};
		bool carousel{false};
		bool frameChecked{false};
		size_t pesPayloadOffset{0};
		uint32_t pesDeclaredLength{0};
		uint64_t pesReceived{0};
		bool haveTimestamp{false};
		uint64_t lastTimestamp{0};
		bool havePcr{false};
		uint64_t lastPcr{0};
	};

	struct Program {
		uint16_t pmtPid{0};
		bool pmtSeen{false};
		uint16_t pcrPid{0x1FFF};
	};

	PidState& state(uint16_t pid);
	void processPacket(const uint8_t* packet);
	void processPcr(PidState& st, const uint8_t* adaptationFlags);
	void processSectionPayload(uint16_t pid, PidState& st, const uint8_t* payload, size_t size, bool unitStart);
	void appendSectionData(uint16_t pid, PidState& st, const uint8_t* data, size_t size);
	void processSection(uint16_t pid, PidState& st, const uint8_t* section, size_t size);
	void processPat(const uint8_t* section, size_t size);
	void processPmt(uint16_t pid, const uint8_t* section, size_t size);
	void processPesPayload(PidState& st, const uint8_t* payload, size_t size, bool unitStart);
	void closePes(PidState& st);
	void parsePesHeader(PidState& st);
	void checkPesFrame(PidState& st);
	void noteTimestamp(PidState& st, uint64_t timestamp);

	std::array<std::unique_ptr<PidState>, 8192> pids;
	std::vector<uint8_t> carry;
	bool inSyncLoss{false};
	Totals totals;
	std::map<uint16_t, Program> programs;
	std::map<uint8_t, uint64_t> tableCounts;
	bool patSeen{false};
};

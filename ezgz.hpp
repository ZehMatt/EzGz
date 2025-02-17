#ifndef EZGZ_HPP
#define EZGZ_HPP

#include <iostream>
#include <array>
#include <cstring>
#include <span>
#include <fstream>
#include <charconv>
#include <vector>
#include <numeric>
#include <optional>
#include <functional>
#include <variant>
#include <memory>

namespace EzGz {

template <typename T>
concept DecompressionSettings = std::constructible_from<typename T::Checksum> && requires(typename T::Checksum checksum) {
	int(T::maxOutputBufferSize);
	int(T::minOutputBufferSize);
	int(T::inputBufferSize);
	int(checksum());
	int(checksum(std::span<const uint8_t>()));
	bool(T::verifyChecksum);
};

struct NoChecksum { // Noop
	int operator() () { return 0; }
	int operator() (std::span<const uint8_t>) { return 0; }
};

struct MinDecompressionSettings {
	constexpr static int maxOutputBufferSize = 32768 * 2 + 258;
	constexpr static int minOutputBufferSize = std::min(32768, maxOutputBufferSize / 2); // Max offset + max copy size
	constexpr static int inputBufferSize = 33000;
	using Checksum = NoChecksum;
	constexpr static bool verifyChecksum = false;
};

namespace Detail {

constexpr std::array<uint32_t, 256> generateBasicCrc32LookupTable() {
	constexpr uint32_t reversedPolynomial = 0xedb88320;
	std::array<uint32_t, 256> result = {};
	for (int i = 0; i < std::ssize(result); i++) {
		result[i] = i;
		for (auto j = 0; j < 8; j++)
			result[i] = (result[i] >> 1) ^ ((result[i] & 0x1) * reversedPolynomial);
	}
	return result;
}

constexpr std::array<uint32_t, 256> basicCrc32LookupTable = generateBasicCrc32LookupTable();

static constexpr std::array<uint32_t, 256> generateNextCrc32LookupTableSlice(const std::array<uint32_t, 256>& previous) {
	std::array<uint32_t, 256> result = {};
	for (int i = 0; i < std::ssize(result); i++) {
		result[i] = (previous[i] >> 8) ^ (basicCrc32LookupTable[previous[i] & 0xff]);
	}
	return result;
}

template <int Slice>
struct CrcLookupTable {
	constexpr static std::array<uint32_t, 256> data = generateNextCrc32LookupTableSlice(CrcLookupTable<Slice - 1>::data);
};

template <>
struct CrcLookupTable<0> {
	constexpr static const std::array<uint32_t, 256> data = basicCrc32LookupTable;
};
}

class LightCrc32 {
	uint32_t state = 0xffffffffu;

public:
	uint32_t operator() () { return ~state; }
	uint32_t operator() (std::span<const uint8_t> input) {
		for (auto it : input) {
			const uint8_t tableIndex = (state ^ it);
			state = (state >> 8) ^ Detail::basicCrc32LookupTable[tableIndex];
		}
		return ~state; // Invert all bits at the end
	}
};

// Inspired by https://create.stephan-brumme.com/crc32/
class FastCrc32 {
	uint32_t state = 0xffffffffu;

public:
	uint32_t operator() () { return ~state; }
	uint32_t operator() (std::span<const uint8_t> input) {
		constexpr int chunkSize = 16;
		constexpr std::array<const std::array<uint32_t, 256>, chunkSize> lookupTables = {
			Detail::CrcLookupTable<0>::data, Detail::CrcLookupTable<1>::data, Detail::CrcLookupTable<2>::data, Detail::CrcLookupTable<3>::data,
			Detail::CrcLookupTable<4>::data, Detail::CrcLookupTable<5>::data, Detail::CrcLookupTable<6>::data, Detail::CrcLookupTable<7>::data,
			Detail::CrcLookupTable<8>::data, Detail::CrcLookupTable<9>::data, Detail::CrcLookupTable<10>::data, Detail::CrcLookupTable<11>::data,
			Detail::CrcLookupTable<12>::data, Detail::CrcLookupTable<13>::data, Detail::CrcLookupTable<14>::data, Detail::CrcLookupTable<15>::data};

		ssize_t position = 0;
		for ( ; position + chunkSize < std::ssize(input); position += chunkSize) {
			union {
				std::array<uint8_t, sizeof(state)> bytes;
				uint32_t number = 0;
			} stateBytes;
			stateBytes.number = state;
			if constexpr (std::endian::native == std::endian::big) {
				for (int i = 0; i < std::ssize(stateBytes.bytes) / 2; i++) {
					std::swap(stateBytes.bytes[i], stateBytes.bytes[std::ssize(stateBytes.bytes) - 1 - i]);
				}
			}
			uint32_t firstBytes = 0;
			memcpy(&firstBytes, &input[position], sizeof(firstBytes));
			stateBytes.number ^= firstBytes;

			state = 0;
			for (int i = 0; i < std::ssize(stateBytes.bytes); i++) {
				state ^= lookupTables[chunkSize - 1 - i][stateBytes.bytes[i]];
			}
			for (int i = std::ssize(stateBytes.bytes); i < chunkSize; i++) {
				state ^= lookupTables[chunkSize - 1 - i][input[position + i]];
			}
		}

		for ( ; position < std::ssize(input); position++) {
			const uint8_t tableIndex = (state ^ input[position]);
			state = (state >> 8) ^ Detail::basicCrc32LookupTable[tableIndex];
		}
		return ~state; // Invert all bits at the end
	}
};

struct DefaultDecompressionSettings : MinDecompressionSettings {
	constexpr static int maxOutputBufferSize = 100000;
	constexpr static int inputBufferSize = 100000;
	using Checksum = FastCrc32;
	constexpr static bool verifyChecksum = true;
};

namespace Detail {

static constexpr std::array<uint8_t, 19> codeCodingReorder = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// Provides access to input stream as chunks of contiguous data
template <DecompressionSettings Settings>
class ByteInput {
	std::array<uint8_t, Settings::inputBufferSize + sizeof(uint32_t)> buffer = {};
	std::function<int(std::span<uint8_t> batch)> readMore;
	int position = 0;
	int filled = 0;

	int refillSome() {
		if (position > std::ssize(buffer) / 2) {
			filled -= position;
			memmove(buffer.data(), &buffer[position], filled);
			position = 0;
		}
		int added = readMore(std::span<uint8_t>(buffer.begin() + filled, buffer.end()));
		filled += added;
		return added;
	}

	void ensureSize(int bytes) {
		while ((position) + bytes > filled) [[unlikely]] {
			int added = refillSome();
			if (added == 0) {
				throw std::runtime_error("Unexpected end of stream");
			}
		}
	}

public:
	ByteInput(std::function<int(std::span<uint8_t> batch)> readMoreFunction) : readMore(readMoreFunction) {}

	// Note: May not get as many bytes as necessary, would need to be called multiple times
	std::span<const uint8_t> getRange(int size) {
		if (position + size >= filled) {
			refillSome();
		}
		ssize_t start = position;
		int available = std::min<int>(size, filled - start);
		position += available;
		return {buffer.begin() + start, buffer.begin() + start + available};
	}

	uint64_t getBytes(int amount) {
		return getInteger<int64_t>(amount);
	}

	template <typename IntType>
	uint64_t getInteger(int bytes = sizeof(IntType)) {
		IntType result = 0;
		ensureSize(bytes);
		memcpy(&result, &buffer[position], bytes);
		position += bytes;
		return result;
	}

	// Can return only up to the size of the last read
	void returnBytes(int amount) {
		position -= amount;
	}

	template <int MaxTableSize>
	auto encodedTable(int realSize, const std::array<uint8_t, 256>& codeCodingLookup, const std::array<uint8_t, codeCodingReorder.size()>& codeCodingLengths);
};

template <typename T>
concept ByteReader = requires(T reader) {
	reader.returnBytes(1);
	std::span<const uint8_t>(reader.getRange(6));
};

// Provides optimised access to data from a ByteInput by bits
template <ByteReader ByteInputType>
class BitReader {
	ByteInputType* input;
	int bitsLeft = 0;
	uint64_t data = 0; // Invariant - lowest bit is the first valid
	static constexpr int minimumBits = 16; // The specification doesn't require any reading by bits that are longer than 16 bits

	void refillIfNeeded() {
		if (bitsLeft < minimumBits) {
			std::span<const uint8_t> added = input->getRange(sizeof(data) - (minimumBits / 8));
			union {
				std::array<uint8_t, sizeof(uint64_t)> bytes;
				uint64_t number = 0;
			} dataAdded;
			if constexpr (std::endian::native == std::endian::little) {
				memcpy(dataAdded.bytes.data(), added.data(), std::ssize(added));
			} else if constexpr (std::endian::native == std::endian::big) {
				for (int i = 0; i < std::ssize(added); i++) {
					dataAdded.bytes[sizeof(data) - i] = added[i];
				}
			} else static_assert(std::endian::native == std::endian::big || std::endian::native == std::endian::little);
			dataAdded.number <<= bitsLeft;
			data += dataAdded.number;
			bitsLeft += (added.size() << 3);
		}
	}

	static constexpr std::array<uint16_t, 17> upperRemovals = {0x0000, 0x0001, 0x0003, 0x0007, 0x000f, 0x001f, 0x003f, 0x007f, 0x00ff,
			0x01ff, 0x03ff, 0x07ff, 0x0fff, 0x1fff, 0x3fff, 0x7fff, 0xffff};

	static constexpr std::array<uint8_t, 256> reversedBytes = {0x0, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0, 0x10,
			0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0, 0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8, 0x18, 0x98, 0x58,
			0xd8, 0x38, 0xb8, 0x78, 0xf8, 0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4, 0x14, 0x94, 0x54, 0xd4, 0x34,
			0xb4, 0x74, 0xf4, 0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec, 0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c,
			0xfc, 0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2, 0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2, 0x0a,
			0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea, 0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa, 0x6, 0x86, 0x46,
			0xc6, 0x26, 0xa6, 0x66, 0xe6, 0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6, 0x0e, 0x8e, 0x4e, 0xce, 0x2e,
			0xae, 0x6e, 0xee, 0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe, 0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61,
			0xe1, 0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1, 0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9, 0x19,
			0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9, 0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5, 0x15, 0x95, 0x55,
			0xd5, 0x35, 0xb5, 0x75, 0xf5, 0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed, 0x1d, 0x9d, 0x5d, 0xdd, 0x3d,
			0xbd, 0x7d, 0xfd, 0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3, 0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73,
			0xf3, 0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb, 0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb, 0x07,
			0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7, 0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7, 0x0f, 0x8f, 0x4f,
			0xcf, 0x2f, 0xaf, 0x6f, 0xef, 0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff};

public:

	BitReader(ByteInputType* byteInput) : input(byteInput) {}
	BitReader(BitReader&& other) noexcept : input(other.input), bitsLeft(other.bitsLeft), data(other.data) {
		other.input = nullptr;
	}
	BitReader(const BitReader&) = delete;
	BitReader& operator=(BitReader&& other) {
		if (input)
			input->returnBytes(bitsLeft >> 3);
		input = other.input;
		other.input = nullptr;
		bitsLeft = other.bitsLeft;
		data = other.data;
		return *this;
	}
	BitReader& operator=(const BitReader&) = delete;
	~BitReader() {
		if (input)
			input->returnBytes(bitsLeft >> 3);
	}

	class BitGroup {
		BitReader* parent = nullptr;
		uint64_t data = 0;
		int bits = 0;
	public:
		constexpr BitGroup(BitReader* parent, uint64_t data, int bits) : parent(parent), data(data), bits(bits) {}

		void getMore(int amount) {
			uint64_t added = parent->getBits(amount).data;
			data <<= amount;
			data |= added;
			bits += amount;
		}

		uint64_t value() const {
			return data;
		}

		int getBitCount() const {
			return bits;
		}

		auto operator<=>(uint64_t other) {
			return data <=> other;
		}
		bool operator==(uint64_t other) {
			return data == other;
		}

	};

	// Up to 8 bits, unwanted bits blanked
	BitGroup getBits(int amount) {
		refillIfNeeded();
		uint8_t result = reversedBytes[uint8_t(data)];
		data >>= amount;
		bitsLeft -= amount;
		result >>= 8 - amount;
		return BitGroup(this, result, amount);
	}

	// Up to 16 bits, unwanted bits blanked
	uint16_t getBitsForwardOrder(int amount) {
		refillIfNeeded();
		uint16_t result = data;
		data >>= amount;
		bitsLeft -= amount;
		result &= upperRemovals[amount];
		return result;
	}

	// Provides 8 bits, the functor must return how many of them were actually wanted
	template <typename ReadAndTellHowMuchToConsume>
	void peekAByteAndConsumeSome(const ReadAndTellHowMuchToConsume& readAndTellHowMuchToConsume) {
		refillIfNeeded();
		uint8_t pulled = data;
		auto consumed = readAndTellHowMuchToConsume(reversedBytes[pulled]);
		data >>= consumed;
		bitsLeft -= consumed;
	}

	// Uses the table in the specification to determine how many bytes are copied
	int parseLongerSize(int partOfSize) {
		if (partOfSize != 31) {
			// Sizes in this range take several extra bits
			int size = partOfSize;
			int nextBits = (size - 7) >> 2;
			auto additionalBits = getBitsForwardOrder(nextBits);
			size++; // Will ease the next line
			size = ((((size & 0x3) << nextBits) | additionalBits)) + ((1 << (size >> 2)) + 3); // This is a generalisation of the size table at 3.2.5
			return size;
		} else {
			return 258; // If the value is maximmum, it means the size is 258
		}
	}

	// Uses the table in the specification to determine distance from where bytes are copied
	int parseLongerDistance(int partOfDistance) {
		int readMore = (partOfDistance - 3) >> 1;
		auto moreBits = getBitsForwardOrder(readMore);
		constexpr static std::array<int, 30> distanceOffsets = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33,
				49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
		int distance = distanceOffsets[partOfDistance - 1] + moreBits;
		return distance;
	}
};

// Handles output of decompressed data, filling bytes from past bytes and chunking. Consume needs to be called to empty it
template <DecompressionSettings Settings>
class ByteOutput {
	std::array<char, Settings::maxOutputBufferSize> buffer = {};
	int used = 0; // Number of bytes filled in the buffer (valid data must start at index 0)
	int consumed = 0; // The last byte that was returned by consume()
	bool expectsMore = true; // If we expect more data to be present
	typename Settings::Checksum checksum = {};

	void checkSize(int added = 1) {
		if (used + added > std::ssize(buffer)) [[unlikely]] {
			throw std::logic_error("Writing more bytes than available, probably an internal bug");
		}
	}

public:
	int available() {
		return buffer.size() - used;
	}

	std::span<const char> consume(const int bytesToKeep = 0) {
		// Last batch has to be handled differently
		if (!expectsMore) [[unlikely]] {
			std::span<const char> returning = std::span<const char>(buffer.data() + consumed, used - consumed);
			checksum(std::span<uint8_t>(reinterpret_cast<uint8_t*>(buffer.data() + consumed), used - consumed));

			consumed = used;
			return returning;
		}

		// Clean the space from the previous consume() call
		int bytesKept = std::min(bytesToKeep, consumed);
		int removing = consumed - bytesKept;
		int minimum = Settings::minOutputBufferSize - used + consumed; // Ensure we keep enough bytes that the operation will end with less valid data in the buffer than the mandatory minimum
		if (bytesKept < minimum) {
			bytesKept = minimum;
			removing = consumed - bytesKept;
		}
		if (removing < 0) [[unlikely]] {
			throw std::logic_error("consume() cannot keep more bytes than it provided before");
		}
		memmove(buffer.begin(), buffer.begin() + removing, used - removing);
		used -= removing;
		consumed = used; // Make everything in the buffer available (except the data returned earlier)

		// Return a next batch
		checksum(std::span<uint8_t>(reinterpret_cast<uint8_t*>(buffer.data() + bytesKept), consumed - bytesKept));
		return std::span<const char>(buffer.data() + bytesKept, consumed - bytesKept);
	}

	void addByte(char byte) {
		checkSize();
		buffer[used] = byte;
		used++;
	}

	void addBytes(std::span<const char> bytes) {
		checkSize(bytes.size());
		memcpy(buffer.data() + used, bytes.data(), bytes.size());
		used += bytes.size();
	}

	void repeatSequence(int length, int distance) {
		checkSize(length);
		int written = 0;
		while (written < length) {
			if (distance > used) {
				throw std::runtime_error("Looking back too many bytes, corrupted archive or insufficient buffer size");
			}
			int toWrite = std::min(distance, length - written);
			memmove(buffer.data() + used, buffer.data() + used - distance, toWrite);
			used += toWrite;
			written += toWrite;
		}
	}

	auto& getChecksum() {
		return checksum;
	}

	void done() { // Called when the whole buffer can be consumed because the data won't be needed anymore
		expectsMore = false;
	}
};

// Represents a table encoding Huffman codewords and can parse the stream by bits
template <int MaxSize, typename ReaderType>
class EncodedTable {
	ReaderType& reader;
	struct CodeEntry {
		uint16_t code = 0;
		uint8_t length = 0; // 0 if unused
	};
	std::array<CodeEntry, MaxSize> codes = {};
	struct CodeRemainder {
		uint8_t remainder = 0;
		uint8_t bitsLeft = 0;
		uint16_t index = 0; // bit or with 0x8000 if it's the last one in sequence
	};
	std::array<CodeRemainder, MaxSize> remainders = {};
	std::array<int, 256> codesIndex = {}; // If value is greater than MaxSize, it's a remainder at index value minus MaxSize

	static constexpr int UNINDEXED = -1;
	static constexpr int UNUSED = -2;

public:
	EncodedTable(ReaderType& reader, int realSize, std::array<uint8_t, 256> codeCodingLookup, std::array<uint8_t, codeCodingReorder.size()> codeCodingLengths)
	: reader(reader) {
		std::array<int, 17> quantities = {};

		// Read the Huffman-encoded Huffman codes
		for (int i = 0; i < realSize; ) {
			int length = 0;
			reader.peekAByteAndConsumeSome([&] (uint8_t peeked) {
				length = codeCodingLookup[peeked];
				return codeCodingLengths[length];
			});
			if (length < 16) {
				codes[i].length = length;
				i++;
				quantities[length]++;
			} else if (length == 16) {
				if (i == 0) [[unlikely]]
					throw std::runtime_error("Invalid lookback position");
				int copy = reader.getBitsForwardOrder(2) + 3;
				for (int j = i; j < i + copy; j++) {
					codes[j].length = codes[i - 1].length;
				}
				quantities[codes[i - 1].length] += copy;
				i += copy;
			} else if (length > 16) {
				int zeroCount = 0;
				if (length == 17) {
					zeroCount = reader.getBitsForwardOrder(3) + 3;
				} else {
					int sevenBitsValue = reader.getBitsForwardOrder(7);
					zeroCount = sevenBitsValue + 11;
				};
				for (int j = i; j < i + zeroCount; j++) {
					codes[j].length = 0;
				}
				i += zeroCount;
			}
		}

		for (int& it : codesIndex) it = UNUSED;

		struct UnindexedEntry {
			int quantity = 0;
			int startIndex = 0;
			int filled = 0;
		};
		std::array<UnindexedEntry, 256> unindexedEntries = {};

		// Generate the codes
		int nextCode = 0;
		for (int size = 1; size <= 16; size++) {
			if (quantities[size] > 0) {
				for (int i = 0; i <= realSize; i++) {
					if (codes[i].length == size) {
						if (nextCode >= (1 << size)) [[unlikely]]
								throw std::runtime_error("Bad Huffman encoding, run out of Huffman codes");
						codes[i].code = nextCode;
						if (size <= 8) [[likely]] {
							for (int code = codes[i].code << (8 - size); code < (codes[i].code + 1) << (8 - size); code++) {
								codesIndex[code] = i;
							}
						} else {
							int startPart = nextCode >> (size - 8);
							codesIndex[startPart] = UNINDEXED;
							unindexedEntries[startPart].quantity++;
						}
						nextCode++;
					}
				}
			}
			nextCode <<= 1;
		}

		// Calculate ranges of the longer parts
		int currentStartIndex = 0;
		for (auto& entry : unindexedEntries) {
			entry.startIndex = currentStartIndex;
			currentStartIndex += entry.quantity;
		}

		// Index the longer parts
		for (int i = 0; i < std::ssize(codes); i++) {
			CodeEntry& code = codes[i];
			if (code.length > 8) {
				int index = code.code >> (code.length - 8);
				UnindexedEntry& unindexedEntry = unindexedEntries[index];
				CodeRemainder& remainder = remainders[unindexedEntry.startIndex + unindexedEntry.filled];
				codesIndex[index] = MaxSize + unindexedEntry.startIndex;
				unindexedEntry.filled++;
				remainder.remainder = (code.code << (16 - code.length)); // The upper bits are cut
				remainder.bitsLeft = code.length - 8;
				remainder.index = i;
				if (unindexedEntry.filled == unindexedEntry.quantity)
					remainder.index |= 0x8000;
			}
		}
	}

	int readWord() {
		int word = 0;
		uint8_t firstByte = 0;
		reader.peekAByteAndConsumeSome([&] (uint8_t peeked) {
			firstByte = peeked;
			word = codesIndex[peeked];
			if (word >= MaxSize) {
				return 8;
			} else if (word == UNUSED) {
				throw std::runtime_error("Unknown Huffman code (not even first 8 bits)");
			}
			return int(codes[word].length);
		});

		// Longer codes than a byte are indexed specially
		static constexpr std::array<uint8_t, 9> startMasks = { 0x00, 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe, 0xff };
		if (word >= MaxSize) {
			reader.peekAByteAndConsumeSome([&] (uint8_t peeked) {
				for (int i = word - MaxSize; i < MaxSize * 2; i++) {
					if ((peeked & startMasks[remainders[i].bitsLeft]) == remainders[i].remainder) {
						word = remainders[i].index & 0x7fff;
						return remainders[i].bitsLeft;
					}

					if (remainders[i].index & 0x8000) {
						throw std::runtime_error("Unknown Huffman code (ending bits don't fit)");
					}
				}
				throw std::runtime_error("Unknown Huffman code (bad prefix)");
			});
		}
		return word;
	}
};

template <DecompressionSettings Settings>
template <int MaxTableSize>
auto ByteInput<Settings>::encodedTable(int realSize, const std::array<uint8_t, 256>& codeCodingLookup, const std::array<uint8_t, codeCodingReorder.size()>& codeCodingLengths) {
	return EncodedTable<MaxTableSize, ByteInput<Settings>>(*this, realSize, codeCodingLookup, codeCodingLengths);
}

// Higher level class handling the overall state of parsing. Implemented as a state machine to allow pausing when output is full.
template <DecompressionSettings Settings>
class DeflateReader {
	ByteInput<Settings>& input;
	ByteOutput<Settings>& output;

	struct CopyState {
		int copyDistance = 0;
		int copyLength = 0;

		bool restart(ByteOutput<Settings>& output) {
			int copying = std::min(output.available(), copyLength);
			output.repeatSequence(copying, copyDistance);
			copyLength -= copying;
			return (copyLength == 0);
		}
		bool copy(ByteOutput<Settings>& output, int length, int distance) {
			copyLength = length;
			copyDistance = distance;
			return restart(output);
		}
	};

	struct LiteralState {
		int bytesLeft = 0;
		LiteralState(DeflateReader* parent) {
			int length = parent->input.getBytes(2);
			int antiLength = parent->input.getBytes(2);
			if ((~length & 0xffff) != antiLength) {
				throw std::runtime_error("Corrupted data, inverted length of literal block is mismatching");
			}
			bytesLeft = length;
		}

		bool parseSome(DeflateReader* parent) {
			if (parent->output.available() > bytesLeft) {
				std::span<const uint8_t> chunk = parent->input.getRange(bytesLeft);
				parent->output.addBytes(std::span<const char>(reinterpret_cast<const char*>(chunk.data()), (chunk.size())));
				bytesLeft -= chunk.size();
				return (bytesLeft > 0);
			} else {
				std::span<const uint8_t> chunk = parent->input.getRange(parent->output.available());
				bytesLeft -= chunk.size();
				parent->output.addBytes(std::span<const char>(reinterpret_cast<const char*>(chunk.data()), (chunk.size())));
				return true;
			}
		}
	};

	struct FixedCodeState : CopyState {
		BitReader<ByteInput<Settings>> input;
		FixedCodeState(BitReader<ByteInput<Settings>>&& input) : input(std::move(input)) {}

		bool parseSome(DeflateReader* parent) {
			if (CopyState::copyLength > 0) { // Resume copying if necessary
				if (CopyState::restart(parent->output)) {
					return true; // Out of space
				}
			}
			while (parent->output.available()) {
				auto part = input.getBits(7);
				if (part == 0) {
					return false;
				}

				if (part >= 0b0011000 && part <= 0b1011111) {
					part.getMore(1);
					parent->output.addByte(uint8_t(part.value() - 0b00110000)); // Bits 0-143
				} else if (part >= 0b1100100) {
					part.getMore(2);
					parent->output.addByte(uint8_t(part.value() - (0b110010000 - 144))); // Bits 144-255
				} else {
					int size = 0;
					if (part <= 0010111) {
						// 7 bit lookback
						size = part.value() + 2; // First value means size 3
					} else {
						// 8 bit lookback, would be range 1100000 - 1100011
						part.getMore(1);
						size = part.value() + (25 - 0b11000000); // Placed after the 21 possible values of the 7 bit versions (+3)
					}
					if (size > 10) {
						size = input.parseLongerSize(size);
					}
					// Now, the distance. Short distances are simply written, longer distances are written on several more bits
					auto readingDistance = input.getBits(5);
					int distance = readingDistance.value() + 1;
					if (distance > 4) {
						distance = input.parseLongerDistance(distance);
					}
					CopyState::copy(parent->output, size, distance);
				}
			}
			return (parent->output.available() == 0);
		}
	};

	struct DynamicCodeState : CopyState {
		BitReader<ByteInput<Settings>> input;
		EncodedTable<288, BitReader<ByteInput<Settings>>> codes;
		EncodedTable<31, BitReader<ByteInput<Settings>>> distanceCode;

		DynamicCodeState(decltype(input)&& inputMoved, int codeCount, int distanceCodeCount, const std::array<uint8_t, 256>& codeCodingLookup,
						const std::array<uint8_t, codeCodingReorder.size()>& codeCodingLengths)
			: input(std::move(inputMoved))
			, codes(input, codeCount, codeCodingLookup, codeCodingLengths)
			, distanceCode(input, distanceCodeCount, codeCodingLookup, codeCodingLengths)
		{ }

		bool parseSome(DeflateReader* parent) {
			if (CopyState::copyLength > 0) { // Resume copying if necessary
				if (CopyState::restart(parent->output)) {
					return true; // Out of space
				}
			}
			while (parent->output.available()) {
				int word = codes.readWord();

				if (word < 256) {
					parent->output.addByte(word);
				} else if (word == 256) [[unlikely]] {
					break;
				} else {
					int length = word - 254;
					if (length > 10) {
						length = input.parseLongerSize(length);
					}
					int distance = distanceCode.readWord() + 1;
					if (distance > 4) {
						distance = input.parseLongerDistance(distance);
					}
					CopyState::copy(parent->output, length, distance);
				}
			}
			return (parent->output.available() == 0);
		}
	};

	std::variant<std::monostate, LiteralState, FixedCodeState, DynamicCodeState> decodingState = {};
	bool wasLast = false;

public:
	DeflateReader(ByteInput<Settings>& input, ByteOutput<Settings>& output) : input(input), output(output) {}

	// Returns whether there is more work to do
	bool parseSome() {
		while (true) {
			BitReader<ByteInput<Settings>> bitInput(nullptr);
			if (LiteralState* state = std::get_if<LiteralState>(&decodingState)) {
				if (state->parseSome(this)) {
					return true;
				}
				bitInput = BitReader<ByteInput<Settings>>(&input);
			} else if (FixedCodeState* state = std::get_if<FixedCodeState>(&decodingState)) {
				if (state->parseSome(this)) {
					return true;
				}
				bitInput = std::move(state->input);
			} else if (DynamicCodeState* state = std::get_if<DynamicCodeState>(&decodingState)) {
				if (state->parseSome(this)) {
					return true;
				}
				bitInput = std::move(state->input);
			} else {
				bitInput = BitReader<ByteInput<Settings>>(&input);
			}
			decodingState = std::monostate();

			// No decoding state
			if (wasLast) {
				output.done();
				return false;
			}
			wasLast = bitInput.getBits(1).value();
			auto compressionType = bitInput.getBits(2);
			if (compressionType == 0b00) {
				BitReader(std::move(bitInput)); // Move it to a temporary and destroy it
				decodingState.template emplace<LiteralState>(this);
			} else if (compressionType == 0b10) {
				decodingState.template emplace<FixedCodeState>(std::move(bitInput));
			} else if (compressionType == 0b01) {
				// Read lengths
				const int extraCodes = bitInput.getBitsForwardOrder(5); // Will be used later
				constexpr int maximumExtraCodes = 29;
				if (extraCodes > maximumExtraCodes) [[unlikely]] {
					throw std::runtime_error("Impossible number of extra codes");
				}
				const int distanceCodes = bitInput.getBitsForwardOrder(5) + 1; // Will be used later
				if (distanceCodes > 31) [[unlikely]] {
					throw std::runtime_error("Impossible number of distance codes");
				}
				const int codeLengthCount = bitInput.getBitsForwardOrder(4) + 4;
				if (codeLengthCount > 19) [[unlikely]]
						throw std::runtime_error("Invalid distance code count");

				// Read Huffman code lengths
				std::array<uint8_t, codeCodingReorder.size()> codeCodingLengths = {};
				for (int i = 0; i < codeLengthCount; i++) {
					codeCodingLengths[codeCodingReorder[i]] = bitInput.getBitsForwardOrder(3);
				}

				// Generate Huffman codes for lengths
				std::array<int, codeCodingReorder.size()> codeCoding = {};
				std::array<uint8_t, 256> codeCodingLookup = {};
				int nextCodeCoding = 0;
				for (int size = 1; size <= 8; size++) {
					for (int i = 0; i < std::ssize(codeCoding); i++)
						if (codeCodingLengths[i] == size) {
							codeCoding[i] = nextCodeCoding;

							for (int code = codeCoding[i] << (8 - size); code < (codeCoding[i] + 1) << (8 - size); code++) {
								codeCodingLookup[code] = i;
							}

							nextCodeCoding++;
						}
					nextCodeCoding <<= 1;
				}

				decodingState.template emplace<DynamicCodeState>(std::move(bitInput), 257 + extraCodes, distanceCodes, codeCodingLookup, codeCodingLengths);
			} else {
				throw std::runtime_error("Unknown type of block compression");
			}
		}
	}
};

} // namespace Detail

// Handles decompression of a deflate-compressed archive, no headers
template <DecompressionSettings Settings = DefaultDecompressionSettings>
std::vector<char> readDeflateIntoVector(std::function<int(std::span<uint8_t> batch)> readMoreFunction) {
	std::vector<char> result;
	Detail::ByteInput<Settings> input(readMoreFunction);
	Detail::ByteOutput<Settings> output;
	Detail::DeflateReader reader(input, output);
	bool workToDo = false;
	do {
		workToDo = reader.parseSome();
		std::span<const char> batch = output.consume();
		result.insert(result.end(), batch.begin(), batch.end());
	} while (workToDo);
	return result;
}

template <DecompressionSettings Settings = DefaultDecompressionSettings>
std::vector<char> readDeflateIntoVector(std::span<const uint8_t> allData) {
	return readDeflateIntoVector<Settings>([allData, position = 0] (std::span<uint8_t> toFill) mutable -> int {
		int filling = std::min(allData.size() - position, toFill.size());
		memcpy(toFill.data(), &allData[position], filling);
		position += filling;
		return filling;
	});
}

// Handles decompression of a deflate-compressed archive, no headers
template <DecompressionSettings Settings = DefaultDecompressionSettings>
class IDeflateArchive {
protected:
	Detail::ByteInput<Settings> input;
	Detail::ByteOutput<Settings> output;
	Detail::DeflateReader<Settings> deflateReader = {input, output};
	bool done = false;

	virtual void onFinish() {}

public:
	IDeflateArchive(std::function<int(std::span<uint8_t> batch)> readMoreFunction) : input(readMoreFunction) {}

	IDeflateArchive(const std::string& fileName) : input([file = std::make_shared<std::ifstream>(fileName, std::ios::binary)] (std::span<uint8_t> batch) mutable {
		if (!file->good()) {
			throw std::runtime_error("Can't read file");
		}
		file->read(reinterpret_cast<char*>(batch.data()), batch.size());
		int bytesRead = file->gcount();
		if (bytesRead == 0) {
			throw std::runtime_error("Truncated file");
		}
		return bytesRead;
	}) {}

	IDeflateArchive(std::span<const uint8_t> data) : input([data] (std::span<uint8_t> batch) mutable {
		int copying = std::min(batch.size(), data.size());
		if (copying == 0) {
			throw std::runtime_error("Truncated input");
		}
		memcpy(batch.data(), data.data(), copying);
		data = std::span<const uint8_t>(data.begin() + copying, data.end());
		return copying;
	}) {}

	// Returns whether there are more bytes to read
	std::optional<std::span<const char>> readSome(int bytesToKeep = 0) {
		if (done) {
			return std::nullopt;
		}
		bool moreStuffToDo = deflateReader.parseSome();
		std::span<const char> batch = output.consume(bytesToKeep);
		if (!moreStuffToDo) {
			onFinish();
			done = true;
		}
		return batch;
	}

	void readByLines(const std::function<void(std::span<const char>)> reader, char separator = '\n') {
		int keeping = 0;
		std::span<const char> batch = {};
		bool wasSeparator = false;
		while (std::optional<std::span<const char>> batchOrNot = readSome(keeping)) {
			batch = *batchOrNot;
			std::span<const char>::iterator start = batch.begin();
			for (std::span<const char>::iterator it = start; it != batch.end(); ++it) {
				if (wasSeparator) {
					wasSeparator = false;
					start = it;
				}
				if (*it == separator) {
					reader(std::span<const char>(start, it));
					wasSeparator = true;
				}
			}
			keeping = batch.end() - start;
		}
		if (keeping > 0) {
			if (wasSeparator)
				reader(std::span<const char>());
			else
				reader(std::span<const char>(batch.end() - keeping, batch.end()));
		}
	}

	void readAll(const std::function<void(std::span<const char>)>& reader) {
		while (std::optional<std::span<const char>> batch = readSome()) {
			reader(*batch);
		}
	}

	std::vector<char> readAll() {
		std::vector<char> returned;
		while (std::optional<std::span<const char>> batch = readSome()) {
			returned.insert(returned.end(), batch->begin(), batch->end());
		};
		return returned;
	}
};

enum class CreatingOperatingSystem {
	UNIX_BASED,
	WINDOWS,
	OTHER
};

// File information in the .gz file
struct IGzFileInfo {
	int32_t modificationTime = 0;
	CreatingOperatingSystem operatingSystem = CreatingOperatingSystem::OTHER;
	bool fastestCompression = false;
	bool densestCompression = false;
	std::optional<std::vector<uint8_t>> extraData = {};
	std::string name;
	std::string comment;
	bool probablyText = false;

	template <DecompressionSettings Settings>
	IGzFileInfo(Detail::ByteInput<Settings>& input) {
		typename Settings::Checksum checksum = {};
		auto check = [&checksum] (auto num) -> uint32_t {
			std::array<uint8_t, sizeof(num)> asBytes;
			memcpy(asBytes.data(), &num, asBytes.size());
			return checksum(asBytes);
		};
		if (input.template getInteger<uint8_t>() != 0x1f || input.template getInteger<uint8_t>() != 0x8b || input.template getInteger<uint8_t>() != 0x08)
			throw std::runtime_error("Trying to parse something that isn't a Gzip archive");
		check(0x1f);
		check(0x8b);
		check(0x08);
		uint8_t flags = input.template getInteger<uint8_t>();
		check(flags);
		modificationTime = input.template getInteger<uint32_t>();
		check(modificationTime);
		uint8_t extraFlags = input.template getInteger<uint8_t>();
		check(extraFlags);

		if (extraFlags == 4) {
			densestCompression = true;
		} else if (extraFlags == 8) {
			fastestCompression = true;
		}
		uint8_t creatingOperatingSystem = input.template getInteger<uint8_t>(); // Was at input[9]
		check(creatingOperatingSystem);
		if (creatingOperatingSystem == 0) {
			operatingSystem = CreatingOperatingSystem::WINDOWS;
		} else if (creatingOperatingSystem == 3) {
			operatingSystem = CreatingOperatingSystem::UNIX_BASED;
		}

		if (flags & 0x04) {
			uint16_t extraHeaderSize = input.template getInteger<uint16_t>();
			check(extraHeaderSize);
			int readSoFar = 0;
			extraData.emplace();
			while (readSoFar < extraHeaderSize) {
				std::span<const uint8_t> taken = input.getRange(extraHeaderSize - readSoFar);
				checksum(taken);
				extraData->insert(extraData->end(), taken.begin(), taken.end());
				readSoFar += taken.size();
			}
		}
		if (flags & 0x08) {
			char letter = input.template getInteger<uint8_t>();
			check(letter);
			while (letter != '\0') {
				name += letter;
				letter = input.template getInteger<uint8_t>();
				check(letter);
			}
		}
		if (flags & 0x10) {
			char letter = input.template getInteger<uint8_t>();
			check(letter);
			while (letter != '\0') {
				name += letter;
				letter = input.template getInteger<uint8_t>();
				check(letter);
			}
		}
		if (flags & 0x01) {
			probablyText = true;
		}
		if (flags & 0x02) {
			uint16_t expectedHeaderCrc = input.template getInteger<uint16_t>();
			check(expectedHeaderCrc);
			uint16_t realHeaderCrc = checksum();
			if (expectedHeaderCrc != realHeaderCrc)
				throw std::runtime_error("Gzip archive's headers crc32 checksum doesn't match the actual header's checksum");
		}
	}
};

// Parses a .gz file, only takes care of the header, the rest is handled by its parent class IDeflateArchive
template <DecompressionSettings Settings = DefaultDecompressionSettings>
class IGzFile : public IDeflateArchive<Settings> {
	IGzFileInfo parsedHeader;
	using Deflate = IDeflateArchive<Settings>;

	void onFinish() override {
		uint32_t expectedCrc = Deflate::input.template getInteger<uint32_t>();
		if constexpr(Settings::verifyChecksum) {
			auto realCrc = Deflate::output.getChecksum()();
			if (expectedCrc != realCrc)
				throw std::runtime_error("Gzip archive's crc32 checksum doesn't match the calculated checksum");
		}
	}

public:
	IGzFile(std::function<int(std::span<uint8_t> batch)> readMoreFunction) : Deflate(readMoreFunction), parsedHeader(Deflate::input) {}
	IGzFile(const std::string& fileName) : Deflate(fileName), parsedHeader(Deflate::input) {}
	IGzFile(std::span<const uint8_t> data) : Deflate(data), parsedHeader(Deflate::input) {}

	const IGzFileInfo& info() const {
		return parsedHeader;
	}
};

namespace Detail {
template <DecompressionSettings Settings = DefaultDecompressionSettings>
class IGzStreamBuffer : public std::streambuf {
	IGzFile<Settings> inputFile;
	int bytesToKeep = 10;
	ssize_t produced = 0;
public:
	template<typename Arg>
	IGzStreamBuffer(const Arg& arg, int bytesToKeep) : inputFile(arg), bytesToKeep(bytesToKeep) {}

	int underflow() override {
		std::optional<std::span<const char>> batch = inputFile.readSome(bytesToKeep);
		if (batch.has_value()) {
			// We have to believe std::istream that it won't edit the data, otherwise it would be necessary to copy the data
			char* start = const_cast<char*>(batch->data());
			setg(start - std::min<ssize_t>(bytesToKeep, produced), start, start + batch->size());
			produced += batch->size();
			return traits_type::to_int_type(*gptr());
		} else {
			return traits_type::eof();
		}
	}

	const IGzFileInfo& info() const {
		return inputFile.info();
	}
};
}

// Using IGzFile as std::istream, configurable
template <DecompressionSettings Settings = DefaultDecompressionSettings>
class BasicIGzStream : private Detail::IGzStreamBuffer<Settings>, public std::istream
{
public:
	// Open and read a file, always keeping the given number of characters specified in the second character (10 by default)
	BasicIGzStream(const std::string& sourceFile, int bytesToKeep = 10) : Detail::IGzStreamBuffer<Settings>(sourceFile, bytesToKeep), std::istream(this) {}
	// Read from a buffer
	BasicIGzStream(std::span<const uint8_t> data, int bytesToKeep = 10) : Detail::IGzStreamBuffer<Settings>(data, bytesToKeep),  std::istream(this) {}
	// Use a function that fills a buffer of data and returns how many bytes it wrote
	BasicIGzStream(std::function<int(std::span<uint8_t> batch)> readMoreFunction, int bytesToKeep = 10) : Detail::IGzStreamBuffer<Settings>(readMoreFunction, bytesToKeep), std::istream(this) {}
	// Read from an existing stream
	BasicIGzStream(std::istream& input, int bytesToKeep = 10) : Detail::IGzStreamBuffer<Settings>([&input] (std::span<uint8_t> batch) -> int {
		input.read(reinterpret_cast<char*>(batch.data()), batch.size());
		return input.gcount();
	}, bytesToKeep), std::istream(this) {}

	using Detail::IGzStreamBuffer<Settings>::info;
};

// Most obvious usage, default settings
using IGzStream = BasicIGzStream<>;

} // namespace EzGz

#endif // EZGZ_HPP

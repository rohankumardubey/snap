// SelectSAMByAlignmentTime.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <stdio.h>
#include <windows.h>


int minTimeToExclude;
int maxTimeToExclude = INT32_MAX;
CRITICAL_SECTION CriticalSection[1];
volatile _int64 NextFileOffsetToProcess = 0;
_int64 fileSize;
HANDLE blockReadyEvent;	// Set by a worker when it adds an output block, cleared by the main thread (writer) when it doesn't have the next block ready
HANDLE readerThrottle;

const char* inputFileName;

_int64 totalLinesProcessed = 0;
_int64 totalLinesSkipped = 0;

const size_t threadChunkSize = 100 * 1024 * 1024;	// How much a thread processes at at time.
const size_t readAheadAmount = (size_t)10 * 1024 * 1024 * 1024;

int cheezyLogBase2(_int64 value) // rounds down
{
	int retVal = 0;
	value /= 2; // Since 2^0 = 1; we'll also define cheezyLogBase2(x) = 0 where x<= 0.
	while (value > 0) {
		retVal++;
		value /= 2;
	}
	return retVal;
}

class Histogram {
public:
	//
	// Increment is ignored for log scale.  For log scale, it's the log of the value, not of value-minValue.
	//
	Histogram(_int64 maxKeyValue_, bool logScale_) : maxKeyValue(maxKeyValue_), logScale(logScale_), nBadValues(0)
	{
		if (logScale) {
			nBuckets = cheezyLogBase2(maxKeyValue) + 1;
		} else {
			nBuckets = maxKeyValue + 1;
		}

		buckets = new Bucket[nBuckets];
	} // ctor

	~Histogram() {
		delete[] buckets;
	}

	void mergeInto(Histogram* peer) {
		if (maxKeyValue != peer->maxKeyValue || logScale != peer->logScale || nBuckets != peer->nBuckets) {
			fprintf(stderr, "Histogram: trying to merge two non-conforming Histograms\n");
			exit(1);
		}

		for (int i = 0; i < nBuckets; i++) {
			peer->buckets[i].count += buckets[i].count;
			peer->buckets[i].totalValue += buckets[i].totalValue;
		}

		peer->nBadValues += nBadValues;
	} // mergeInto

	void addValue(int key, int value) {
		int whichBucket;
		if (key < 0) {
			whichBucket = 0;
		} else if (key > maxKeyValue) {
			whichBucket = nBuckets - 1;
		} else {
			if (logScale) {
				whichBucket = cheezyLogBase2(key);
			} else {
				whichBucket = key;
			}
		}

		_ASSERT(whichBucket >= 0 && whichBucket < nBuckets);

		buckets[whichBucket].count++;
		buckets[whichBucket].totalValue += value;
	}

	void noteBadValue()
	{
		nBadValues++;
	}

	void print(FILE* outfile, bool includeMean) 
	{
		if (includeMean) {
			fprintf(outfile, "Key (rounded down)\tcount\ttotal value\tpdf count\tpdf total\tcdf count\tcdf value\tmean value\n");
		} else {
			fprintf(outfile, "Key (rounded down)\tcount\ttotal value\tpdf count\tpdf total\tcdf count\tcdf value\n");
		}
		_int64 totalCount = 0;
		_int64 grandTotal = 0;

		for (int i = 0; i < nBuckets; i++) {
			totalCount += buckets[i].count;
			grandTotal += buckets[i].totalValue;
		}

		_int64 runningCount = 0;
		_int64 runningTotal = 0;

		for (int i = 0; i < nBuckets; i++) {
			runningCount += buckets[i].count;
			runningTotal += buckets[i].totalValue;

			if (includeMean) {
				fprintf(outfile, "%d\t%lld\t%lld\t%f\t%f\t%f\t%f\t%lld\n",
					logScale ? (1 << i) :i, buckets[i].count, buckets[i].totalValue,
					(double)buckets[i].count / totalCount, (double)buckets[i].totalValue / grandTotal,
					(double)runningCount / totalCount, (double)runningTotal / grandTotal,
					(buckets[i].count == 0) ? 0 : buckets[i].totalValue / buckets[i].count);
			} else {
				fprintf(outfile, "%d\t%lld\t%lld\t%f\t%f\t%f\t%f\n",
					logScale ? (1 << i) : i, buckets[i].count, buckets[i].totalValue,
					(double)buckets[i].count / totalCount, (double)buckets[i].totalValue / grandTotal,
					(double)runningCount / totalCount, (double)runningTotal / grandTotal);
			} // include total
		} // for each bucket

		if (nBadValues > 0) {
			fprintf(outfile, "%lld bad (negative time) values were ignored.\n", nBadValues);
		}
	} // print

private:
	int nBuckets;
	int maxKeyValue;
	bool logScale;
	_int64 nBadValues;

	struct Bucket {
		Bucket() : count(0), totalValue(0) {}

		_int64	count;
		_int64 totalValue;
	};

	Bucket* buckets;
};

const int alignmentTimeHistogramMaxValue = 2 * 1024 * 1024;	// 2-ish seconds in microseconds
const int MAPQHistogramMaxValue = 70;			// The histogram is one bigger than this, the last value being "unaligned"
const int EditDistanceHistogramMaxValue = 80;	// The histogram is one bigger than this, the last value being "unaligned"

Histogram global_alignmentTimeHistogram(alignmentTimeHistogramMaxValue, true);
Histogram global_MAPQHistogram(MAPQHistogramMaxValue + 1, false);
Histogram global_editDistanceHistogram(EditDistanceHistogramMaxValue + 1, false);


class OutputBlock {
public:
	char* data;
	size_t dataSize;
	_int64 fileOffset;	// In the input file.  This will be a multiple of threadChunkSize

	OutputBlock* next;
	OutputBlock* prev;

	OutputBlock()
	{
		data = NULL;
		dataSize = 0;
		fileOffset = -1;
		next = prev = this;
	}

	~OutputBlock()
	{
		delete[] data;
	}

	void AddToList(OutputBlock *listHeader) {
		//
		// The list is in fileOffset order.
		//

		_ASSERT(fileOffset > listHeader->fileOffset); // Else we could infinite loop.

		OutputBlock *blockToAddAfter = listHeader->prev;

		while (blockToAddAfter->fileOffset > fileOffset) {
			blockToAddAfter = blockToAddAfter->prev;
		}

		prev = blockToAddAfter;
		next = blockToAddAfter->next;
		prev->next = this;
		next->prev = this;
	}

	void remove() {
		prev->next = next;
		next->prev = prev;

		prev = next = NULL;
	}
};

OutputBlock BlocksReadyToWrite[1];


unsigned GetNumberOfProcessors()
{
	SYSTEM_INFO systemInfo[1];
	GetSystemInfo(systemInfo);

	return systemInfo->dwNumberOfProcessors;
}

DWORD WorkerThread(PVOID param) {
	_int64 linesProcessed = 0;
	_int64 linesSkipped = 0;
	
	int readBufferSize = threadChunkSize + 100000;
	char* readBuffer = new char[readBufferSize];

	Histogram alignmentTimeHistogram(alignmentTimeHistogramMaxValue, true);
	Histogram MAPQHistogram(MAPQHistogramMaxValue + 1, false);
	Histogram editDistanceHistogram(EditDistanceHistogramMaxValue + 1, false);

	HANDLE hInputFile = CreateFileA(inputFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (INVALID_HANDLE_VALUE == hInputFile) {
		fprintf(stderr, "Unable to open %s, %d\n", inputFileName, GetLastError());
		exit(1);
	}

	for (;;) {
		_int64 readOffset = InterlockedAdd64(&NextFileOffsetToProcess, threadChunkSize) - threadChunkSize;
		if (readOffset > fileSize) {
			break;
		}

		if (WAIT_OBJECT_0 != WaitForSingleObject(readerThrottle, INFINITE)) {
			fprintf(stderr, "WaitForSingleObject on throttle failed, %d\n", GetLastError());
			exit(1);
		}

		LARGE_INTEGER liFileOffset;
		liFileOffset.QuadPart = readOffset;

		if (!SetFilePointerEx(hInputFile, liFileOffset, NULL, SEEK_SET)) {
			fprintf(stderr, "Unable to SetFilePointer, %d\n", GetLastError());
			exit(1);
		}

		DWORD bytesRead;
		if (!ReadFile(hInputFile, readBuffer, readBufferSize, &bytesRead, NULL)) {
			fprintf(stderr, "ReadFile failed, %d\n", GetLastError());
			exit(1);
		}

		if (bytesRead != readBufferSize && bytesRead != (DWORD)(fileSize - readOffset)) {
			fprintf(stderr, "Read unexpected number of bytes %d != %d at offset %lld\n", bytesRead, readBufferSize, readOffset);
			exit(1);
		}

		_int64 stopOffset = readOffset + threadChunkSize;	// Run to the first \n after this offset or EOF

		OutputBlock* block = new OutputBlock;
		block->fileOffset = readOffset;
		block->data = new char[threadChunkSize + 10000];	// Won't necessarily use it all, but this is a bound (and really, a buffer overflow, but this should never have hostile input).

		const char* line = readBuffer;
		if (readOffset != 0) {
			while (line < readBuffer + bytesRead && *line != '\n') {
				line++;
			}

			line++;	// Consume the newline

			if (line >= readBuffer + bytesRead) {
				//
				// The last chunk didn't include a whole line.  We're done.
				//
				delete block;
				break;
			}
		}

		while (line <= readBuffer + threadChunkSize && line < readBuffer + bytesRead) {
			const char* endOfLine = line;
			while (endOfLine < readBuffer + bytesRead && *endOfLine != '\n') {	// Don't check for end of chunk, we always consume the first newline in the next chunk.
				endOfLine++;
			}
			endOfLine++;	// So it points at the next line

			if (*line == '@') {
				//
				// A header line.  Copy it identically.
				//
				memcpy(block->data + block->dataSize, line, endOfLine - line);
				block->dataSize += endOfLine - line;
			} else {
				linesProcessed++; // don't count headers

								  //
				// Find the flag and MAPQ fields.  flag is after the first tab and MAPQ after the 4th
				//
				const char* current = line;
				while (*current != '\t' && current < endOfLine - 1) {
					current++;
				}
				
				if (current >= endOfLine - 1) {
					fprintf(stderr, "Malformed SAM line with no tabs\n");
					continue;
				}

				int flags = atoi(current + 1);

				int nTabsSeen = 1;
				current++;

				while (nTabsSeen < 4) {
					while (*current != '\t' && current < endOfLine - 1) {
						current++;
					}

					if (current >= endOfLine - 1) {
						fprintf(stderr, "Malformed SAM line with too few tabs\n");
						exit(1);	// feh
					}

					nTabsSeen++;
					current++;
				}

				int mapq = atoi(current);

				bool unaligned = (flags & 0x4) != 0;

				int alignmentTime;
				int editDistance;

				//
				// Look for the AT tag, "\tAT:i:" and the NM tag, "\tNM:i:"
				//

				bool foundAT = false;
				bool foundNM = false;
				for (; current < endOfLine - 7; current++) {	// -7 leaves space for \tAT:i: at least one digit and then something after (tab or newline)
					if (current[0] == '\t' && current[1] == 'A' && current[2] == 'T' && current[3] == ':' && current[4] == 'i' && current[5] == ':') {
						alignmentTime = atoi(current + 6);

						if (alignmentTime < minTimeToExclude || alignmentTime > maxTimeToExclude) {
							memcpy(block->data + block->dataSize, line, endOfLine - line);
							block->dataSize += endOfLine - line;
						} else {
							linesSkipped++;
						}

						foundAT = true;
						if (foundNM || unaligned) {
							break;
						}
					} else if (!unaligned && current[0] == '\t' && current[1] == 'N' && current[2] == 'M' && current[3] == ':' && current[4] == 'i' && current[5] == ':') {
						editDistance = atoi(current + 6);

						foundNM = true;
						if (foundAT) {
							break;
						}
					}
				} // walking the line

				if (!foundAT) {
					const int bytesToPrint = 500;
					char buffer[bytesToPrint+1];
					memcpy(buffer, line, bytesToPrint);
					buffer[bytesToPrint] = '\0';
					fprintf(stderr, "Found line without an AT tag, first bit: %s\n", buffer);
					exit(1);
				}

				if (alignmentTime < 0) {
					alignmentTimeHistogram.noteBadValue();
					MAPQHistogram.noteBadValue();
					editDistanceHistogram.noteBadValue();
				} else {
					alignmentTimeHistogram.addValue(alignmentTime, alignmentTime);
					if (unaligned) {
						MAPQHistogram.addValue(MAPQHistogramMaxValue + 1, alignmentTime);
						editDistanceHistogram.addValue(EditDistanceHistogramMaxValue + 1, alignmentTime);
					} else {
						MAPQHistogram.addValue(mapq, alignmentTime);
						editDistanceHistogram.addValue(editDistance, alignmentTime);
					}
				}

			} // not a header line

			line = endOfLine;
		} // while we have data in this chunk

		if (block->dataSize > 0) {
			EnterCriticalSection(CriticalSection);
			block->AddToList(BlocksReadyToWrite);
			SetEvent(blockReadyEvent);
			if (readOffset > BlocksReadyToWrite->next->fileOffset + readAheadAmount) {
				ResetEvent(readerThrottle);
			}
			LeaveCriticalSection(CriticalSection);
		} else {
			delete block;
		}
	} // for ever

	InterlockedAdd64(&totalLinesProcessed, linesProcessed);
	InterlockedAdd64(&totalLinesSkipped, linesSkipped);

	EnterCriticalSection(CriticalSection);
	alignmentTimeHistogram.mergeInto(&global_alignmentTimeHistogram);
	MAPQHistogram.mergeInto(&global_MAPQHistogram);
	editDistanceHistogram.mergeInto(&global_editDistanceHistogram);
	LeaveCriticalSection(CriticalSection);

	return 0;	
}

void usage()
{
	fprintf(stderr, "usage: SelectSAMByAlignmentTime inputFile outputFile histogramFile minTimeToExclude {maxTimeToExclude}\n");
	exit(1);
} // usage

int main(int argc, char **argv)
{
	if (argc != 5 && argc != 6) {
		usage();
	}

	minTimeToExclude = atoi(argv[4]);
	if (argc >= 6) {
		maxTimeToExclude = atoi(argv[5]);
	}

	if (minTimeToExclude < 0 || minTimeToExclude >= maxTimeToExclude) {
		fprintf(stderr, "min time to exclude can't be negative and must be less than max time to exclude\n");
		exit(1);
	}

	inputFileName = argv[1];

	HANDLE hInputFile = CreateFileA(inputFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (INVALID_HANDLE_VALUE == hInputFile) {
		fprintf(stderr, "Unable to open %s, %d\n", inputFileName, GetLastError());
		exit(1);
	}

	LARGE_INTEGER liFileSize;
	if (!GetFileSizeEx(hInputFile, &liFileSize)) {
		fprintf(stderr, "Unable to GetFileSizeEx(), %d\n", GetLastError());
		exit(1);
	}

	CloseHandle(hInputFile);

	fileSize = liFileSize.QuadPart;

	InitializeCriticalSection(CriticalSection);

	blockReadyEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (NULL == blockReadyEvent) {
		fprintf(stderr, "Unable to CreateEvent, %d\n", GetLastError());
		exit(1);
	}

	readerThrottle = CreateEvent(NULL, TRUE, TRUE, NULL);
	if (NULL == blockReadyEvent) {
		fprintf(stderr, "Unable to CreateEvent, %d\n", GetLastError());
		exit(1);
	}


	HANDLE hOutputFile = CreateFileA(argv[2], GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == hOutputFile) {
		fprintf(stderr, "Unable to open output file %s, %d\n", argv[2], GetLastError());
		exit(1);
	}

	FILE* histogramFile;
	if (0 != fopen_s(&histogramFile, argv[3], "w")) {
		fprintf(stderr, "Unable to open histogram file '%s'\n", argv[3]);
		exit(1);
	}

	unsigned nProcessors = GetNumberOfProcessors();

	for (int i = 0; i < nProcessors; i++) {
		if (!CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL)) {
			fprintf(stderr, "CreateThread failed, %d\n", GetLastError());
			exit(1);
		}
	}

	_int64 nextBlockOffset = 0;

	_int64 startTime = GetTickCount64();

	printf("Processing file (1 dot/GB): ");
	_int64 lastDot = 0;

	while (nextBlockOffset < fileSize) {
		DWORD waitResult = WaitForSingleObject(blockReadyEvent, INFINITE);
		if (waitResult != WAIT_OBJECT_0) {
			fprintf(stderr, "WaitForSingleObject failed, %d\n", GetLastError());
			exit(1);
		}

		for (;;) {
			OutputBlock* blockToWrite = NULL;

			EnterCriticalSection(CriticalSection);
			if (BlocksReadyToWrite->next->fileOffset == nextBlockOffset) {
				blockToWrite = BlocksReadyToWrite->next;
				blockToWrite->remove();
			}

			if (blockToWrite == NULL) {
				ResetEvent(blockReadyEvent);
			} else if (blockToWrite->fileOffset + readAheadAmount > NextFileOffsetToProcess) {
				//
				// We're not too far behind.  Let the readers go.
				//
				SetEvent(readerThrottle);
			}
			LeaveCriticalSection(CriticalSection);

			if (blockToWrite == NULL) {
				break;	// Go back to sleep waiting for work
			}

			DWORD nBytesWritten;
			if (!WriteFile(hOutputFile, blockToWrite->data, blockToWrite->dataSize, &nBytesWritten, NULL)) {
				fprintf(stderr, "WriteFile failed, %d\n", GetLastError());
				exit(1);
			}

			nextBlockOffset += threadChunkSize;
			delete blockToWrite;

			if (nextBlockOffset >= lastDot + 1024 * 1024 * 1024) {
				printf(".");
				lastDot += 1024 * 1024 * 1024;
			}
		} // for ever (write run without sleeping)
	} // while we have work

	printf("\nSkipped %lld of %lld reads in %llds\n", totalLinesSkipped, totalLinesProcessed, (GetTickCount64() - startTime) / 1000);

	fprintf(histogramFile, "Histogram of alignment times:\n");
	global_alignmentTimeHistogram.print(histogramFile, false);

	fprintf(histogramFile, "\n\nHistogram of alignment time by edit distance (max value is unaligned):\n");
	global_editDistanceHistogram.print(histogramFile, true);

	fprintf(histogramFile, "\n\nHistogram of alignment time by MAPQ (max value is unaligned):\n");
	global_MAPQHistogram.print(histogramFile, true);

	fclose(histogramFile);

	CloseHandle(hOutputFile);

} // main

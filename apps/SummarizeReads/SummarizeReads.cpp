/*++

Module Name:

SummarizeReads.cpp

Abstract:

Take a set of files in SAM or BAM format, sample them, and extract the min, max and mean read length, min, max and mean 
non-N base count, and whether they're paired or single end

Authors:

Bill Bolosky, June, 2015

Environment:
`
User mode service.

Revision History:


--*/

#include "stdafx.h"
#include "SAM.h"
#include "Bam.h"
#include "Genome.h"
#include "Compat.h"
#include "Read.h"
#include "RangeSplitter.h"
#include "BigAlloc.h"
#include "FASTQ.h"
#include "Tables.h"

void usage()
{
    fprintf(stderr, "usage: SummarizeReads genomeIndex analysis_id inputFile outputFile {nReadsToSample}\n");
    fprintf(stderr, "       inputFile is a SAM or BAM file.  SummarizeReads\n");
    fprintf(stderr, "       will open the file, sample reads from the file, and then append a line to\n");
    fprintf(stderr, "       outputFile containing the min, max and mean read length, count of valid (non-N) bases per read\n");
    fprintf(stderr, "       and whether the reads are paired or single-end.\n");
    soft_exit(1);
}

ReadSupplierGenerator *readSupplierGenerator = NULL;

volatile _int64 nRunningThreads;
SingleWaiterObject allThreadsDone;
const Genome *genome;

struct ThreadContext {
    unsigned    whichThread;

    _int64 totalReads;
    _int64 minReadLength, maxReadLength, totalReadLength;
    _int64 minGoodBases, maxGoodBases, totalGoodBases;

    _int64 nCrossContigPairs, totalPairedReads, totalPairedReadDistance, maxPairedReadDistance, minPairedReadDistance, nBeyondTrackedPairedDistance;

#define THREAD_CONTEXT_PAIRED_READ_MAX_FOR_MEDIAN   5000
    _int64 nAtPairedDistance[THREAD_CONTEXT_PAIRED_READ_MAX_FOR_MEDIAN];

    bool anyPaired;
    bool allPaired;

};

bool inline isADigit(char x) {
    return x >= '0' && x <= '9';
}

int nReadsPerThread;

void
WorkerThreadMain(void *param)
{
    ThreadContext *context = (ThreadContext *)param;

    ReadSupplier *readSupplier = readSupplierGenerator->generateNewReadSupplier();

    Read *read;
    context->totalReads = context->maxReadLength = context->maxGoodBases = context->totalReadLength = context->totalGoodBases = 0;
    context->minGoodBases = context->minReadLength = context->minPairedReadDistance = 10000000000;
    context->allPaired = true;
    context->anyPaired = false;

    context->nCrossContigPairs = context->totalPairedReads = context->totalPairedReadDistance = context->maxPairedReadDistance = context->nBeyondTrackedPairedDistance = 0;

    for (int i = 0; i < THREAD_CONTEXT_PAIRED_READ_MAX_FOR_MEDIAN; i++) {
        context->nAtPairedDistance[i] = 0;
    }

    while (NULL != (read = readSupplier->getNextRead())) {
        context->minReadLength = __min(read->getDataLength(), context->minReadLength);
        context->maxReadLength = __max(read->getDataLength(), context->maxReadLength);
        context->totalReads++;
        context->totalReadLength += read->getDataLength();

        context->allPaired &= read->getOriginalSAMFlags() & SAM_MULTI_SEGMENT;
        context->anyPaired |= read->getOriginalSAMFlags() & SAM_MULTI_SEGMENT;

        int nGoodBases = 0;
        for (unsigned i = 0; i < read->getDataLength(); i++) {
            if (BASE_VALUE[read->getData()[i]] != 4) {
                nGoodBases++;
            }
        }

        context->minGoodBases = __min(nGoodBases, context->minGoodBases);
        context->maxGoodBases = __max(nGoodBases, context->maxGoodBases);
        context->totalGoodBases += nGoodBases;

        if (read->getOriginalSAMFlags() & SAM_MULTI_SEGMENT) {
            context->totalPairedReads++;

            if ((read->getOriginalSAMFlags() & SAM_UNMAPPED) == 0 && read->getOriginalPNEXT() != 0) {
                GenomeLocation alignedLocation = read->getOriginalAlignedLocation();
                
                const Genome::Contig* contig = genome->getContigAtLocation(alignedLocation);
                if (NULL != contig) {

                    if (memcmp(read->getOriginalRNEXT(), "*", 1) != 0 && (read->getOriginalRNEXTLength() != contig->nameLength || memcmp(contig->name, read->getOriginalRNEXT(), contig->nameLength))) {
                        context->nCrossContigPairs++;
                    } else if ((read->getOriginalSAMFlags() & SAM_ALL_ALIGNED) == SAM_ALL_ALIGNED && (read->getOriginalSAMFlags() & SAM_UNMAPPED) == 0 && (read->getOriginalSAMFlags() & SAM_NEXT_UNMAPPED) == 0 &&
                        ((read->getOriginalSAMFlags() & SAM_REVERSE_COMPLEMENT) == 0) != ((read->getOriginalSAMFlags() & SAM_NEXT_REVERSED) == 0)) {
                        _int64 distance = read->getOriginalPNEXT() - (alignedLocation - contig->beginningLocation);
                        if (distance < 0) {
                            distance = 0 - distance;
                        }

                        context->totalPairedReadDistance += distance;
                        context->maxPairedReadDistance = __max(context->maxPairedReadDistance, distance);
                        context->minPairedReadDistance = __min(context->minPairedReadDistance, distance);
                        if (distance < THREAD_CONTEXT_PAIRED_READ_MAX_FOR_MEDIAN) {
                            context->nAtPairedDistance[distance]++;
                        } else {
                            context->nBeyondTrackedPairedDistance++;
                        }
                    }
                } // if we have a contig
            }
        }

        if (context->totalReads >= nReadsPerThread) {
            break;
        }
    } // for each read from the reader


    if (0 == InterlockedAdd64AndReturnNewValue(&nRunningThreads, -1)) {
        SignalSingleWaiterObject(&allThreadsDone);
    }

}


int main(int argc, char * argv[])
{
    BigAllocUseHugePages = false;
    CreateSingleWaiterObject(&allThreadsDone);

    int nReadsToSample = 10000000;

    if (5 != argc && 6 != argc) usage();

    static const char *genomeSuffix = "Genome";
    size_t filenameLen = strlen(argv[1]) + 1 + strlen(genomeSuffix) + 1;
    char *fileName = new char[strlen(argv[1]) + 1 + strlen(genomeSuffix) + 1];
    snprintf(fileName, filenameLen, "%s%c%s", argv[1], PATH_SEP, genomeSuffix);
    genome = Genome::loadFromFile(fileName, 0);
    if (NULL == genome) {
        fprintf(stderr, "Unable to load genome from file '%s'\n", fileName);
        return -1;
    }
    delete[] fileName;
    fileName = NULL;

    FILE *inputFile;
    if (!strcmp("-", argv[3])) {
        inputFile = stdin;
    } else {
        inputFile = fopen(argv[3], "r");
        if (NULL == inputFile) {
            fprintf(stderr, "Unable to open input file '%s'\n", argv[3]);
            soft_exit(1);
        }
    }

    FILE *outputFile;
    if (!strcmp("-", argv[4])) {
        outputFile = stdout;
    } else {
        outputFile = fopen(argv[4], "a");
        if (NULL == outputFile) {
            fprintf(stderr, "Unable to open output file '%s'\n", argv[4]);
            soft_exit(1);
        }
    }

    unsigned nThreads;

#ifdef _DEBUG
    nThreads = 1;
#else   // _DEBUG
    nThreads = GetNumberOfProcessors();
#endif // _DEBUG

    nReadsPerThread = (nReadsToSample + nThreads - 1) / nThreads;

    DataSupplier::ExpansionFactor = 2.0;

    ThreadContext *threadContexts = new ThreadContext[nThreads];


    DataSupplier::ThreadCount = nThreads;
    nRunningThreads = nThreads;

    ReaderContext readerContext;
    readerContext.clipping = NoClipping;
    readerContext.defaultReadGroup = "";
    readerContext.genome = genome;
    readerContext.ignoreSecondaryAlignments = true;
    readerContext.ignoreSupplementaryAlignments = true;
    readerContext.header = NULL;
    readerContext.headerLength = 0;
    readerContext.headerBytes = 0;

    if (NULL != strrchr(argv[3], '.') && !_stricmp(strrchr(argv[3], '.'), ".bam")) {
        readSupplierGenerator = BAMReader::createReadSupplierGenerator(argv[3], nThreads, readerContext);
    } else {
        readSupplierGenerator = SAMReader::createReadSupplierGenerator(argv[3], nThreads, readerContext);
    }

    ResetSingleWaiterObject(&allThreadsDone);
    for (unsigned i = 0; i < nThreads; i++) {
        StartNewThread(WorkerThreadMain, &threadContexts[i]);
    }
    WaitForSingleWaiterObject(&allThreadsDone);

    _int64 totalReads = 0;
    _int64 minReadLength = 1000000, maxReadLength = 0, totalReadLength = 0;
    _int64 minGoodBases = 1000000, maxGoodBases = 0, totalGoodBases = 0;
    _int64 totalPairedReads = 0, totalPairedReadDistance = 0, nCrossContigPairs = 0;
    _int64 minPairedReadDistance = 100000000, maxPairedReadDistance = 0, nBeyondTrackedPairedDistance = 0;

    _int64 nAtPairedDistance[THREAD_CONTEXT_PAIRED_READ_MAX_FOR_MEDIAN];
    for (int i = 0; i < THREAD_CONTEXT_PAIRED_READ_MAX_FOR_MEDIAN; i++) {
        nAtPairedDistance[i] = 0;
    }
    _int64 totalForMedian = 0;

    bool anyPaired = false;
    bool allPaired = true;

    for (unsigned i = 0; i < nThreads; i++) {
        totalReads += threadContexts[i].totalReads;
        minReadLength = __min(threadContexts[i].minReadLength, minReadLength);
        maxReadLength = __max(threadContexts[i].maxReadLength, maxReadLength);
        totalReadLength += threadContexts[i].totalReadLength;

        minGoodBases = __min(threadContexts[i].minGoodBases, minGoodBases);
        maxGoodBases = __max(threadContexts[i].maxGoodBases, maxGoodBases);
        totalGoodBases += threadContexts[i].totalGoodBases;

        totalPairedReads += threadContexts[i].totalPairedReads;
        totalPairedReadDistance += threadContexts[i].totalPairedReadDistance;
        nCrossContigPairs += threadContexts[i].nCrossContigPairs;
        minPairedReadDistance = __min(minPairedReadDistance, threadContexts[i].minPairedReadDistance);
        maxPairedReadDistance = __max(maxPairedReadDistance, threadContexts[i].maxPairedReadDistance);

        for (int j = 0; j < THREAD_CONTEXT_PAIRED_READ_MAX_FOR_MEDIAN; j++) {
            nAtPairedDistance[j] += threadContexts[i].nAtPairedDistance[j];
            totalForMedian += threadContexts[i].nAtPairedDistance[j];
        }
        nBeyondTrackedPairedDistance += threadContexts[i].nBeyondTrackedPairedDistance;

        anyPaired |= threadContexts[i].anyPaired;
        allPaired &= threadContexts[i].allPaired;
    }

    int median = -1;

    if (nBeyondTrackedPairedDistance > totalForMedian || totalForMedian == 0) {
        median = -1;
    } else {
        _int64 countRemaining = (nBeyondTrackedPairedDistance + totalForMedian) / 2;

        for (int i = 0; i < THREAD_CONTEXT_PAIRED_READ_MAX_FOR_MEDIAN; i++) {
            countRemaining -= nAtPairedDistance[i];
            if (countRemaining <= 0) {
                median = i;
                break;
            }
        }
    }


    fprintf(outputFile, "%s\t%s\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%d\t%d\t%lld\t%lld\t%d\t%lld\t%lf\n",
        argv[2], argv[3], totalReads, minReadLength, maxReadLength, totalReadLength, minGoodBases, maxGoodBases, totalGoodBases, anyPaired, allPaired, 
        minPairedReadDistance, maxPairedReadDistance, median, nCrossContigPairs, (double)totalPairedReadDistance / (double) (totalPairedReads - nCrossContigPairs));

    fclose(outputFile);
    fclose(inputFile);

    return 0;
}


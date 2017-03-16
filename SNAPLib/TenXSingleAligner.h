/*++

Module Name:

    TenXSingleAligner.h

Abstract:

    A paired-end aligner based on set intersections to narrow down possible candidate locations.

Authors:

    Hongyi Xin and Bill Bolosky, June, 2016

Environment:

    User mode service.

Revision History:

--*/
#ifndef __TENX_SINGLE_ALIGNER__
#define __TENX_SINGLE_ALIGNER__

#define UNLINKED_ID -1
#define ANCHOR_ID -2
#define MAGNET_ID -3

#pragma once

#include "PairedEndAligner.h"
#include "BaseAligner.h"
#include "BigAlloc.h"
#include "directions.h"
#include "LandauVishkin.h"
#include "FixedSizeMap.h"
#include "AlignmentAdjuster.h"


#ifndef __INTERSECTING__CONSTANTS__
#define __INTERSECTING__CONSTANTS__
const unsigned DEFAULT_INTERSECTING_ALIGNER_MAX_HITS = 2000;
const unsigned DEFAULT_MAX_CANDIDATE_POOL_SIZE = 1000000;
#endif //#define __INTERSECTING__CONSTANTS__

class TenXSingleAligner : public PairedEndAligner
{
private:
    class HashTableHitSet; // Just a declaration for later use.


public:
    TenXSingleAligner(
        GenomeIndex     *index_,
        unsigned        maxReadSize_,
        unsigned        maxHits_,
        unsigned        maxK_,
        unsigned        maxSeedsFromCommandLine_,
        double          seedCoverage_,
        unsigned        minSpacing_,                 // Minimum distance to allow between the two ends.
        unsigned        maxSpacing_,                 // Maximum distance to allow between the two ends.
        unsigned        maxBigHits_,
        unsigned        extraSearchDepth_,
        unsigned        maxCandidatePoolSize,
        int             maxSecondaryAlignmentsPerContig_,
        BigAllocator    *allocator,
        bool            noUkkonen_,
        bool            noOrderedEvaluation_,
        bool            noTruncation_,
        bool            ignoreAlignmentAdjustmentsForOm_,
        unsigned        printStatsMapQLimit_,
        unsigned        clusterEDCompensation_,
        double          unclusteredPenalty_,
        _uint8          *clusterCounter_,
        bool            *clusterToggle_);

    void    setLandauVishkin(
        LandauVishkin<1> *landauVishkin_,
        LandauVishkin<-1> *reverseLandauVishkin_)
    {
        landauVishkin = landauVishkin_;
        reverseLandauVishkin = reverseLandauVishkin_;
    }

    virtual ~TenXSingleAligner();

    // returns false if there isn't enough memory to hold secondary alignments. Fails alignment step 3
    bool align(
        Read                  *read0,
        Read                  *read1,
        PairedAlignmentResult *result,
        int                    maxEditDistanceForSecondaryResults,
        _int64                 secondaryResultBufferSize,
        _int64                *nSecondaryResults,
        PairedAlignmentResult *secondaryResults,             // The caller passes in a buffer of secondaryResultBufferSize and it's filled in by align()
        _int64                 maxSecondaryResultsToReturn
    );

    // again, this is just a place holder for virtual class, wraps the above align
    virtual bool align(
        Read                  *read0,
        Read                  *read1,
        PairedAlignmentResult *result,
        int                    maxEditDistanceForSecondaryResults,
        _int64                 secondaryResultBufferSize,
        _int64                *nSecondaryResults,
        PairedAlignmentResult *secondaryResults,             // The caller passes in a buffer of secondaryResultBufferSize and it's filled in by align()
        _int64                 singleSecondaryBufferSize,
        _int64                 maxSecondaryResultsToReturn,
        _int64                *nSingleEndSecondaryResultsForFirstRead,
        _int64                *nSingleEndSecondaryResultsForSecondRead,
        SingleAlignmentResult *singleEndSecondaryResults     // Single-end secondary alignments for when the paired-end alignment didn't work properly
    ) {
        return align(read0, read1, result, maxEditDistanceForSecondaryResults, secondaryResultBufferSize, nSecondaryResults, secondaryResults, maxSecondaryResultsToReturn);
    };

    // return true if the pair is done processing after phase 1--no need to go to phase 2, 3 and 4.
    bool align_phase_1(
        Read                  *read0,
        Read                  *read1,
        unsigned              *popularSeedsSkipped
    );
    //
    // align_phase_2_init loads the initial locus pointers. Return true if there is at least one common locus.
    //
    bool align_phase_2_init();
    //
    // align_phase_2_to_target_loc advances all location pairs to right before clusterTargetLoc. For all loc pairs that are before clusterTargetLoc, the potential mapping will be associated with cluster clusterInfoPtr
    // It will terminate after advancing lastGenomeLocationForReadWithFewerHits of both directions beyond clusterTargetLoc.
    //
    bool align_phase_2_to_target_loc(const GenomeLocation &clusterTargetLoc, int clusterIdx);
    //
    // align_phase_2_get_next_loc is accompanied with align_phase_2_to_target_loc. It returns the bigger next locus from the fewer side among the the 2 directions.
    //
    GenomeLocation* align_phase_2_get_locus();
    //
    // align_phase_2_move_locus returns 0 if we have found a good match. Returns 1 if seedLoc of the fewHit side has exhauseted. Returns -1 if fewHit side surpasses moreHIt side.
    //
    bool align_phase_2_move_locus(unsigned whichSetPair);
    //
    // should only call align_phase_2_single_step_add_candidate if align_phase_2_move_locus returns 0
    //
    bool align_phase_2_single_step_add_candidate(unsigned whichSetPair, int clusterIdx);
    //
    // align_phase_2 is a dummy mimicking IntersectingPairedEndAligner
    //
    void align_phase_2();
    //
    // align_phase_2_single_step is the inner loop of align_phase_2
    //
    // bool align_phase_2_single_step(unsigned whichSetPair);

    //
    // align_phase_3 calculates the edit distance of each candidate mapping
    //

    //
    // align_phase_3_coarse_score calculates the mapping when considering all clusters to be valid
    // when inRevise is set to ture, we don't update bestCompensatedScore
    //
    void align_phase_3_score(
        int                     &bestCompensatedScore,
        bool                    inRevise
    );

    //
    // align_phase_3_increment_cluster increments the cluster counter
    //
    void align_phase_3_increment_cluster(
        int                     bestCompensatedScore
    );

    //
    // align_phase_3_refine corrects the best mapping, while adding more secondary mappings
    // returns false if no change in bestCompensatedScore. Return true otherwise
    //
    bool align_phase_3_correct_best_score(
        int                    &bestCompensatedScore,
        _uint8                 minClusterSize
    );
    
    //
    // align_phase_3_correct_count_results counts the number of secondary results and returns ture if reallocation is required
    //
    bool align_phase_3_count_results(
        int                    maxEditDistanceForSecondaryResults,
        int                    &bestCompensatedScore,
        _uint8                 minClusterSize,
        _int64                 *nSecondaryResults,
        _int64                 secondaryResultBufferSize,
        double                 &probabilityOfAllPairs
    );

    //
    // align_phase_3_finalize computes overall probability, but adds no new secondary mappings
    //
    void align_phase_3_generate_results(
        _uint8                 minClusterSize,
        int                    maxEditDistanceForSecondaryResults,
        int                    &bestCompensatedScore,
        _int64                 *nSecondaryResults,
        PairedAlignmentResult  *secondaryResults,             // The caller passes in a buffer of secondaryResultBufferSize and it's filled in by align()
        PairedAlignmentResult  *bestResult
    );

    //
    // align_phase_4 cleans up the result from phase 3. Mainly deduplicate closeby mappings.
    //
    void align_phase_4(
    Read                       *read0,
    Read                       *read1,
    int                        maxEditDistanceForSecondaryResults,
    _int64                     maxSecondaryResultsToReturn,
    unsigned                   *popularSeedsSkipped,
    int                        &bestCompensatedScore,
    double                     probabilityOfAllPairs,
    _int64                     *nSecondaryResults,
    PairedAlignmentResult      *secondaryResults,
    PairedAlignmentResult      *bestResult
    );

    static size_t getBigAllocatorReservation(GenomeIndex * index, unsigned maxBigHitsToConsider, unsigned maxReadSize, unsigned seedLen, unsigned maxSeedsFromCommandLine,
        double seedCoverage, unsigned maxEditDistanceToConsider, unsigned maxExtraSearchDepth, unsigned maxCandidatePoolSize,
        int maxSecondaryAlignmentsPerContig);

    void *operator new(size_t size, BigAllocator *allocator) { _ASSERT(size == sizeof(TenXSingleAligner)); return allocator->allocate(size); }
    void operator delete(void *ptr, BigAllocator *allocator) {/* do nothing.  Memory gets cleaned up when the allocator is deleted.*/ }

    void *operator new(size_t size) { return BigAlloc(size); }
    void operator delete(void *ptr) { BigDealloc(ptr); }

    virtual _int64 getLocationsScored() const {
        return nLocationsScored;
    }


private:

    TenXSingleAligner() : alignmentAdjuster(NULL) {}  // This is for the counting allocator, it doesn't build a useful object

    void allocateDynamicMemory(BigAllocator *allocator, unsigned maxReadSize, unsigned maxBigHitsToConsider, unsigned maxSeedsToUse,
        unsigned maxEditDistanceToConsider, unsigned maxExtraSearchDepth, unsigned maxCandidatePoolSize,
        int maxSecondaryAlignmentsPerContig);

    unsigned        printStatsMapQLimit;
    GenomeIndex     *index;
    const Genome    *genome;
    GenomeDistance  genomeSize;
    unsigned        maxReadSize;
    unsigned        maxHits;
    unsigned        maxBigHits;
    unsigned        extraSearchDepth;
    unsigned        maxK;
    unsigned        numSeedsFromCommandLine;
    double          seedCoverage;
    static const unsigned    MAX_MAX_SEEDS = 30;
    unsigned        minSpacing;
    unsigned        maxSpacing;
    unsigned        seedLen;
    bool            doesGenomeIndexHave64BitLocations;
    _int64          nLocationsScored;
    bool            noUkkonen;
    bool            noOrderedEvaluation;
    bool            noTruncation;
    bool            ignoreAlignmentAdjustmentsForOm;

    AlignmentAdjuster           alignmentAdjuster;

    static const unsigned       maxMergeDistance;

    static const int            NUM_SET_PAIRS = 2; // A "set pair" is read0 FORWARD + read1 RC, or read0 RC + read1 FORWARD.  Again, it doesn't make sense to change this.

    //
    // It's a template, because we 
    // have different sizes of genome locations depending on the hash table format.  So, GL must be unsigned or GenomeLocation
    //    
    template<class GL> struct HashTableLookup {
        unsigned        seedOffset;
        _int64            nHits;
        const GL        *hits;
        unsigned        whichDisjointHitSet;

        //
        // We keep the hash table lookups that haven't been exhaused in a circular list.
        //
        HashTableLookup<GL>    *nextLookupWithRemainingMembers;
        HashTableLookup<GL>    *prevLookupWithRemainingMembers;

        //
        // State for handling the binary search of a location in this lookup.
        // This would ordinarily be stack local state in the binary search
        // routine, but because a) we want to interleave the steps of the binary
        // search in order to allow cache prefetches to have time to execute;
        // and b) we don't want to do dynamic memory allocation (really at all),
        // it gets stuck here.
        //
        int            limit[2]; // The upper and lower limits of the current binary search in hits
        GL             maxGenomeLocationToFindThisSeed;

        //
        // A linked list of lookups that haven't yet completed this binary search.  This is a linked
        // list with no header element, so testing for emptiness needs to happen at removal time.
        // It's done that way to avoid a comparison for list head that would result in a hard-to-predict
        // branch.
        //
        HashTableLookup<GL>    *nextLookupForCurrentBinarySearch;
        HashTableLookup<GL>    *prevLookupForCurrentBinarySearch;

        _int64         currentHitForIntersection;

        //
        // A place for the hash table to write in singletons.  We need this because when the hash table is
        // built with > 4 byte genome locations, it usually doesn't store 8 bytes, so we need to
        // provide the lookup function a place to write the result.  Since we need one per
        // lookup, it goes here.
        //
        GL             singletonGenomeLocation[2];  // The [2] is because we need to look one before sometimes, and that allows space
    };

    //
    // A set of seed hits, represented by the lookups that came out of the big hash table.  It can be over 32 or
    // 64 bit indices, but its external interface is always 64 bits (it extends on the way out if necessary).
    //
    class HashTableHitSet {
    public:
        HashTableHitSet() {}
        void firstInit(unsigned maxSeeds_, unsigned maxMergeDistance_, BigAllocator *allocator, bool doesGenomeIndexHave64BitLocations_);

        //
        // Reset to empty state.
        //
        void init();

        //
        // Record a hash table lookup.  All recording must be done before any
        // calls to getNextHitLessThanOrEqualTo.  A disjoint hit set is a set of hits
        // that don't share any bases in the read.  This is interesting because the edit
        // distance of a read must be at least the number of seeds that didn't hit for
        // any disjoint hit set (because there must be a difference in the read within a
        // seed for it not to hit, and since the reads are disjoint there can't be a case
        // where the same difference caused two seeds to miss).
        //
        void recordLookup(unsigned seedOffset, _int64 nHits, const unsigned *hits, bool beginsDisjointHitSet);
        void recordLookup(unsigned seedOffset, _int64 nHits, const GenomeLocation *hits, bool beginsDisjointHitSet);

        //
        // This efficiently works through the set looking for the next hit at or below this address.
        // A HashTableHitSet only allows a single iteration through its address space per call to
        // init().
        //
        bool getNextHitLessThanOrEqualTo(GenomeLocation maxGenomeLocationToFind, GenomeLocation *actualGenomeLocationFound, unsigned *seedOffsetFound);

        //
        // Walk down just one step, don't binary search.
        //
        bool getNextLowerHit(GenomeLocation *genomeLocation, unsigned *seedOffsetFound);


        //
        // Find the highest genome address.
        //
        bool getFirstHit(GenomeLocation *genomeLocation, unsigned *seedOffsetFound);

        unsigned computeBestPossibleScoreForCurrentHit();

        //
        // This is bit of storage that the 64 bit lookup needs in order to extend singleton hits into 64 bits, since they may be
        // stored in the index in fewer.
        //
        GenomeLocation *getNextSingletonLocation()
        {
            return &lookups64[nLookupsUsed].singletonGenomeLocation[1];
        }


    private:
        struct DisjointHitSet {
            unsigned countOfExhaustedHits;
            unsigned missCount;
        };

        int                              currentDisjointHitSet;
        DisjointHitSet                   *disjointHitSets;
        HashTableLookup<unsigned>        *lookups32;
        HashTableLookup<GenomeLocation>  *lookups64;
        HashTableLookup<unsigned>        lookupListHead32[1];
        HashTableLookup<GenomeLocation>  lookupListHead64[1];
        unsigned                         maxSeeds;
        unsigned                         nLookupsUsed;
        GenomeLocation                   mostRecentLocationReturned;
        unsigned                         maxMergeDistance;
        bool                             doesGenomeIndexHave64BitLocations;
    };

    HashTableHitSet                      *hashTableHitSets[NUM_READS_PER_PAIR][NUM_DIRECTIONS];

    int                                  countOfHashTableLookups[NUM_READS_PER_PAIR];
    _int64                               totalHashTableHits[NUM_READS_PER_PAIR][NUM_DIRECTIONS];
    _int64                               largestHashTableHit[NUM_READS_PER_PAIR][NUM_DIRECTIONS];
    unsigned                             readWithMoreHits;
    unsigned                             readWithFewerHits;

    //
    // A location that's been scored (or waiting to be scored).  This is needed in order to do merging
    // of close-together hits and to track potential mate pairs.
    //
    struct HitLocation {
        GenomeLocation        genomeLocation;
        int                   genomeLocationOffset;   // This is needed because we might get an offset back from scoring (because it's really scoring a range).
        unsigned              seedOffset;
        bool                  isScored;           // Mate pairs are sometimes not scored when they're inserted, because they
        unsigned              score;
        unsigned              maxK;               // The maxK that this was scored with (we may need to rescore if we need a higher maxK and score is -1)
        double                matchProbability;
        unsigned              bestPossibleScore;

        //
        // We have to be careful in the case where lots of offsets in a row match well against the read (think
        // about repetitive short sequences, i.e., ATTATTATTATT...).  We want to merge the close ones together,
        // but if the repetitive sequence extends longer than maxMerge, we don't want to just slide the window
        // over the whole range and declare it all to be one.  There is really no good definition for the right
        // thing to do here, so instead all we do is that when we declare two candidates to be matched we
        // pick one of them to be the match primary and then coalesce all matches that are within maxMatchDistance
        // of the match primary.  No one can match with any of the locations in the set that's beyond maxMatchDistance
        // from the set primary.  This means that in the case of repetitve sequences that we'll declare locations
        // right next to one another not to be matches.  There's really no way around this while avoiding
        // matching things that are possibly much more than maxMatchDistance apart.
        //
        GenomeLocation  genomeLocationOfNearestMatchedCandidate;
    };


    char *rcReadData[NUM_READS_PER_PAIR];                   // the reverse complement of the data for each read
    char *rcReadQuality[NUM_READS_PER_PAIR];                // the reversed quality strings for each read
    unsigned readLen[NUM_READS_PER_PAIR];

    Read *reads[NUM_READS_PER_PAIR][NUM_DIRECTIONS];        // These are the reads that are provided in the align call, together with their reverse complements, which are computed.
    Read rcReads[NUM_READS_PER_PAIR];
    //Read rcReads[NUM_READS_PER_PAIR][NUM_DIRECTIONS];

    char *reversedRead[NUM_READS_PER_PAIR][NUM_DIRECTIONS]; // The reversed data for each read for forward and RC.  This is used in the backwards LV

    LandauVishkin<1>  *landauVishkin;
    LandauVishkin<-1> *reverseLandauVishkin;

    char rcTranslationTable[256];
    unsigned nTable[256];

    BYTE *seedUsed;

    inline bool IsSeedUsed(_int64 indexInRead) const {
        return (seedUsed[indexInRead / 8] & (1 << (indexInRead % 8))) != 0;
    }

    inline void SetSeedUsed(_int64 indexInRead) {
        seedUsed[indexInRead / 8] |= (1 << (indexInRead % 8));
    }

    //
    // "Local probability" means the probability that each end is correct given that the pair itself is correct.
    // Consider the example where there's exactly one decent match for one read, but the other one has several
    // that are all within the correct range for the first one.  Then the local probability for the second read
    // is lower than the first.  The overall probability of an alignment then is 
    // pairProbability * localProbability/ allPairProbability.
    //
    double localBestPairProbability[NUM_READS_PER_PAIR];

    void scoreLocation(
        unsigned             whichRead,
        Direction            direction,
        GenomeLocation       genomeLocation,
        unsigned             seedOffset,
        unsigned             scoreLimit,
        unsigned             *score,
        double               *matchProbability,
        int                  *genomeLocationOffset   // The computed offset for genomeLocation (which is needed because we scan several different possible starting locations)
    );

    //
    // We keep track of pairs of locations to score using two structs, one for each end.  The ends for the read with fewer hits points into
    // a list of structs for the end with more hits, so that we don't need one stuct for each pair, just one for each end, and also so that 
    // we don't need to score the mates more than once if they could be paired with more than one location from the end with fewer hits.
    //
    
    // Declare
    struct MergeAnchor;

    struct ScoringMateCandidate {
        //
        // These are kept in arrays in decreasing genome order, one for each set pair, so you can find the next largest location by just looking one
        // index lower, and vice versa.
        //
        double                  matchProbability;
        GenomeLocation          readWithMoreHitsGenomeLocation;
        unsigned                bestPossibleScore;
        unsigned                score;
        unsigned                scoreLimit;             // The scoreLimit with which score was computed
        unsigned                seedOffset;
        int                     genomeOffset;

        void init(GenomeLocation readWithMoreHitsGenomeLocation_, unsigned bestPossibleScore_, unsigned seedOffset_) {
            readWithMoreHitsGenomeLocation = readWithMoreHitsGenomeLocation_;
            bestPossibleScore = bestPossibleScore_;
            seedOffset = seedOffset_;
            score = -2;
            scoreLimit = -1;
            matchProbability = 0;
            genomeOffset = 0;
        }
    };

    struct ScoringCandidate {
        ScoringCandidate *      scoreListNext;              // This is a singly-linked list
        MergeAnchor *           mergeAnchor;
        unsigned                scoringMateCandidateIndex;  // Index into the array of scoring mate candidates where we should look 
        GenomeLocation          readWithFewerHitsGenomeLocation;
        int                     fewerEndGenomeLocationOffset;
        unsigned                whichSetPair;
        unsigned                seedOffset;

        unsigned                bestPossibleScore;
        unsigned                fewerEndScore;

        int                     clusterIdx;

        void init(GenomeLocation readWithFewerHitsGenomeLocation_, unsigned whichSetPair_, unsigned scoringMateCandidateIndex_, unsigned seedOffset_,
            unsigned bestPossibleScore_, ScoringCandidate *scoreListNext_, int clusterIdx_)
        {
            readWithFewerHitsGenomeLocation = readWithFewerHitsGenomeLocation_;
            whichSetPair = whichSetPair_;
            _ASSERT(whichSetPair < NUM_SET_PAIRS);  // You wouldn't think this would be necessary, but...
            scoringMateCandidateIndex = scoringMateCandidateIndex_;
            seedOffset = seedOffset_;
            bestPossibleScore = bestPossibleScore_;
            scoreListNext = scoreListNext_;
            mergeAnchor = NULL;
            clusterIdx = clusterIdx_;
        }
    };

    //
    // These are used to keep track of places where we should merge together candidate locations for MAPQ purposes, because they're sufficiently
    // close in the genome.
    //
    struct MergeAnchor {
        double                  matchProbability;
        GenomeLocation          locationForReadWithMoreHits;
        GenomeLocation          locationForReadWithFewerHits;
        PairedAlignmentResult   *resultPtr;
        int                     pairScore;
        int                     clusterIdx;
        ScoringCandidate        *candidate;
        ScoringMateCandidate    *mate;

        void init(GenomeLocation locationForReadWithMoreHits_, GenomeLocation locationForReadWithFewerHits_, double matchProbability_, int pairScore_, int clusterIdx_, ScoringCandidate *candidate_, ScoringMateCandidate    *mate_) {
            locationForReadWithMoreHits = locationForReadWithMoreHits_;
            locationForReadWithFewerHits = locationForReadWithFewerHits_;
            matchProbability = matchProbability_;
            pairScore = pairScore_;
            clusterIdx = clusterIdx_;
            candidate = candidate_;
            mate = mate_;
            resultPtr = NULL;
        }

        //
        // Returns whether this candidate is a match for this merge anchor.
        //
        bool doesRangeMatch(GenomeLocation newMoreHitLocation, GenomeLocation newFewerHitLocation) {
            GenomeDistance deltaMore = DistanceBetweenGenomeLocations(locationForReadWithMoreHits, newMoreHitLocation);
            GenomeDistance deltaFewer = DistanceBetweenGenomeLocations(locationForReadWithFewerHits, newFewerHitLocation);

            return deltaMore < 50 && deltaFewer < 50;
        }


        //
        // Returns true and sets oldMatchProbability if this should be eliminated due to a match.
        //
        bool checkMerge(GenomeLocation newMoreHitLocation, GenomeLocation newFewerHitLocation, double newMatchProbability, int newPairScore, int newClusterIdx, ScoringCandidate *newCandidate, ScoringMateCandidate *newMate);

        static int
        compareByClusterIdx(const void *first_, const void *second_)
        {
            const MergeAnchor *first = (MergeAnchor *)first_;
            const MergeAnchor *second = (MergeAnchor *)second_;

            int firstClusterIdx = first->clusterIdx;
            int secondClusterIdx = second->clusterIdx;
            
            if (firstClusterIdx < secondClusterIdx) {
                return 1;
            } else if (firstClusterIdx > secondClusterIdx) {
                return -1;
            } else {
                return 0;
            }
        }
    };

    //
    // A pool of scoring candidates.  For each alignment call, we free them all by resetting lowestFreeScoringCandidatePoolEntry to 0,
    // and then fill in the content when they're initialized.  This means that for alignments with few candidates we'll be using the same
    // entries over and over, so they're likely to be in the cache.  We have maxK * maxSeeds * 2 of these in the pool, so we can't possibly run
    // out.  We rely on their being allocated in descending genome order within a set pair.
    //
    ScoringCandidate *scoringCandidatePool;
    unsigned scoringCandidatePoolSize;
    unsigned lowestFreeScoringCandidatePoolEntry;

    //
    // maxK + 1 lists of Scoring Candidates.  The lists correspond to bestPossibleScore for the candidate and its best mate.
    //

    ScoringCandidate    **scoringCandidates;
    double              *probabilityForED;

    //
    // The scoring mates.  The each set scoringCandidatePoolSize / 2.
    //
    ScoringMateCandidate *scoringMateCandidates[NUM_SET_PAIRS];
    unsigned lowestFreeScoringMateCandidate[NUM_SET_PAIRS];

    //
    // Merge anchors.  Again, we allocate an upper bound number of them, which is the same as the number of scoring candidates.
    //
    MergeAnchor *mergeAnchorPool;
    unsigned firstFreeMergeAnchor;
    unsigned mergeAnchorPoolSize;


    struct HitsPerContigCounts {
        _int64  epoch;              // Rather than zeroing this whole array every time, we just bump the epoch number; results with an old epoch are considered zero
        int     hits;
    };

    HitsPerContigCounts *hitsPerContigCounts;   // How many alignments are we reporting for each contig.  Used to implement -mpc, otheriwse unallocated.
    int maxSecondaryAlignmentsPerContig;
    _int64 contigCountEpoch;

    // 10x additional member variables 
    // For carrying over the query data
    HashTableHitSet*        setPair[NUM_DIRECTIONS][NUM_READS_PER_PAIR];
    bool                    outOfMoreHitsLocations[NUM_DIRECTIONS];
    unsigned                lastSeedOffsetForReadWithFewerHits[NUM_DIRECTIONS];
    GenomeLocation          lastGenomeLocationForReadWithFewerHits[NUM_DIRECTIONS];
    unsigned                lastSeedOffsetForReadWithMoreHits[NUM_DIRECTIONS];
    GenomeLocation          lastGenomeLocationForReadWithMoreHits[NUM_DIRECTIONS];
    unsigned                maxUsedBestPossibleScoreList;
    bool                    noMoreLocus[NUM_DIRECTIONS];

    // Cluster toggles. The caller makes sure that there are enough space in the array
    _uint8                  *clusterCounterAry;
    bool                    *clusterToggle;
    
    // Unclustered compensation
    unsigned                clusterEDCompensation;
    double                  unclusteredPenalty;
};

#endif //#define __TENX_SINGLE_ALIGNER__

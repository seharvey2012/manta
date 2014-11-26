// -*- mode: c++; indent-tabs-mode: nil; -*-
//
// Manta
// Copyright (c) 2013-2014 Illumina, Inc.
//
// This software is provided under the terms and conditions of the
// Illumina Open Source Software License 1.
//
// You should have received a copy of the Illumina Open Source
// Software License 1 along with this program. If not, see
// <https://github.com/sequencing/licenses/>
//

//
// \author Chris Saunders and Felix Schlesinger
//

//#define ALN_DEBUG

#include "AlignerUtil.hh"

#include <cassert>

#ifdef ALN_DEBUG
#include "blt_util/log.hh"
#include <iostream>
#endif



template <typename SymIter>
bool
isUpstreamSpliceAcceptor(
    const SymIter refBegin,
    SymIter refIter,
    bool fwStrand)
{
    if (std::distance(refBegin,refIter)<2) return false;
    if (!fwStrand) // Default (Truseq) RNA data is on the opposite strand from the original RNA
        return (*(refIter-2)=='A' && *(refIter-1)=='G');
    else
        return (*(refIter-2)=='A' && *(refIter-1)=='C');
}



template <typename SymIter>
bool
isDownstreamSpliceDonor(
    SymIter refIter,
    const SymIter refEnd,
    bool fwStrand)
{
    if (std::distance(refIter,refEnd)<2) return false;
    if (!fwStrand) // Default (Truseq) RNA data is on the opposite strand from the original RNA
        return (*(refIter)=='G' && *(refIter+1)=='T');
    else
        return (*(refIter)=='C' && *(refIter+1)=='T');

}


template <typename ScoreType>
template <typename SymIter>
void
GlobalJumpIntronAligner<ScoreType>::
align(
    const SymIter queryBegin, const SymIter queryEnd,
    const SymIter ref1Begin, const SymIter ref1End,
    const SymIter ref2Begin, const SymIter ref2End,
    bool ref1Fw, bool ref2Fw,
    JumpAlignmentResult<ScoreType>& result) const
{
    result.clear();

    const AlignmentScores<ScoreType>& scores(this->getScores());

    const size_t querySize(std::distance(queryBegin, queryEnd));
    const size_t ref1Size(std::distance(ref1Begin, ref1End));
    const size_t ref2Size(std::distance(ref2Begin, ref2End));

    assert(0 != querySize);
    assert(0 != ref1Size);
    assert(0 != ref2Size);

    _score1.resize(querySize+1);
    _score2.resize(querySize+1);
    _ptrMat1.resize(querySize+1, ref1Size+1);
    _ptrMat2.resize(querySize+1, ref2Size+1);

    ScoreVec* thisSV(&_score1);
    ScoreVec* prevSV(&_score2);

    static const ScoreType badVal(-10000);

    // global alignment of query -- disallow start from insertion or deletion
    // state, query can 'fall-off' the end of a short reference, in which case it will
    // be soft-clipped and each base off the end will count as an 'offEdge' state:
    for (unsigned queryIndex(0); queryIndex<=querySize; queryIndex++)
    {
        ScoreVal& val((*thisSV)[queryIndex]);
        val.match = queryIndex * scores.offEdge;
        val.del = badVal;
        val.ins = badVal;
        val.jump = badVal;
        val.intron = badVal;
    }

    BackTrace<ScoreType> btrace;

    {
        unsigned ref1Index(0);
        for (SymIter ref1Iter(ref1Begin); ref1Iter != ref1End; ++ref1Iter, ++ref1Index)
        {
            std::swap(thisSV,prevSV);

            {
                // disallow start from the insert or delete state:
                ScoreVal& val((*thisSV)[0]);
                val.match = 0;
                val.del = badVal;
                val.ins = badVal;
                val.jump = badVal;
                val.intron = badVal;
            }

            unsigned queryIndex(0);
            for (SymIter queryIter(queryBegin); queryIter != queryEnd; ++queryIter, ++queryIndex)
            {
                // update match
                ScoreVal& headScore((*thisSV)[queryIndex+1]);
                PtrVal& headPtr(_ptrMat1.val(queryIndex+1,ref1Index+1));
                {
                    const ScoreVal& sval((*prevSV)[queryIndex]);
                    headPtr.match = this->max3(
                                        headScore.match,
                                        sval.match,
                                        sval.del,
                                        sval.ins);
                    // Only can leave the intron (splice) state if the last two
                    // bases of the intron match the motif
                    if (isUpstreamSpliceAcceptor(ref1Begin,ref1Iter, ref1Fw))
                    {
                        if (headScore.match < sval.intron)
                        {
                            headScore.match = sval.intron;
                            headPtr.match = AlignState::SPLICE;
                        }
                    }
                    headScore.match += ((*queryIter==*ref1Iter) ? scores.match : scores.mismatch);
                }

                // update delete
                {
                    const ScoreVal& sval((*prevSV)[queryIndex+1]);
                    headPtr.del = this->max3(
                                      headScore.del,
                                      sval.match + scores.open,
                                      sval.del,
                                      sval.ins + scores.open);

                    headScore.del += scores.extend;
                    if (0==queryIndex) headScore.del += badVal;
                }

                // update insert
                {
                    const ScoreVal& sval((*thisSV)[queryIndex]);
                    headPtr.ins = this->max3(
                                      headScore.ins,
                                      sval.match + scores.open,
                                      sval.del + scores.open,
                                      sval.ins);

                    headScore.ins += scores.extend;
                    if (0==queryIndex) headScore.ins += badVal;
                }
                // update intron / splice
                {
                    const ScoreVal& sval((*prevSV)[queryIndex+1]);
                    headPtr.intron = AlignState::SPLICE;
                    headScore.intron = sval.intron;
                    // Only can enter the intron (splice) state if the first two
                    // bases of the intron match the motif
                    if (isDownstreamSpliceDonor(ref1Iter,ref1End, ref1Fw))
                    {
                        if (sval.match + _intronOpenScore > sval.intron)
                        {
                            headScore.intron = sval.match + _intronOpenScore;
                            headPtr.intron = AlignState::MATCH;
                        }
                    }
                }
                // update jump
                {
                    const ScoreVal& sval((*prevSV)[queryIndex+1]);
                    headPtr.jump = this->max4(
                                       headScore.jump,
                                       headScore.match + this->getJumpScore(),
                                       badVal,
                                       headScore.ins + this->getJumpScore(),
                                       sval.jump);
                }

#ifdef ALN_DEBUG
                log_os << "queryIdx refIdx ref1Idx: " << queryIndex+1 << " " << ref1Index+1 << " " << ref1Index+1 << "\n";
                log_os << headScore.match << ":" << headScore.del << ":" << headScore.ins << ":" << headScore.jump << ":" << headScore.intron << "/"
                       << static_cast<int>(headPtr.match) << static_cast<int>(headPtr.del) << static_cast<int>(headPtr.ins) << static_cast<int>(headPtr.jump) << "\n";
                log_os << "Q:" << *queryIter << " R:" << *ref1Iter << "\n";
#endif
            }
#ifdef ALN_DEBUG
            log_os << "\n";
#endif

            // get backtrace info:
            {
                const ScoreVal& sval((*thisSV)[querySize]);
                updateBacktrace(sval.match, ref1Index+1, querySize, btrace);
            }
        }
    }

    // in the backtrace start search, also allow for the case where the query falls-off the end of the reference:
    for (unsigned queryIndex(0); queryIndex<=querySize; queryIndex++)
    {
        const ScoreVal& sval((*thisSV)[queryIndex]);
        const ScoreType thisMax(sval.match + (querySize-queryIndex) * scores.offEdge);
        updateBacktrace(thisMax, ref1Size, queryIndex, btrace);
    }


    {
        for (unsigned queryIndex(0); queryIndex<=querySize; queryIndex++)
        {
            ScoreVal& val((*thisSV)[queryIndex]);
            val.match = queryIndex * scores.offEdge;
            val.del = badVal;
            val.ins = badVal;
            val.intron = badVal;
            //val.jump = badVal; // preserve jump setting from last iteration of ref1
        }

        unsigned ref2Index(0);
        for (SymIter ref2Iter(ref2Begin); ref2Iter != ref2End; ++ref2Iter, ++ref2Index)
        {
            std::swap(thisSV,prevSV);

            {
                // disallow start from the insert or delete state:
                ScoreVal& val((*thisSV)[0]);
                val.match = 0;
                val.del = badVal;
                val.ins = badVal;
                val.jump = badVal;
                val.intron = badVal;
            }

            unsigned queryIndex(0);
            for (SymIter queryIter(queryBegin); queryIter != queryEnd; ++queryIter, ++queryIndex)
            {
                // update match
                ScoreVal& headScore((*thisSV)[queryIndex+1]);
                PtrVal& headPtr(_ptrMat2.val(queryIndex+1,ref2Index+1));
                {
                    const ScoreVal& sval((*prevSV)[queryIndex]);
                    headPtr.match = this->max4(
                                        headScore.match,
                                        sval.match,
                                        sval.del,
                                        sval.ins,
                                        sval.jump);
                    // Only can leave the intron (splice) state if the last two
                    // bases of the intron match the motif
                    if (isUpstreamSpliceAcceptor(ref2Begin,ref2Iter, ref2Fw))
                    {
                        if (headScore.match < sval.intron)
                        {
                            headScore.match = sval.intron;
                            headPtr.match = AlignState::SPLICE;
                        }
                    }

                    headScore.match += ((*queryIter==*ref2Iter) ? scores.match : scores.mismatch);
                }

                // update delete
                {
                    const ScoreVal& sval((*prevSV)[queryIndex+1]);
                    headPtr.del = this->max3(
                                      headScore.del,
                                      sval.match + scores.open,
                                      sval.del,
                                      sval.ins + scores.open);

                    headScore.del += scores.extend;
                }

                // update insert
                {
                    const ScoreVal& sval((*thisSV)[queryIndex]);
                    headPtr.ins = this->max4(
                                      headScore.ins,
                                      sval.match + scores.open,
                                      sval.del + scores.open,
                                      sval.ins,
                                      sval.jump); // jump->ins moves get a pass on the gap-open penalty, to support mirco-insertions

                    headScore.ins += scores.extend;
                }
                // update intron
                {
                    const ScoreVal& sval((*prevSV)[queryIndex+1]);
                    headPtr.intron = AlignState::SPLICE;
                    headScore.intron = sval.intron;
                    // Only can enter the intron (splice) state if the first two
                    // bases of the intron match the motif
                    if (isDownstreamSpliceDonor(ref2Iter,ref2End, ref2Fw))
                    {
                        if (sval.match + _intronOpenScore > sval.intron)
                        {
                            headScore.intron = sval.match + _intronOpenScore;
                            headPtr.intron = AlignState::MATCH;
                        }
                    }
                }
                // update jump
                {
                    const ScoreVal& sval((*prevSV)[queryIndex+1]);
                    headPtr.jump = AlignState::JUMP;
                    headScore.jump = sval.jump;
                }

#ifdef ALN_DEBUG
                log_os << "queryIdx refIdx ref2Idx: " << queryIndex+1 << " " << ref1Size+ref2Index+1 << " " << ref2Index+1 << "\n";
                log_os << headScore.match << ":" << headScore.del << ":" << headScore.ins << ":" << headScore.jump << "/"
                       << static_cast<int>(headPtr.match) << static_cast<int>(headPtr.del) << static_cast<int>(headPtr.ins) << static_cast<int>(headPtr.jump) << "\n";
#endif
            }
#ifdef ALN_DEBUG
            log_os << "\n";
#endif

            // get backtrace start info:
            {
                const ScoreVal& sval((*thisSV)[querySize]);
                updateBacktrace(sval.match, ref1Size+ref2Index+1, querySize, btrace);
            }
        }
    }

    // in the backtrace start search, also allow for the case where the query falls-off the end of the reference:
    for (unsigned queryIndex(0); queryIndex<=querySize; queryIndex++)
    {
        const ScoreVal& sval((*thisSV)[queryIndex]);
        const ScoreType thisMax(sval.match + (querySize-queryIndex) * scores.offEdge);
        updateBacktrace(thisMax, ref1Size+ref2Size, queryIndex, btrace);
    }

#ifdef ALN_DEBUG
    log_os << "bt-start queryIndex: " << btrace.queryBegin << " refIndex: " << btrace.refBegin << " state: " << AlignState::label(btrace.state) << " maxScore: " << btrace.max << "\n";
#endif

    this->backTraceAlignment(
        queryBegin, queryEnd,
        ref1Begin, ref1End,
        ref2Begin, ref2End,
        querySize, ref1Size, ref2Size,
        _ptrMat1, _ptrMat2,
        btrace, result);
}


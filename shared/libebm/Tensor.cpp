// Copyright (c) 2023 The InterpretML Contributors
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#include "pch.hpp"

#include <type_traits> // std::is_standard_layout
#include <stdlib.h> // malloc, realloc, free
#include <stddef.h> // size_t, ptrdiff_t
#include <string.h> // memcpy

#define ZONE_main
#include "zones.h"

#include "ebm_internal.hpp"
#include "Feature.hpp"
#include "Term.hpp"
#include "Tensor.hpp"

namespace DEFINED_ZONE_NAME {
#ifndef DEFINED_ZONE_NAME
#error DEFINED_ZONE_NAME must be defined
#endif // DEFINED_ZONE_NAME

Tensor* Tensor::Allocate(const size_t cDimensionsMax, const size_t cScores) {
   EBM_ASSERT(cDimensionsMax <= k_cDimensionsMax);
   EBM_ASSERT(1 <= cScores); // having 0 classes makes no sense, and having 1 class is useless

   if(IsMultiplyError(k_initialTensorCapacity, cScores)) {
      LOG_0(Trace_Warning, "WARNING Allocate IsMultiplyError(k_initialTensorCapacity, cScores)");
      return nullptr;
   }
   const size_t cTensorScoreCapacity = sizeof(FloatScore) * k_initialTensorCapacity * cScores;

   // this can't overflow since cDimensionsMax can't be bigger than k_cDimensionsMax, which is arround 64
   const size_t cBytesTensor = offsetof(Tensor, m_aDimensions) + sizeof(DimensionInfo) * cDimensionsMax;
   Tensor* const pTensor = reinterpret_cast<Tensor*>(malloc(cBytesTensor));
   if(UNLIKELY(nullptr == pTensor)) {
      LOG_0(Trace_Warning, "WARNING Allocate nullptr == pTensor");
      return nullptr;
   }

   pTensor->m_cScores = cScores;
   pTensor->m_cDimensionsMax = cDimensionsMax;
   pTensor->m_cDimensions = cDimensionsMax;
   pTensor->m_cTensorScoreCapacity = cTensorScoreCapacity;
   pTensor->m_bExpanded = false;

   // this isn't required to be aligned, but do it anyways to keep as much of it on a single cache line as possible
   FloatScore* const aTensorScores = static_cast<FloatScore*>(AlignedAlloc(cTensorScoreCapacity));
   if(UNLIKELY(nullptr == aTensorScores)) {
      LOG_0(Trace_Warning, "WARNING Allocate nullptr == aTensorScores");
      free(pTensor); // don't need to call the full Free(*) yet
      return nullptr;
   }
   pTensor->m_aTensorScores = aTensorScores;

   // we only need to set the base case to zero, not our entire initial allocation
   // we checked for cScores * k_initialTensorCapacity * sizeof(FloatScore), and 1 <= k_initialTensorCapacity,
   // so sizeof(FloatScore) * cScores can't overflow
   //
   // we check elsewhere that IEEE754 is used, so bit zeroing is making zeroed floats
   memset(aTensorScores, 0, sizeof(*aTensorScores) * cScores);

   if(0 != cDimensionsMax) {
      DimensionInfo* pDimension = pTensor->GetDimensions();
      size_t iDimension = 0;
      do {
         pDimension->m_cSlices = 1;
         pDimension->m_cSliceCapacity = k_initialSliceCapacity;
         pDimension->m_aSplits = nullptr;
         ++pDimension;
         ++iDimension;
      } while(iDimension < cDimensionsMax);

      pDimension = pTensor->GetDimensions();
      iDimension = 0;
      do {
         UIntSplit* const aSplits = static_cast<UIntSplit*>(malloc(sizeof(UIntSplit) * (k_initialSliceCapacity - 1)));
         if(UNLIKELY(nullptr == aSplits)) {
            LOG_0(Trace_Warning, "WARNING Allocate nullptr == aSplits");
            Free(pTensor); // free everything!
            return nullptr;
         }
         pDimension->m_aSplits = aSplits;
         ++pDimension;
         ++iDimension;
      } while(iDimension < cDimensionsMax);
   }
   return pTensor;
}

void Tensor::Free(Tensor* const pTensor) {
   if(LIKELY(nullptr != pTensor)) {
      AlignedFree(pTensor->m_aTensorScores);
      if(LIKELY(0 != pTensor->m_cDimensionsMax)) {
         const DimensionInfo* pDimensionInfo = pTensor->GetDimensions();
         const DimensionInfo* const pDimensionInfoEnd = &pDimensionInfo[pTensor->m_cDimensionsMax];
         do {
            free(pDimensionInfo->m_aSplits);
            ++pDimensionInfo;
         } while(pDimensionInfoEnd != pDimensionInfo);
      }
      free(pTensor);
   }
}

void Tensor::Reset() {
   DimensionInfo* pDimensionInfo = GetDimensions();
   for(size_t iDimension = 0; iDimension < m_cDimensions; ++iDimension) {
      pDimensionInfo[iDimension].m_cSlices = 1;
   }
   // we only need to set the base case to zero
   // this can't overflow since we previously allocated this memory
   for(size_t i = 0; i < m_cScores; ++i) {
      m_aTensorScores[i] = 0;
   }
   m_bExpanded = false;
}

ErrorEbm Tensor::SetCountSlices(const size_t iDimension, const size_t cSlices) {
   EBM_ASSERT(iDimension < m_cDimensions);
   DimensionInfo* const pDimension = &GetDimensions()[iDimension];
   // we shouldn't be able to expand our length after we're been expanded since expanded should be the maximum size
   // already
   EBM_ASSERT(!m_bExpanded || cSlices <= pDimension->m_cSlices);
   if(UNLIKELY(pDimension->m_cSliceCapacity < cSlices)) {
      EBM_ASSERT(!m_bExpanded); // we shouldn't be able to expand our length after we're been expanded since expanded
                                // should be the maximum size already

      size_t cSplits = cSlices - 1;
      if(IsAddError(cSplits, cSplits >> 1)) {
         LOG_0(Trace_Warning, "WARNING SetCountSplits IsAddError(cSplits, cSplits >> 1)");
         return Error_OutOfMemory;
      }
      // just increase it by 50% since we don't expect to grow our splits often after an initial period,
      // and realloc takes some of the cost of growing away
      size_t cNewSplitCapacity = cSplits + (cSplits >> 1);
      LOG_N(Trace_Info, "SetCountSplits Growing to size %zu", cNewSplitCapacity);

      if(IsMultiplyError(sizeof(UIntSplit), cNewSplitCapacity)) {
         LOG_0(Trace_Warning, "WARNING SetCountSplits IsMultiplyError(sizeof(UIntSplit), cNewSplitCapacity)");
         return Error_OutOfMemory;
      }
      size_t cBytes = sizeof(UIntSplit) * cNewSplitCapacity;
      UIntSplit* const aNewSplits = static_cast<UIntSplit*>(realloc(pDimension->m_aSplits, cBytes));
      if(UNLIKELY(nullptr == aNewSplits)) {
         // according to the realloc spec, if realloc fails to allocate the new memory, it returns nullptr BUT the old
         // memory is valid. we leave m_aThreadByteBuffer1 alone in this instance and will free that memory later in the
         // destructor
         LOG_0(Trace_Warning, "WARNING SetCountSplits nullptr == aNewSplits");
         return Error_OutOfMemory;
      }
      pDimension->m_aSplits = aNewSplits;
      pDimension->m_cSliceCapacity = cNewSplitCapacity + 1;
   } // never shrink our array unless the user chooses to Trim()
   pDimension->m_cSlices = cSlices;
   return Error_None;
}

ErrorEbm Tensor::EnsureTensorScoreCapacity(const size_t cTensorScores) {
   if(IsMultiplyError(sizeof(FloatScore), cTensorScores)) {
      LOG_0(Trace_Warning, "WARNING EnsureTensorScoreCapacity IsMultiplyError(sizeof(FloatScore), cTensorScores)");
      return Error_OutOfMemory;
   }
   size_t cBytes = sizeof(FloatScore) * cTensorScores;
   return AlignedGrow(reinterpret_cast<void**>(&m_aTensorScores), &m_cTensorScoreCapacity, cBytes, EBM_TRUE);
}

ErrorEbm Tensor::Copy(const Tensor& rhs) {
   EBM_ASSERT(m_cDimensions == rhs.m_cDimensions);

   ErrorEbm error;

   const DimensionInfo* pThisDimensionInfo = GetDimensions();
   const DimensionInfo* pRhsDimensionInfo = rhs.GetDimensions();

   size_t cTensorScores = m_cScores;
   for(size_t iDimension = 0; iDimension < m_cDimensions; ++iDimension) {
      const DimensionInfo* const pDimension = &pRhsDimensionInfo[iDimension];
      size_t cSlices = pDimension->m_cSlices;
      EBM_ASSERT(
            !IsMultiplyError(cTensorScores, cSlices)); // we're copying this memory, so multiplication can't overflow
      cTensorScores *= cSlices;
      error = SetCountSlices(iDimension, cSlices);
      if(UNLIKELY(Error_None != error)) {
         LOG_0(Trace_Warning, "WARNING Copy SetCountSlices(iDimension, cSlices)");
         return error;
      }
      EBM_ASSERT(!IsMultiplyError(
            sizeof(UIntSplit), cSlices - 1)); // we're copying this memory, so multiplication can't overflow
      memcpy(pThisDimensionInfo[iDimension].m_aSplits, pDimension->m_aSplits, sizeof(UIntSplit) * (cSlices - 1));
   }
   error = EnsureTensorScoreCapacity(cTensorScores);
   if(UNLIKELY(Error_None != error)) {
      // already logged
      return error;
   }
   EBM_ASSERT(!IsMultiplyError(
         sizeof(FloatScore), cTensorScores)); // we're copying this memory, so multiplication can't overflow
   memcpy(m_aTensorScores, rhs.m_aTensorScores, sizeof(FloatScore) * cTensorScores);
   m_bExpanded = rhs.m_bExpanded;
   return Error_None;
}

bool Tensor::MultiplyAndCheckForIssues(const double v) {
   const FloatScore vFloat = static_cast<FloatScore>(v);
   const DimensionInfo* pThisDimensionInfo = GetDimensions();

   size_t cTensorScores = m_cScores;
   for(size_t iDimension = 0; iDimension < m_cDimensions; ++iDimension) {
      // we're accessing existing memory, so it can't overflow
      EBM_ASSERT(!IsMultiplyError(cTensorScores, pThisDimensionInfo[iDimension].m_cSlices));
      cTensorScores *= pThisDimensionInfo[iDimension].m_cSlices;
   }

   FloatScore* pCur = &m_aTensorScores[0];
   FloatScore* pEnd = &m_aTensorScores[cTensorScores];
   int bBad = 0;
   // we always have 1 score, even if we have zero splits
   do {
      const FloatScore val = *pCur * vFloat;
      // TODO: these can be done with bitwise operators, which would be good for SIMD.  Check to see what assembly this
      // turns into. since both NaN and +-infinity have the exponential as FF, and no other values do, the best
      // optimized assembly would test the exponential bits for FF and then OR a 1 if the test is true and 0 if the test
      // is false
      // TODO: another issue is that isnan and isinf don't work on some compilers with some compiler settings
      bBad |= std::isnan(val) || std::isinf(val);
      *pCur = val;
      ++pCur;
   } while(pEnd != pCur);
   return !!bBad;
}

ErrorEbm Tensor::Expand(const Term* const pTerm) {
   ErrorEbm error;

   LOG_0(Trace_Verbose, "Entered Expand");

   if(m_bExpanded) {
      // we're already expanded
      LOG_0(Trace_Verbose, "Exited Expand");
      return Error_None;
   }

   EBM_ASSERT(nullptr != pTerm);
   const size_t cDimensions = pTerm->GetCountDimensions();
   if(size_t{0} != cDimensions) {
      const TermFeature* pTermFeature1 = pTerm->GetTermFeatures();
      const TermFeature* const pTermFeaturesEnd = &pTermFeature1[cDimensions];
      DimensionInfoStackExpand aDimensionInfoStackExpand[k_cDimensionsMax];
      DimensionInfoStackExpand* pDimensionInfoStackFirst = aDimensionInfoStackExpand;
      const DimensionInfo* pDimensionFirst1 = GetDimensions();
      size_t cTensorScores1 = m_cScores;
#ifndef NDEBUG
      size_t cNewTensorScoresDebug = m_cScores;
#endif // NDEBUG
      // first, get basic counts of how many splits and scores we'll have in our final result
      do {
         const FeatureBoosting* const pFeature = pTermFeature1->m_pFeature;
         const size_t cBins = pFeature->GetCountBins();

#ifndef NDEBUG
         cNewTensorScoresDebug *= cBins;
#endif // NDEBUG

         const size_t cSlices1 = pDimensionFirst1->m_cSlices;

         EBM_ASSERT(
               !IsMultiplyError(cTensorScores1, cSlices1)); // this is accessing existing memory, so it can't overflow
         cTensorScores1 *= cSlices1;

         pDimensionInfoStackFirst->m_pSplit1 = &pDimensionFirst1->m_aSplits[cSlices1 - 1];

         pDimensionInfoStackFirst->m_iEdge2 = cBins;
         pDimensionInfoStackFirst->m_cNewSlices = cBins;

         ++pDimensionFirst1;
         ++pDimensionInfoStackFirst;
         ++pTermFeature1;
      } while(pTermFeaturesEnd != pTermFeature1);

      EBM_ASSERT(!IsMultiplyError(m_cScores, pTerm->GetCountTensorBins()));
      const size_t cNewTensorScores = m_cScores * pTerm->GetCountTensorBins();
      EBM_ASSERT(cNewTensorScoresDebug == cNewTensorScores);
      EBM_ASSERT(1 <= cNewTensorScores);

      // call EnsureTensorScoreCapacity before using the m_aTensorScores pointer since m_aTensorScores might change
      // inside EnsureTensorScoreCapacity
      error = EnsureTensorScoreCapacity(cNewTensorScores);
      if(UNLIKELY(Error_None != error)) {
         // already logged
         return error;
      }

      FloatScore* const aTensorScores = m_aTensorScores;
      const DimensionInfo* const aDimension1 = GetDimensions();

      EBM_ASSERT(cTensorScores1 <= cNewTensorScores);
      const FloatScore* pTensorScore1 = &aTensorScores[cTensorScores1];
      FloatScore* pTensorScoreTop = &aTensorScores[cNewTensorScores];

      // traverse the scores in reverse so that we can put our results at the higher order indexes where we are
      // guaranteed not to overwrite our existing scores which we still need to copy first do the scores because we need
      // to refer to the old splits when making decisions about where to move next
      while(true) {
         const FloatScore* pTensorScore1Move = pTensorScore1;
         const FloatScore* const pTensorScoreTopEnd = pTensorScoreTop - m_cScores;
         do {
            --pTensorScore1Move;
            --pTensorScoreTop;
            EBM_ASSERT(aTensorScores <= pTensorScore1Move);
            EBM_ASSERT(aTensorScores <= pTensorScoreTop);
            *pTensorScoreTop = *pTensorScore1Move;
         } while(pTensorScoreTopEnd != pTensorScoreTop);

         // For a single dimensional Tensor checking here is best.
         // For two or higher dimensions, we could instead check inside our loop below for when we reach the end of the
         // pDimensionInfoStack, thus eliminating the check on most loops. We'll spend most of our time working on
         // single features though, so we optimize for that case, but if we special cased the single dimensional case,
         // then we would want to move this check into the loop below in the case of multi-dimensioncal Tensors
         if(UNLIKELY(aTensorScores == pTensorScoreTop)) {
            // we've written our final tensor cell, so we're done
            break;
         }

         DimensionInfoStackExpand* pDimensionInfoStackSecond = aDimensionInfoStackExpand;
         const DimensionInfo* pDimensionSecond1 = aDimension1;

         size_t multiplication1 = m_cScores;

         while(true) {
            const UIntSplit* const pSplit1 = pDimensionInfoStackSecond->m_pSplit1;
            size_t iEdge2 = pDimensionInfoStackSecond->m_iEdge2;

            UIntSplit* const aSplits1 = pDimensionSecond1->m_aSplits;

            EBM_ASSERT(static_cast<size_t>(pSplit1 - aSplits1) < iEdge2);
            if(UNPREDICTABLE(aSplits1 < pSplit1)) {
               EBM_ASSERT(1 < iEdge2);

               const UIntSplit* const pSplit1MinusOne = pSplit1 - 1;

               const size_t d1 = static_cast<size_t>(*pSplit1MinusOne);

               --iEdge2;

               const bool bMove = UNPREDICTABLE(iEdge2 <= d1);
               pDimensionInfoStackSecond->m_pSplit1 = bMove ? pSplit1MinusOne : pSplit1;
               pTensorScore1 = bMove ? pTensorScore1 - multiplication1 : pTensorScore1;

               pDimensionInfoStackSecond->m_iEdge2 = iEdge2;
               break;
            } else {
               if(UNPREDICTABLE(1 < iEdge2)) {
                  pDimensionInfoStackSecond->m_iEdge2 = iEdge2 - 1;
                  break;
               } else {
                  pTensorScore1 -= multiplication1; // put us before the beginning.  We'll add the full row first

                  const size_t cSlices1 = pDimensionSecond1->m_cSlices;

                  // we're already allocated scores, so this is accessing what we've already allocated, so it must not
                  // overflow
                  EBM_ASSERT(!IsMultiplyError(multiplication1, cSlices1));
                  multiplication1 *= cSlices1;

                  // go to the last valid entry back to where we started.  If we don't move down a set, then we re-do
                  // this set of numbers
                  pTensorScore1 += multiplication1;

                  pDimensionInfoStackSecond->m_pSplit1 = &aSplits1[cSlices1 - 1];
                  pDimensionInfoStackSecond->m_iEdge2 = pDimensionInfoStackSecond->m_cNewSlices;

                  ++pDimensionSecond1;
                  ++pDimensionInfoStackSecond;
                  continue;
               }
            }
         }
      }

      EBM_ASSERT(pTensorScoreTop == m_aTensorScores);
      EBM_ASSERT(pTensorScore1 == m_aTensorScores + m_cScores);

      const TermFeature* pTermFeature2 = pTerm->GetTermFeatures();
      size_t iDimension = 0;
      do {
         const FeatureBoosting* const pFeature = pTermFeature2->m_pFeature;
         const size_t cBins = pFeature->GetCountBins();
         EBM_ASSERT(size_t{1} <= cBins); // we exited above on tensors with zero bins in any dimension
         const size_t cSlices = cBins;
         const DimensionInfo* const pDimension = &aDimension1[iDimension];
         if(cSlices != pDimension->m_cSlices) {
            error = SetCountSlices(iDimension, cSlices);
            if(UNLIKELY(Error_None != error)) {
               // already logged
               return error;
            }

            // if cSlices is 1 then pDimension->m_cSlices must be 1 and we'd be filtered out above
            EBM_ASSERT(size_t{2} <= cSlices);

            UIntSplit* const aSplit = pDimension->m_aSplits;
            size_t iEdge = 1;
            do {
               // we checked earlier that countBins could be converted to a UIntSplit
               EBM_ASSERT(!IsConvertError<UIntSplit>(iEdge));
               aSplit[iEdge - 1] = static_cast<UIntSplit>(iEdge);
               ++iEdge;
            } while(cSlices != iEdge);
         }
         ++iDimension;
         ++pTermFeature2;
      } while(pTermFeaturesEnd != pTermFeature2);
   }
   m_bExpanded = true;

   LOG_0(Trace_Verbose, "Exited Expand");
   return Error_None;
}

void Tensor::AddExpandedWithBadValueProtection(const FloatScore* const aFromScores) {
   EBM_ASSERT(m_bExpanded);
   size_t cItems = m_cScores;

   const DimensionInfo* const aDimension = GetDimensions();
   for(size_t iDimension = 0; iDimension < m_cDimensions; ++iDimension) {
      // this can't overflow since we've already allocated them!
      cItems *= aDimension[iDimension].m_cSlices;
   }

   const FloatScore* pFromScore = aFromScores;
   FloatScore* pToScore = m_aTensorScores;
   const FloatScore* const pToScoresEnd = m_aTensorScores + cItems;
   do {
      // if we get a NaN value, then just consider it a no-op zero
      // if we get a +infinity, then just make our value the maximum
      // if we get a -infinity, then just make our value the minimum
      // these changes will make us out of sync with the updates to our logits, but it should be at the extremes anyways
      // so, not much real loss there.  Also, if we have NaN, or +-infinity in an update, we'll be stopping boosting
      // soon but we want to preserve the best term scores that we had

      FloatScore score = *pFromScore;
      score = std::isnan(score) ? FloatScore{0} : score;
      score = *pToScore + score;
      // this is a check for -infinity, without the -infinity value since some compilers make that illegal
      // even so far as to make isinf always FALSE with some compiler flags
      // include the equals case so that the compiler is less likely to optimize that out
      score = score <= std::numeric_limits<FloatScore>::lowest() ? std::numeric_limits<FloatScore>::lowest() : score;
      // this is a check for +infinity, without the +infinity value since some compilers make that illegal
      // even so far as to make isinf always FALSE with some compiler flags
      // include the equals case so that the compiler is less likely to optimize that out
      score = std::numeric_limits<FloatScore>::max() <= score ? std::numeric_limits<FloatScore>::max() : score;
      *pToScore = score;
      ++pFromScore;
      ++pToScore;
   } while(pToScoresEnd != pToScore);
}

ErrorEbm Tensor::Add(const Tensor& rhs) {
   ErrorEbm error;

   DimensionInfoStack dimensionStack[k_cDimensionsMax];

   EBM_ASSERT(m_cDimensions == rhs.m_cDimensions);

   if(0 == m_cDimensions) {
      EBM_ASSERT(1 <= m_cTensorScoreCapacity);
      EBM_ASSERT(nullptr != m_aTensorScores);

      FloatScore* pTo = &m_aTensorScores[0];
      const FloatScore* pFrom = &rhs.m_aTensorScores[0];
      const FloatScore* const pToEnd = &pTo[m_cScores];
      do {
         *pTo += *pFrom;
         ++pTo;
         ++pFrom;
      } while(pToEnd != pTo);

      return Error_None;
   }

   if(m_bExpanded) {
      // TODO: the existing code below works, but handle this differently (we can do it more efficiently)
   }

   if(rhs.m_bExpanded) {
      // TODO: the existing code below works, but handle this differently (we can do it more efficiently)
   }

   const DimensionInfo* pDimensionFirst1 = GetDimensions();
   const DimensionInfo* pDimensionFirst2 = rhs.GetDimensions();

   DimensionInfoStack* pDimensionInfoStackFirst = dimensionStack;
   const DimensionInfoStack* const pDimensionInfoStackEnd = &dimensionStack[m_cDimensions];

   size_t cTensorScores1 = m_cScores;
   size_t cTensorScores2 = m_cScores;
   size_t cNewTensorScores = m_cScores;

   EBM_ASSERT(1 <= m_cDimensions);
   // first, get basic counts of how many splits and values we'll have in our final result
   do {
      const size_t cSlices1 = pDimensionFirst1->m_cSlices;
      UIntSplit* p1Cur = pDimensionFirst1->m_aSplits;
      const size_t cSlices2 = pDimensionFirst2->m_cSlices;
      UIntSplit* p2Cur = pDimensionFirst2->m_aSplits;

      cTensorScores1 *= cSlices1; // this can't overflow since we're counting existing allocated memory
      cTensorScores2 *= cSlices2; // this can't overflow since we're counting existing allocated memory

      UIntSplit* const p1End = &p1Cur[cSlices1 - 1];
      UIntSplit* const p2End = &p2Cur[cSlices2 - 1];

      pDimensionInfoStackFirst->m_pSplit1 = p1End;
      pDimensionInfoStackFirst->m_pSplit2 = p2End;

      size_t cNewSingleDimensionSlices = 1;

      // processing forwards here is slightly faster in terms of cache fetch efficiency.  We'll then be guaranteed to
      // have the splits at least in the cache, which will be benefitial when traversing backwards later below
      while(true) {
         if(UNLIKELY(p2End == p2Cur)) {
            // check the other array first.  Most of the time the other array will be shorter since we'll be adding
            // a sequence of sliced lines and our main line will be in *this, and there will be more slices in general
            // for a line that is added to a lot
            cNewSingleDimensionSlices += static_cast<size_t>(p1End - p1Cur);
            break;
         }
         if(UNLIKELY(p1End == p1Cur)) {
            cNewSingleDimensionSlices += static_cast<size_t>(p2End - p2Cur);
            break;
         }
         ++cNewSingleDimensionSlices; // if we move one or both pointers, we just added annother unique one

         const UIntSplit d1 = *p1Cur;
         const UIntSplit d2 = *p2Cur;

         p1Cur = UNPREDICTABLE(d1 <= d2) ? p1Cur + 1 : p1Cur;
         p2Cur = UNPREDICTABLE(d2 <= d1) ? p2Cur + 1 : p2Cur;
      }
      pDimensionInfoStackFirst->m_cNewSlices = cNewSingleDimensionSlices;
      // we check for simple multiplication overflow from m_cBins in Booster::Initialize when we unpack featureIndexes
      // and in CalcInteractionStrength for interactions
      EBM_ASSERT(!IsMultiplyError(cNewTensorScores, cNewSingleDimensionSlices));
      cNewTensorScores *= cNewSingleDimensionSlices;

      ++pDimensionFirst1;
      ++pDimensionFirst2;

      ++pDimensionInfoStackFirst;
   } while(pDimensionInfoStackEnd != pDimensionInfoStackFirst);

   // call EnsureTensorScoreCapacity before using the m_aTensorScores pointer since m_aTensorScores might change inside
   // EnsureTensorScoreCapacity
   error = EnsureTensorScoreCapacity(cNewTensorScores);
   if(UNLIKELY(Error_None != error)) {
      // already logged
      return error;
   }

   const FloatScore* pTensorScore2 =
         &rhs.m_aTensorScores[cTensorScores2]; // we're accessing allocated memory, so it can't overflow
   const DimensionInfo* const aDimension2 = rhs.GetDimensions();

   FloatScore* const aTensorScores = m_aTensorScores;
   const DimensionInfo* const aDimension1 = GetDimensions();

   const FloatScore* pTensorScore1 =
         &aTensorScores[cTensorScores1]; // we're accessing allocated memory, so it can't overflow
   FloatScore* pTensorScoreTop =
         &aTensorScores[cNewTensorScores]; // we're accessing allocated memory, so it can't overflow

   // traverse the scores in reverse so that we can put our results at the higher order indexes where we are guaranteed
   // not to overwrite our existing scores which we still need to copy first do the scores because we need to refer to
   // the old splits when making decisions about where to move next
   while(true) {
      const FloatScore* pTensorScore1Move = pTensorScore1;
      const FloatScore* pTensorScore2Move = pTensorScore2;
      const FloatScore* const pTensorScoreTopEnd = pTensorScoreTop - m_cScores;
      do {
         --pTensorScore1Move;
         --pTensorScore2Move;
         --pTensorScoreTop;
         *pTensorScoreTop = *pTensorScore1Move + *pTensorScore2Move;
      } while(pTensorScoreTopEnd != pTensorScoreTop);

      // For a single dimensional Tensor checking here is best.
      // For two or higher dimensions, we could instead check inside our loop below for when we reach the end of the
      // pDimensionInfoStack, thus eliminating the check on most loops.  We'll spend most of our time working on single
      // features though, so we optimize for that case, but if we special cased the single dimensional case, then we
      // would want to move this check into the loop below in the case of multi-dimensioncal Tensors
      if(UNLIKELY(aTensorScores == pTensorScoreTop)) {
         // we've written our final tensor cell, so we're done
         break;
      }

      DimensionInfoStack* pDimensionInfoStackSecond = dimensionStack;
      const DimensionInfo* pDimensionSecond1 = aDimension1;
      const DimensionInfo* pDimensionSecond2 = aDimension2;

      size_t multiplication1 = m_cScores;
      size_t multiplication2 = m_cScores;

      while(true) {
         const UIntSplit* const pSplit1 = pDimensionInfoStackSecond->m_pSplit1;
         const UIntSplit* const pSplit2 = pDimensionInfoStackSecond->m_pSplit2;

         UIntSplit* const aSplits1 = pDimensionSecond1->m_aSplits;
         UIntSplit* const aSplits2 = pDimensionSecond2->m_aSplits;

         if(UNPREDICTABLE(aSplits1 < pSplit1)) {
            if(UNPREDICTABLE(aSplits2 < pSplit2)) {
               const UIntSplit* const pSplit1MinusOne = pSplit1 - 1;
               const UIntSplit* const pSplit2MinusOne = pSplit2 - 1;

               const UIntSplit d1 = *pSplit1MinusOne;
               const UIntSplit d2 = *pSplit2MinusOne;

               const bool bMove1 = UNPREDICTABLE(d2 <= d1);
               pDimensionInfoStackSecond->m_pSplit1 = bMove1 ? pSplit1MinusOne : pSplit1;
               pTensorScore1 = bMove1 ? pTensorScore1 - multiplication1 : pTensorScore1;

               const bool bMove2 = UNPREDICTABLE(d1 <= d2);
               pDimensionInfoStackSecond->m_pSplit2 = bMove2 ? pSplit2MinusOne : pSplit2;
               pTensorScore2 = bMove2 ? pTensorScore2 - multiplication2 : pTensorScore2;
               break;
            } else {
               pTensorScore1 -= multiplication1;
               pDimensionInfoStackSecond->m_pSplit1 = pSplit1 - 1;
               break;
            }
         } else {
            if(UNPREDICTABLE(aSplits2 < pSplit2)) {
               pTensorScore2 -= multiplication2;
               pDimensionInfoStackSecond->m_pSplit2 = pSplit2 - 1;
               break;
            } else {
               pTensorScore1 -= multiplication1; // put us before the beginning.  We'll add the full row first
               pTensorScore2 -= multiplication2; // put us before the beginning.  We'll add the full row first

               const size_t cSlices1 = pDimensionSecond1->m_cSlices;
               const size_t cSlices2 = pDimensionSecond2->m_cSlices;

               EBM_ASSERT(!IsMultiplyError(
                     multiplication1, cSlices1)); // we're accessing allocated memory, so it can't overflow
               multiplication1 *= cSlices1;
               EBM_ASSERT(!IsMultiplyError(
                     multiplication2, cSlices2)); // we're accessing allocated memory, so it can't overflow
               multiplication2 *= cSlices2;

               // go to the last valid entry back to where we started.  If we don't move down a set, then we re-do this
               // set of numbers
               pTensorScore1 += multiplication1;
               // go to the last valid entry back to where we started.  If we don't move down a set, then we re-do this
               // set of numbers
               pTensorScore2 += multiplication2;

               pDimensionInfoStackSecond->m_pSplit1 = &aSplits1[cSlices1 - 1];
               pDimensionInfoStackSecond->m_pSplit2 = &aSplits2[cSlices2 - 1];
               ++pDimensionSecond1;
               ++pDimensionSecond2;
               ++pDimensionInfoStackSecond;
               continue;
            }
         }
      }
   }

   EBM_ASSERT(pTensorScoreTop == m_aTensorScores);
   EBM_ASSERT(pTensorScore1 == m_aTensorScores + m_cScores);
   EBM_ASSERT(pTensorScore2 == rhs.m_aTensorScores + m_cScores);

   // now finally do the splits

   const DimensionInfoStack* pDimensionInfoStackCur = dimensionStack;
   const DimensionInfo* pDimension1Cur = aDimension1;
   const DimensionInfo* pDimension2Cur = aDimension2;
   size_t iDimension = 0;
   do {
      const size_t cNewSlices = pDimensionInfoStackCur->m_cNewSlices;
      const size_t cOriginalSlicesBeforeSetting = pDimension1Cur->m_cSlices;

      // this will increase our capacity, if required.  It will also change m_cSplits, so we get that before calling it.
      // SetCountSplits might change m_aTensorScoresAndSplits, so we need to actually keep it here after getting
      // m_cSplits but before set set all our pointers
      error = SetCountSlices(iDimension, cNewSlices);
      if(UNLIKELY(Error_None != error)) {
         // already logged
         return error;
      }

      const UIntSplit* p1Cur = &pDimension1Cur->m_aSplits[cOriginalSlicesBeforeSetting - 1];
      const UIntSplit* p2Cur = &pDimension2Cur->m_aSplits[pDimension2Cur->m_cSlices - 1];
      UIntSplit* pTopCur = &pDimension1Cur->m_aSplits[cNewSlices - 1];

      // traverse in reverse so that we can put our results at the higher order indexes where we are guaranteed not to
      // overwrite our existing scores which we still need to copy
      while(true) {
         EBM_ASSERT(pDimension1Cur->m_aSplits <= pTopCur);
         EBM_ASSERT(pDimension1Cur->m_aSplits <= p1Cur);
         EBM_ASSERT(pDimension2Cur->m_aSplits <= p2Cur);
         EBM_ASSERT(p1Cur <= pTopCur);
         EBM_ASSERT(static_cast<size_t>(p2Cur - pDimension2Cur->m_aSplits) <=
               static_cast<size_t>(pTopCur - pDimension1Cur->m_aSplits));

         if(UNLIKELY(pTopCur == p1Cur)) {
            // since we've finished the rhs splits, our Tensor already has the right splits in place, so all we need is
            // to add the score of the last region in rhs to our remaining scores
            break;
         }
         // pTopCur is an index above pDimension1Cur->m_aSplits.  p2Cur is an index above pDimension2Cur->m_aSplits.  We
         // want to decide if they are at the same index above their respective arrays
         if(UNLIKELY(static_cast<size_t>(pTopCur - pDimension1Cur->m_aSplits) ==
                  static_cast<size_t>(p2Cur - pDimension2Cur->m_aSplits))) {
            EBM_ASSERT(pDimension1Cur->m_aSplits < pTopCur);
            // direct copy the remaining splits.  There should be at least one
            memcpy(pDimension1Cur->m_aSplits,
                  pDimension2Cur->m_aSplits,
                  static_cast<size_t>(pTopCur - pDimension1Cur->m_aSplits) * sizeof(UIntSplit));
            break;
         }

         const UIntSplit* const p1CurMinusOne = p1Cur - 1;
         const UIntSplit* const p2CurMinusOne = p2Cur - 1;

         const UIntSplit d1 = *p1CurMinusOne;
         const UIntSplit d2 = *p2CurMinusOne;

         p1Cur = UNPREDICTABLE(d2 <= d1) ? p1CurMinusOne : p1Cur;
         p2Cur = UNPREDICTABLE(d1 <= d2) ? p2CurMinusOne : p2Cur;

         const UIntSplit d = UNPREDICTABLE(d1 <= d2) ? d2 : d1;

         --pTopCur; // if we move one or both pointers, we just added annother unique one
         *pTopCur = d;
      }
      ++pDimension1Cur;
      ++pDimension2Cur;
      ++pDimensionInfoStackCur;
      ++iDimension;
   } while(iDimension != m_cDimensions);
   return Error_None;
}

#ifndef NDEBUG
bool Tensor::IsEqual(const Tensor& rhs) const {
   if(m_cDimensions != rhs.m_cDimensions) {
      return false;
   }

   const DimensionInfo* pThisDimensionInfo = GetDimensions();
   const DimensionInfo* pRhsDimensionInfo = rhs.GetDimensions();

   size_t cTensorScores = m_cScores;
   for(size_t iDimension = 0; iDimension < m_cDimensions; ++iDimension) {
      const DimensionInfo* const pDimension1 = &pThisDimensionInfo[iDimension];
      const DimensionInfo* const pDimension2 = &pRhsDimensionInfo[iDimension];

      size_t cSlices = pDimension1->m_cSlices;
      if(cSlices != pDimension2->m_cSlices) {
         return false;
      }

      if(1 < cSlices) {
         EBM_ASSERT(!IsMultiplyError(cTensorScores, cSlices)); // we're accessing allocated memory, so it can't overflow
         cTensorScores *= cSlices;

         const UIntSplit* pD1Cur = pDimension1->m_aSplits;
         const UIntSplit* pD2Cur = pDimension2->m_aSplits;
         const UIntSplit* const pD1End = pD1Cur + cSlices - 1;
         do {
            if(UNLIKELY(*pD1Cur != *pD2Cur)) {
               return false;
            }
            ++pD1Cur;
            ++pD2Cur;
         } while(LIKELY(pD1End != pD1Cur));
      }
   }

   const FloatScore* pV1Cur = &m_aTensorScores[0];
   const FloatScore* pV2Cur = &rhs.m_aTensorScores[0];
   const FloatScore* const pV1End = pV1Cur + cTensorScores;
   do {
      if(UNLIKELY(*pV1Cur != *pV2Cur)) {
         return false;
      }
      ++pV1Cur;
      ++pV2Cur;
   } while(LIKELY(pV1End != pV1Cur));

   return true;
}
#endif // NDEBUG

} // namespace DEFINED_ZONE_NAME

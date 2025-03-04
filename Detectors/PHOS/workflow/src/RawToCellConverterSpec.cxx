// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.
#include <string>
#include "FairLogger.h"
#include "CommonDataFormat/InteractionRecord.h"
#include "Framework/ConfigParamRegistry.h"
#include "Framework/ControlService.h"
#include "Framework/WorkflowSpec.h"
#include "DataFormatsPHOS/PHOSBlockHeader.h"
#include "DataFormatsPHOS/TriggerRecord.h"
#include "DetectorsRaw/RDHUtils.h"
#include "Framework/InputRecordWalker.h"
#include "Framework/DataRefUtils.h"
#include "CCDB/CcdbApi.h"
#include "PHOSBase/Mapping.h"
#include "PHOSBase/PHOSSimParams.h"
#include "PHOSReconstruction/CaloRawFitter.h"
#include "PHOSReconstruction/CaloRawFitterGS.h"
#include "PHOSReconstruction/RawDecodingError.h"
#include "PHOSWorkflow/RawToCellConverterSpec.h"
#include "CommonUtils/VerbosityConfig.h"

using namespace o2::phos::reco_workflow;

void RawToCellConverterSpec::init(framework::InitContext& ctx)
{
  LOG(debug) << "Initialize converter ";

  auto path = ctx.options().get<std::string>("mappingpath");
  Mapping::Instance(path);

  auto fitmethod = ctx.options().get<std::string>("fitmethod");
  if (fitmethod == "default") {
    LOG(info) << "Using default raw fitter";
    mRawFitter = std::unique_ptr<o2::phos::CaloRawFitter>(new o2::phos::CaloRawFitter);
  }
  if (fitmethod == "semigaus") {
    LOG(info) << "Using SemiGauss raw fitter";
    mRawFitter = std::unique_ptr<o2::phos::CaloRawFitter>(new o2::phos::CaloRawFitterGS);
  }

  mFillChi2 = (ctx.options().get<std::string>("fillchi2").compare("on") == 0);
  if (mFillChi2) {
    LOG(info) << "Fit quality output will be filled";
  }

  mDecoder = std::make_unique<AltroDecoder>();

  mPedestalRun = (ctx.options().get<std::string>("pedestal").find("on") != std::string::npos);
  if (mPedestalRun) {
    mRawFitter->setPedestal();
    mDecoder->setPedestalRun(); // sets also keeping both HG and LG channels
    LOG(info) << "Pedestal run will be processed";
  }

  mCombineGHLG = (ctx.options().get<std::string>("keepHGLG").compare("on") != 0);
  if (!mCombineGHLG) {
    mDecoder->setCombineHGLG(false);
    LOG(info) << "Both HighGain and LowGain will be kept";
  }
  int presamples = ctx.options().get<int>("presamples");
  mDecoder->setPresamples(presamples);
  LOG(info) << "Using " << presamples << " pre-samples";
}

void RawToCellConverterSpec::run(framework::ProcessingContext& ctx)
{
  // Cache cells from bunch crossings as the component reads timeframes from many links consecutively
  std::vector<o2::InteractionRecord> irList;
  std::vector<std::array<short, 56>> cellTRURanges; // start/end points for cells in mTmpCells[ddl] arrays
  for (int iddl = 14; iddl--;) {
    mTmpCells[iddl].clear();
    mTmpTRU[iddl].clear();
  }
  mOutputHWErrors.clear();
  if (mFillChi2) {
    mOutputFitChi.clear();
  }

  // if we see requested data type input with 0xDEADBEEF subspec and 0 payload this means that the "delayed message"
  // mechanism created it in absence of real data from upstream. Processor should send empty output to not block the workflow
  static size_t contDeadBeef = 0; // number of times 0xDEADBEEF was seen continuously
  std::vector<o2::framework::InputSpec> dummy{o2::framework::InputSpec{"dummy", o2::framework::ConcreteDataMatcher{"PHS", o2::header::gDataDescriptionRawData, 0xDEADBEEF}}};
  for (const auto& ref : framework::InputRecordWalker(ctx.inputs(), dummy)) {
    const auto dh = o2::framework::DataRefUtils::getHeader<o2::header::DataHeader*>(ref);
    auto payloadSize = o2::framework::DataRefUtils::getPayloadSize(ref);
    if (payloadSize == 0) { // send empty output
      auto maxWarn = o2::conf::VerbosityConfig::Instance().maxWarnDeadBeef;
      if (++contDeadBeef <= maxWarn) {
        LOGP(warning, "Found input [{}/{}/{:#x}] TF#{} 1st_orbit:{} Payload {} : assuming no payload for all links in this TF{}",
             dh->dataOrigin.str, dh->dataDescription.str, dh->subSpecification, dh->tfCounter, dh->firstTForbit, payloadSize,
             contDeadBeef == maxWarn ? fmt::format(". {} such inputs in row received, stopping reporting", contDeadBeef) : "");
      }
      mOutputCells.clear();
      ctx.outputs().snapshot(o2::framework::Output{"PHS", "CELLS", mflpId, o2::framework::Lifetime::Timeframe}, mOutputCells);
      mOutputTriggerRecords.clear();
      ctx.outputs().snapshot(o2::framework::Output{"PHS", "CELLTRIGREC", mflpId, o2::framework::Lifetime::Timeframe}, mOutputTriggerRecords);
      mOutputHWErrors.clear();
      ctx.outputs().snapshot(o2::framework::Output{"PHS", "RAWHWERRORS", 0, o2::framework::Lifetime::Timeframe}, mOutputHWErrors);
      if (mFillChi2) {
        mOutputFitChi.clear();
        ctx.outputs().snapshot(o2::framework::Output{"PHS", "CELLFITQA", 0, o2::framework::Lifetime::QA}, mOutputFitChi);
      }
      return; // empty TF, nothing to process
    }
  }
  contDeadBeef = 0; // if good data, reset the counter

  std::vector<o2::framework::InputSpec> inputFilter{o2::framework::InputSpec{"filter", o2::framework::ConcreteDataTypeMatcher{"PHS", "RAWDATA"}, o2::framework::Lifetime::Timeframe}};
  for (const auto& rawData : framework::InputRecordWalker(ctx.inputs(), inputFilter)) {

    const gsl::span<const char>& rawmemory = o2::framework::DataRefUtils::as<const char>(rawData);
    if (rawmemory.size() == 0) {
      continue;
    }

    o2::phos::RawReaderMemory rawreader(rawmemory);

    // loop over all the DMA pages
    while (rawreader.hasNext()) {
      try {
        rawreader.next();
      } catch (RawDecodingError::ErrorType_t e) {
        // LOG(error) << "Raw decoding error " << (int)e;
        // add error list
        mOutputHWErrors.emplace_back(14, (int)e, 1); // Put general errors to non-existing DDL14
        // if problem in header, abandon this page
        if (e == RawDecodingError::ErrorType_t::PAGE_NOTFOUND ||
            e == RawDecodingError::ErrorType_t::HEADER_DECODING ||
            e == RawDecodingError::ErrorType_t::HEADER_INVALID) {
          break;
        }
        // if problem in payload, try to continue
        continue;
      }
      auto& header = rawreader.getRawHeader();
      auto triggerBC = o2::raw::RDHUtils::getTriggerBC(header);
      auto triggerOrbit = o2::raw::RDHUtils::getTriggerOrbit(header);
      auto ddl = o2::raw::RDHUtils::getFEEID(header);

      if (ddl >= o2::phos::Mapping::NDDL || ddl < 0) { // only 0..13 correct DDLs
        LOG(error) << "DDL=" << ddl;
        mOutputHWErrors.emplace_back(14, 16, char(ddl)); // Add non-existing DDL as DDL 15
        continue;                                        // skip STU ddl
      }

      o2::InteractionRecord currentIR(triggerBC, triggerOrbit);
      auto irIter = irList.rbegin();
      auto rangeIter = cellTRURanges.rbegin();
      while (irIter != irList.rend() && *irIter != currentIR) {
        irIter++;
        rangeIter++;
      }
      if (irIter != irList.rend()) {                      // found
        (*rangeIter)[2 * ddl] = mTmpCells[ddl].size();    // start of the cell list
        (*rangeIter)[28 + 2 * ddl] = mTmpTRU[ddl].size(); // start of the tru list
      } else {                                            // create new entry
        irList.push_back(currentIR);
        cellTRURanges.emplace_back();
        cellTRURanges.back().fill(0);
        cellTRURanges.back()[2 * ddl] = mTmpCells[ddl].size();
        cellTRURanges.back()[28 + 2 * ddl] = mTmpTRU[ddl].size();
        rangeIter = cellTRURanges.rbegin();
      }
      std::vector<Cell>& currentCellContainer = mTmpCells[ddl];
      std::vector<Cell>& currentTRUContainer = mTmpTRU[ddl];

      // use the altro decoder to decode the raw data, and extract the RCU trailer
      mDecoder->decode(rawreader, mRawFitter.get(), currentCellContainer, currentTRUContainer);
      const std::vector<o2::phos::RawReaderError>& errs = mDecoder->hwerrors();
      for (auto a : errs) {
        mOutputHWErrors.emplace_back(a);
      }
      if (mFillChi2) {
        const std::vector<short>& chi2list = mDecoder->chi2list();
        for (auto a : chi2list) {
          mOutputFitChi.emplace_back(a);
        }
      }

      // Sort cells according to cell ID
      (*rangeIter)[2 * ddl + 1] = currentCellContainer.size();
      auto itBegin = currentCellContainer.begin() + (*rangeIter)[2 * ddl];
      std::sort(itBegin, currentCellContainer.end(), [](o2::phos::Cell& lhs, o2::phos::Cell& rhs) { return lhs.getAbsId() < rhs.getAbsId(); });
      auto itTrBegin = currentTRUContainer.begin() + (*rangeIter)[28 + 2 * ddl];
      (*rangeIter)[28 + 2 * ddl + 1] = currentTRUContainer.size();
      std::sort(itTrBegin, currentTRUContainer.end(), [](o2::phos::Cell& lhs, o2::phos::Cell& rhs) { return lhs.getAbsId() < rhs.getAbsId(); });
    } // RawReader::hasNext
  }

  // Loop over BCs, sort cells with increasing cell ID and write to output containers
  mOutputCells.clear();
  if (mLastSize > 0) {
    mOutputCells.reserve(mLastSize);
  }
  mOutputTriggerRecords.clear();
  auto rangeIter = cellTRURanges.begin();
  for (auto irIter = irList.begin(); irIter != irList.end(); ++irIter, ++rangeIter) {
    // find all DDLs for current BC
    // sort separately then concatenate
    int prevCellSize = mOutputCells.size();
    for (int iddl = 0; iddl < 14; iddl++) {
      auto cbegin = mTmpCells[iddl].begin() + (*rangeIter)[2 * iddl];
      auto cend = mTmpCells[iddl].begin() + (*rangeIter)[2 * iddl + 1];

      if (mCombineGHLG && !mPedestalRun) { // combine for normal data, do not combine e.g. for LED run and pedestal
        // Combine HG and LG sells
        // Should be next to each other after sorting
        auto it1 = cbegin;
        auto it2 = cbegin;
        it2++;
        while (it1 != cend) {
          if (it2 != cend) {
            if ((*it1).getAbsId() == (*it2).getAbsId()) { // HG and LG channels, if both, copy only HG as more precise
              if ((*it1).getType() == o2::phos::HIGH_GAIN) {
                mOutputCells.push_back(*it1);
              } else {
                mOutputCells.push_back(*it2);
              }
              ++it1; // yes increase twice
              ++it2;
            } else { // no double cells, copy this one
              mOutputCells.push_back(*it1);
            }
          } else { // just copy last one
            mOutputCells.push_back(*it1);
          }
          ++it1;
          ++it2;
        }
      } else {
        mOutputCells.insert(mOutputCells.end(), cbegin, cend);
      }
    } // all readout cells
    for (int iddl = 0; iddl < 14; iddl++) {
      auto trbegin = mTmpTRU[iddl].begin() + (*rangeIter)[28 + 2 * iddl];
      auto trend = mTmpTRU[iddl].begin() + (*rangeIter)[28 + 2 * iddl + 1];
      // Move trigger cells
      for (auto tri = trbegin; tri != trend; tri++) {
        if (tri->getEnergy() > 0) {
          mOutputCells.emplace_back(tri->getAbsId(), tri->getEnergy(), tri->getTime(), tri->getType());
        }
      }
    }

    mOutputTriggerRecords.emplace_back(*irIter, prevCellSize, mOutputCells.size() - prevCellSize);
  }

  mLastSize = 1.1 * mOutputCells.size();

  LOG(debug) << "[PHOSRawToCellConverter - run] Writing " << mOutputCells.size() << " cells ...";
  ctx.outputs().snapshot(o2::framework::Output{"PHS", "CELLS", mflpId, o2::framework::Lifetime::Timeframe}, mOutputCells);
  ctx.outputs().snapshot(o2::framework::Output{"PHS", "CELLTRIGREC", mflpId, o2::framework::Lifetime::Timeframe}, mOutputTriggerRecords);
  ctx.outputs().snapshot(o2::framework::Output{"PHS", "RAWHWERRORS", 0, o2::framework::Lifetime::Timeframe}, mOutputHWErrors);
  if (mFillChi2) {
    ctx.outputs().snapshot(o2::framework::Output{"PHS", "CELLFITQA", 0, o2::framework::Lifetime::QA}, mOutputFitChi);
  }
}

o2::framework::DataProcessorSpec o2::phos::reco_workflow::getRawToCellConverterSpec(unsigned int flpId)
{
  std::vector<o2::framework::InputSpec> inputs;
  inputs.emplace_back("RAWDATA", o2::framework::ConcreteDataTypeMatcher{"PHS", "RAWDATA"}, o2::framework::Lifetime::Optional);
  // receive at least 1 guaranteed input (which will allow to acknowledge the TF)
  inputs.emplace_back("STFDist", "FLP", "DISTSUBTIMEFRAME", 0, o2::framework::Lifetime::Timeframe);

  std::vector<o2::framework::OutputSpec> outputs;
  outputs.emplace_back("PHS", "CELLS", flpId, o2::framework::Lifetime::Timeframe);
  outputs.emplace_back("PHS", "CELLTRIGREC", flpId, o2::framework::Lifetime::Timeframe);
  outputs.emplace_back("PHS", "RAWHWERRORS", 0, o2::framework::Lifetime::Timeframe);
  outputs.emplace_back("PHS", "CELLFITQA", 0, o2::framework::Lifetime::QA);

  return o2::framework::DataProcessorSpec{"PHOSRawToCellConverterSpec",
                                          inputs, // o2::framework::select("A:PHS/RAWDATA"),
                                          outputs,
                                          o2::framework::adaptFromTask<o2::phos::reco_workflow::RawToCellConverterSpec>(flpId),
                                          o2::framework::Options{
                                            {"presamples", o2::framework::VariantType::Int, 2, {"presamples time offset"}},
                                            {"fitmethod", o2::framework::VariantType::String, "default", {"Fit method (default or semigaus)"}},
                                            {"mappingpath", o2::framework::VariantType::String, "", {"Path to mapping files"}},
                                            {"fillchi2", o2::framework::VariantType::String, "off", {"Fill sample qualities on/off"}},
                                            {"keepHGLG", o2::framework::VariantType::String, "off", {"keep HighGain and Low Gain signals on/off"}},
                                            {"pedestal", o2::framework::VariantType::String, "off", {"Analyze as pedestal run on/off"}}}};
}

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

#ifndef O2_TOF_DIAGNOSTIC_CALIBRATOR_H
#define O2_TOF_DIAGNOSTIC_CALIBRATOR_H

/// @file   TOFDiagnosticCalibratorSpec.h
/// @brief  Device to stor in CCDB the diagnostic words from TOF

#include "TOFCalibration/TOFDiagnosticCalibrator.h"
#include "DetectorsCalibration/Utils.h"
#include "CommonUtils/MemFileHelper.h"
#include "Framework/Task.h"
#include "Framework/ConfigParamRegistry.h"
#include "Framework/ControlService.h"
#include "Framework/WorkflowSpec.h"
#include "CCDB/CcdbApi.h"
#include "CCDB/CcdbObjectInfo.h"
#include "DetectorsRaw/HBFUtils.h"

using namespace o2::framework;

namespace o2
{
namespace calibration
{

class TOFDiagnosticCalibDevice : public o2::framework::Task
{
 public:
  TOFDiagnosticCalibDevice(int runnumber = -1) : mRunNumber(runnumber) {}
  void init(o2::framework::InitContext& ic) final
  {
    int slotL = ic.options().get<int>("tf-per-slot");
    int delay = ic.options().get<int>("max-delay");
    mCalibrator = std::make_unique<o2::tof::TOFDiagnosticCalibrator>();
    mCalibrator->setSlotLength(slotL);
    mCalibrator->setMaxSlotsDelay(delay);
    mCalibrator->setRunNumber(mRunNumber);
  }

  void run(o2::framework::ProcessingContext& pc) final
  {
    o2::base::TFIDInfoHelper::fillTFIDInfo(pc, mCalibrator->getCurrentTFInfo());
    auto const data = pc.inputs().get<o2::tof::Diagnostic*>("input");
    LOG(info) << "Processing TF " << mCalibrator->getCurrentTFInfo().tfCounter;
    mCalibrator->process<o2::tof::Diagnostic>(*data);
    sendOutput(pc.outputs());
  }

  void endOfStream(o2::framework::EndOfStreamContext& ec) final
  {
    LOG(info) << "Finalizing calibration";
    constexpr uint64_t INFINITE_TF = 0xffffffffffffffff;
    mCalibrator->checkSlotsToFinalize(INFINITE_TF);
    sendOutput(ec.outputs());
  }

 private:
  std::unique_ptr<o2::tof::TOFDiagnosticCalibrator> mCalibrator;
  int mRunNumber = -1;

  //________________________________________________________________
  void sendOutput(DataAllocator& output)
  {
    // extract CCDB infos and calibration objects, convert it to TMemFile and send them to the output
    // TODO in principle, this routine is generic, can be moved to Utils.h
    using clbUtils = o2::calibration::Utils;
    const auto& payloadVec = mCalibrator->getDiagnosticVector();
    auto& infoVec = mCalibrator->getDiagnosticInfoVector(); // use non-const version as we update it
    assert(payloadVec.size() == infoVec.size());
    for (uint32_t i = 0; i < payloadVec.size(); i++) {
      auto& w = infoVec[i];
      auto image = o2::ccdb::CcdbApi::createObjectImage(&payloadVec[i], &w);
      LOG(info) << "Sending object " << w.getPath() << "/" << w.getFileName() << " of size " << image->size()
                << " bytes, valid for " << w.getStartValidityTimestamp() << " : " << w.getEndValidityTimestamp();
      output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBPayload, "TOF_Diagnostic", i}, *image.get()); // vector<char>
      output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBWrapper, "TOF_Diagnostic", i}, w);            // root-serialized
    }
    if (payloadVec.size()) {
      mCalibrator->initOutput(); // reset the outputs once they are already sent
    }
  }
};

} // namespace calibration

namespace framework
{

DataProcessorSpec getTOFDiagnosticCalibDeviceSpec(int runnumber)
{
  using device = o2::calibration::TOFDiagnosticCalibDevice;
  using clbUtils = o2::calibration::Utils;

  std::vector<OutputSpec> outputs;
  outputs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBPayload, "TOF_Diagnostic"}, Lifetime::Sporadic);
  outputs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBWrapper, "TOF_Diagnostic"}, Lifetime::Sporadic);
  return DataProcessorSpec{
    "tof-diagnostic-calibration",
    Inputs{{"input", "TOF", "DIAFREQ"}},
    outputs,
    AlgorithmSpec{adaptFromTask<device>(runnumber)},
    Options{
      {"tf-per-slot", VariantType::Int, 5, {"number of TFs per calibration time slot"}},
      {"max-delay", VariantType::Int, 3, {"number of slots in past to consider"}}}};
}

} // namespace framework
} // namespace o2

#endif

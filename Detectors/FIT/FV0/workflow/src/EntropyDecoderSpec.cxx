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

/// @file   EntropyDecoderSpec.cxx

#include <vector>

#include "Framework/ControlService.h"
#include "Framework/ConfigParamRegistry.h"
#include "FV0Workflow/EntropyDecoderSpec.h"

using namespace o2::framework;

namespace o2
{
namespace fv0
{

EntropyDecoderSpec::EntropyDecoderSpec(int verbosity)
{
  mTimer.Stop();
  mTimer.Reset();
  mCTFCoder.setVerbosity(verbosity);
}

void EntropyDecoderSpec::init(o2::framework::InitContext& ic)
{
  std::string dictPath = ic.options().get<std::string>("ctf-dict");
  if (!dictPath.empty() && dictPath != "none") {
    mCTFCoder.createCodersFromFile<CTF>(dictPath, o2::ctf::CTFCoderBase::OpType::Decoder);
  }
}

void EntropyDecoderSpec::run(ProcessingContext& pc)
{
  auto cput = mTimer.CpuTime();
  mTimer.Start(false);

  auto buff = pc.inputs().get<gsl::span<o2::ctf::BufferType>>("ctf");

  auto& digits = pc.outputs().make<std::vector<o2::fv0::Digit>>(OutputRef{"digits"});
  auto& channels = pc.outputs().make<std::vector<o2::fv0::ChannelData>>(OutputRef{"channels"});

  // since the buff is const, we cannot use EncodedBlocks::relocate directly, instead we wrap its data to another flat object
  if (buff.size()) {
    const auto ctfImage = o2::fv0::CTF::getImage(buff.data());
    mCTFCoder.decode(ctfImage, digits, channels);
  }
  mTimer.Stop();
  LOG(info) << "Decoded " << channels.size() << " FV0 channels in " << digits.size() << " digits in " << mTimer.CpuTime() - cput << " s";
}

void EntropyDecoderSpec::endOfStream(EndOfStreamContext& ec)
{
  LOGF(info, "FV0 Entropy Decoding total timing: Cpu: %.3e Real: %.3e s in %d slots",
       mTimer.CpuTime(), mTimer.RealTime(), mTimer.Counter() - 1);
}

DataProcessorSpec getEntropyDecoderSpec(int verbosity, unsigned int sspec)
{
  std::vector<OutputSpec> outputs{
    OutputSpec{{"digits"}, "FV0", "DIGITSBC", 0, Lifetime::Timeframe},
    OutputSpec{{"channels"}, "FV0", "DIGITSCH", 0, Lifetime::Timeframe}};

  return DataProcessorSpec{
    "fv0-entropy-decoder",
    Inputs{InputSpec{"ctf", "FV0", "CTFDATA", sspec, Lifetime::Timeframe}},
    outputs,
    AlgorithmSpec{adaptFromTask<EntropyDecoderSpec>(verbosity)},
    Options{{"ctf-dict", VariantType::String, o2::base::NameConf::getCTFDictFileName(), {"File of CTF decoding dictionary"}}}};
}

} // namespace fv0
} // namespace o2

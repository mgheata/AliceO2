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

/// \file   CTFCoder.cxx
/// \author fnoferin@cern.ch
/// \brief class for entropy encoding/decoding of TOF compressed digits data

#include "TOFReconstruction/CTFCoder.h"
#include "CommonUtils/StringUtils.h"
#include <TTree.h>

using namespace o2::tof;

///___________________________________________________________________________________
// Register encoded data in the tree (Fill is not called, will be done by caller)
void CTFCoder::appendToTree(TTree& tree, CTF& ec)
{
  ec.appendToTree(tree, mDet.getName());
}

///___________________________________________________________________________________
// extract and decode data from the tree
void CTFCoder::readFromTree(TTree& tree, int entry, std::vector<ReadoutWindowData>& rofRecVec, std::vector<Digit>& cdigVec, std::vector<uint8_t>& pattVec)
{
  assert(entry >= 0 && entry < tree.GetEntries());
  CTF ec;
  ec.readFromTree(tree, mDet.getName(), entry);
  decode(ec, rofRecVec, cdigVec, pattVec);
}

///________________________________
void CTFCoder::compress(CompressedInfos& cc,
                        const gsl::span<const ReadoutWindowData>& rofRecVec,
                        const gsl::span<const Digit>& cdigVec,
                        const gsl::span<const uint8_t>& pattVec)
{
  // store in the header the orbit of 1st ROF
  cc.clear();
  if (!rofRecVec.size()) {
    return;
  }
  const auto& rofRec0 = rofRecVec[0];
  int nrof = rofRecVec.size();

  LOGF(debug, "TOF compress %d ReadoutWindow with %ld digits", nrof, cdigVec.size());
  cc.header.det = mDet;
  cc.header.nROFs = nrof;
  cc.header.firstOrbit = rofRec0.getBCData().orbit;
  cc.header.firstBC = rofRec0.getBCData().bc;
  cc.header.nPatternBytes = pattVec.size();
  cc.header.nDigits = cdigVec.size();

  cc.bcIncROF.resize(cc.header.nROFs);
  cc.orbitIncROF.resize(cc.header.nROFs);
  cc.ndigROF.resize(cc.header.nROFs);
  cc.ndiaROF.resize(cc.header.nROFs);
  cc.ndiaCrate.resize(cc.header.nROFs * 72);
  //
  cc.timeFrameInc.resize(cc.header.nDigits);
  cc.timeTDCInc.resize(cc.header.nDigits);
  cc.stripID.resize(cc.header.nDigits);
  cc.chanInStrip.resize(cc.header.nDigits);
  cc.tot.resize(cc.header.nDigits);
  cc.pattMap.resize(pattVec.size());

  uint16_t prevBC = cc.header.firstBC;
  uint32_t prevOrbit = cc.header.firstOrbit;

  std::vector<Digit> digCopy;

  for (uint32_t irof = 0; irof < rofRecVec.size(); irof++) {
    const auto& rofRec = rofRecVec[irof];

    const auto& intRec = rofRec.getBCData();
    int64_t rofInBC = intRec.toLong();
    // define interaction record
    if (intRec.orbit == prevOrbit) {
      cc.orbitIncROF[irof] = 0;
      cc.bcIncROF[irof] = intRec.bc - prevBC; // store increment of BC if in the same orbit
    } else {
      cc.orbitIncROF[irof] = intRec.orbit - prevOrbit;
      cc.bcIncROF[irof] = intRec.bc; // otherwise, store absolute bc
      prevOrbit = intRec.orbit;
    }
    prevBC = intRec.bc;
    auto ndig = rofRec.size();
    cc.ndigROF[irof] = ndig;
    cc.ndiaROF[irof] = rofRec.sizeDia();
    for (int icrate = 0; icrate < 72; icrate++) {
      if (rofRec.isEmptyCrate(icrate)) {
        cc.ndiaCrate[irof * 72 + icrate] = 0;
      } else {
        cc.ndiaCrate[irof * 72 + icrate] = rofRec.getDiagnosticInCrate(icrate) + 1; // shifted by one since -1 means crate not available (then to get unsigned int)
      }
    }

    if (!ndig) { // no hits data for this ROF --> not fill
      continue;
    }

    int idigMin = rofRec.first(), idigMax = idigMin + ndig;
    int idig = idigMin;

    // make a copy of digits
    digCopy.clear();
    for (; idig < idigMax; idig++) {
      digCopy.emplace_back(cdigVec[idig]);
    }

    // sort digits according to time (ascending order)
    std::sort(digCopy.begin(), digCopy.end(),
              [](o2::tof::Digit a, o2::tof::Digit b) {
                if (a.getBC() == b.getBC()) {
                  return a.getTDC() < b.getTDC();
                } else {
                  return a.getBC() < b.getBC();
                }
              });

    int timeframe = 0;
    int tdc = 0;
    idig = idigMin;
    for (; idig < idigMax; idig++) {
      const auto& dig = digCopy[idig - idigMin];
      int deltaBC = dig.getBC() - rofInBC;
      int ctimeframe = deltaBC / 64;
      int cTDC = (deltaBC % 64) * 1024 + dig.getTDC();
      if (ctimeframe == timeframe) {
        cc.timeFrameInc[idig] = 0;
        cc.timeTDCInc[idig] = cTDC - tdc;
      } else {
        cc.timeFrameInc[idig] = ctimeframe - timeframe;
        cc.timeTDCInc[idig] = cTDC;
        timeframe = ctimeframe;
      }
      tdc = cTDC;

      int chan = dig.getChannel();
      cc.stripID[idig] = chan / Geo::NPADS;
      cc.chanInStrip[idig] = chan % Geo::NPADS;
      cc.tot[idig] = dig.getTOT();
      LOGF(debug, "%d) TOFBC = %d, deltaBC = %d, TDC = %d, CH=%d", irof, rofInBC, deltaBC, cTDC, chan);
      LOGF(debug, "%d) TF=%d, TDC=%d, STRIP=%d, CH=%d, TOT=%d", idig, cc.timeFrameInc[idig], cc.timeTDCInc[idig], cc.stripID[idig], cc.chanInStrip[idig], cc.tot[idig]);
    }
  }
  memcpy(cc.pattMap.data(), pattVec.data(), cc.header.nPatternBytes); // RSTODO: do we need this?
}

///________________________________
void CTFCoder::createCoders(const std::vector<char>& bufVec, o2::ctf::CTFCoderBase::OpType op)
{
  const auto ctf = CTF::getImage(bufVec.data());
  CompressedInfos cc; // just to get member types
#define MAKECODER(part, slot) createCoder<decltype(part)::value_type>(op, ctf.getFrequencyTable(slot), int(slot))
  // clang-format off
  MAKECODER(cc.bcIncROF,     CTF::BLCbcIncROF);
  MAKECODER(cc.orbitIncROF,  CTF::BLCorbitIncROF);
  MAKECODER(cc.ndigROF,      CTF::BLCndigROF);
  MAKECODER(cc.ndiaROF,      CTF::BLCndiaROF);
  MAKECODER(cc.ndiaCrate,    CTF::BLCndiaCrate);

  MAKECODER(cc.timeFrameInc, CTF::BLCtimeFrameInc);
  MAKECODER(cc.timeTDCInc,   CTF::BLCtimeTDCInc);
  MAKECODER(cc.stripID,      CTF::BLCstripID);
  MAKECODER(cc.chanInStrip,  CTF::BLCchanInStrip);
  MAKECODER(cc.tot,          CTF::BLCtot);
  MAKECODER(cc.pattMap,      CTF::BLCpattMap);
  // clang-format on
}

///________________________________
size_t CTFCoder::estimateCompressedSize(const CompressedInfos& cc)
{
  size_t sz = 0;
  // clang-format off
  // RS FIXME this is very crude estimate, instead, an empirical values should be used
#define VTP(vec) typename std::remove_reference<decltype(vec)>::type::value_type
#define ESTSIZE(vec, slot) mCoders[int(slot)] ?                         \
  rans::calculateMaxBufferSize(vec.size(), reinterpret_cast<const o2::rans::LiteralEncoder64<VTP(vec)>*>(mCoders[int(slot)].get())->getAlphabetRangeBits(), sizeof(VTP(vec)) ) : vec.size()*sizeof(VTP(vec))

  sz += ESTSIZE(cc.bcIncROF,     CTF::BLCbcIncROF);
  sz += ESTSIZE(cc.orbitIncROF,  CTF::BLCorbitIncROF);
  sz += ESTSIZE(cc.ndigROF,      CTF::BLCndigROF);
  sz += ESTSIZE(cc.ndiaROF,      CTF::BLCndiaROF);
  sz += ESTSIZE(cc.ndiaCrate,    CTF::BLCndiaCrate);
  sz += ESTSIZE(cc.timeFrameInc, CTF::BLCtimeFrameInc);
  sz += ESTSIZE(cc.timeTDCInc,   CTF::BLCtimeTDCInc);
  sz += ESTSIZE(cc.stripID,      CTF::BLCstripID);
  sz += ESTSIZE(cc.chanInStrip,  CTF::BLCchanInStrip);
  sz += ESTSIZE(cc.tot,          CTF::BLCtot);
  sz += ESTSIZE(cc.pattMap,      CTF::BLCpattMap);
  // clang-format on
  sz *= 2. / 3; // if needed, will be autoexpanded
  LOG(debug) << "Estimated output size is " << sz << " bytes";
  return sz;
}

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

///
/// @file   CalibPadGainTracks.h
/// @author Matthias Kleiner, matthias.kleiner@cern.ch
///

#ifndef AliceO2_TPC_CalibPadGainTracks_H
#define AliceO2_TPC_CalibPadGainTracks_H

// o2 includes
#include "DataFormatsTPC/TrackTPC.h"
#include "DataFormatsTPC/ClusterNative.h"
#include "TPCBase/CalDet.h"
#include "TPCBase/Mapper.h"
#include "TPCCalibration/CalibPadGainTracksBase.h"
#include "GPUO2Interface.h"
#include "DataFormatsTPC/CalibdEdxTrackTopologyPol.h"

#include <vector>
#include <gsl/span>
#include <tuple>

class TCanvas;

namespace o2
{
namespace tpc
{

/// \brief Gain calibration class
///
/// This class is used to produce pad wise gain calibration information with reconstructed tracks.
/// The idea is to use the self calibrated probe qMax/dEdx and store the information for each pad in an histogram.
/// The dEdx can be used from the track itself or from some bethe-bloch parametrization.
/// Using the dEdx information from the bethe bloch avoids biases in the dEdx of the track itself.
/// However the use of a bethe bloch parametrization is not yet implemented and shouldnt be used yet.
/// When enough thata is collected, the truncated mean of each histogram delivers the relative gain of each pad.
/// This method can be used to study the pad-by-pad gain as a function of time (i.e. performing this method n times with n different consecutive data samples)
///
/// origin: TPC
/// \author Matthias Kleiner, matthias.kleiner@cern.ch
///
///
/// how to use:
/// example:
/// CalibPadGainTracks cGain{};
/// cGain.init(20, 0, 3, 1, 1); // set the binning which will be used: 20 bins, minimum x=0, maximum x=10, use underflow and overflow bin
/// start loop over the data
/// cGain.setMembers(tpcTracks, tpcTrackClIdxVecInput, clusterIndex); // set the member variables: TrackTPC, TPCClRefElem, o2::tpc::ClusterNativeAccess
/// cGain.setMomentumRange(.1, 3);
/// cGain.processTracks();
/// after looping of the data (filling the histograms) is done
/// cGain.fillgainMap(); // fill the gainmap with the truncated mean from each histogram
/// cGain.dumpGainMap(); // write the gainmap to file
///
/// see also: extractGainMap.C macro

class CalibPadGainTracks : public CalibPadGainTracksBase
{

 public:
  /// mode of normalizing qmax
  enum DEdxType : unsigned char {
    dedxTrack,    ///< normalize qMax using the truncated mean from the track
    dedxTracking, ///< normalize qMax using the dEdx which was calculated during the tracking
    dedxBB        ///< normalize qMax by evaluating a Bethe Bloch fit. THIS is yet not implemented and shouldnt be used.
  };

  enum DEdxRegion : unsigned char {
    chamber, ///< use the dE/dx from IROC and OROC
    stack,   ///< use the dE/dx from IROC, OROC1, OROC2, OROC3
    sector   ///< use the dE/dx from the whole sector
  };

  /// default constructor
  /// \param initCalPad initialisation of the calpad for the gain map (if the gainmap is not extracted it can be false to save some memory)
  CalibPadGainTracks(const bool initCalPad = true) : CalibPadGainTracksBase(initCalPad) { reserveMemory(); }

  /// default destructor
  ~CalibPadGainTracks() = default;

  /// processes input tracks and filling the histograms with self calibrated probe qMax/dEdx
  void processTracks();

  /// set the member variables
  /// \param vTPCTracksArrayInp vector of tpc tracks
  /// \param tpcTrackClIdxVecInput set the TPCClRefElem member variable
  /// \param clIndex set the ClusterNativeAccess member variable
  void setMembers(gsl::span<const o2::tpc::TrackTPC>* vTPCTracksArrayInp, gsl::span<const o2::tpc::TPCClRefElem>* tpcTrackClIdxVecInput, const o2::tpc::ClusterNativeAccess& clIndex);

  /// this function sets the mode of the class.
  /// e.g. mode=0 -> use the truncated mean from the track for normalizing the dedx
  ///      mode=1 -> use the value from the BB-fit for normalizing the dedx. NOT implemented yet
  void setMode(DEdxType iMode) { mMode = iMode; }

  /// \param momMin minimum accpeted momentum of the tracks
  /// \param momMax maximum accpeted momentum of the tracks
  void setMomentumRange(const float momMin, const float momMax);

  /// \param eta maximum accpeted eta of the tracks
  void setMaxEta(const float eta) { mEtaMax = eta; }

  /// \param nCl minimum number of clusters required of the tracks
  void setMinNClusters(const int nCl) { mMinClusters = nCl; }

  /// \param field magnetic field in kG, used for track propagation
  void setField(const float field) { mField = field; }

  /// setting a gain map from a file
  /// \param inpFile input file containing some caldet
  /// \param mapName name of the caldet
  void setRefGainMap(const char* inpFile, const char* mapName);

  /// setting a gain map from a file
  void setRefGainMap(const CalPad& gainmap) { mGainMapRef = std::make_unique<CalPad>(gainmap); }

  /// set how the dedx is calculated which is used for normalizing the cluster charge
  void setdEdxRegion(const DEdxRegion dedx);

  /// \return returns minimum momentum of accepted tracks
  float getMomMin() const { return mMomMin; }

  /// \return returns maximum momentum of accepted tracks
  float getMomMax() const { return mMomMax; }

  /// \return returns maximum eta of accepted tracks
  float getEtaMax() const { return mEtaMax; }

  /// \return returns minimum number of clusters required of the tracks
  float getMinNClusters() const { return mMinClusters; }

  /// \return returns magnetic field which is used for propagation of track parameters
  float getField() const { return mField; };

  /// dump object to disc
  /// \param outFileName name of the output file
  /// \param outName name of the object in the output file
  void dumpToFile(const char* outFileName = "calPadGainTracks.root", const char* outName = "calPadGain") const;

  /// loading the track topology correction from a file
  /// \param fileName name of the file containing the object
  void loadPolTopologyCorrectionFromFile(std::string_view fileName);

 private:
  gsl::span<const TrackTPC>* mTracks{nullptr};                                        ///<! vector containing the tpc tracks which will be processed. Cant be const due to the propagate function
  gsl::span<const TPCClRefElem>* mTPCTrackClIdxVecInput{nullptr};                     ///<! input vector with TPC tracks cluster indicies
  const o2::tpc::ClusterNativeAccess* mClusterIndex{nullptr};                         ///<! needed to access clusternative with tpctracks
  DEdxType mMode = dedxTrack;                                                         ///< normalization type: type=DedxTrack use truncated mean, type=DedxBB use value from BB fit
  DEdxRegion mDedxRegion = stack;                                                     ///<  using the dE/dx per chamber, stack or per sector
  float mField{-5};                                                                   ///< Magnetic field in kG, used for track propagation
  float mMomMin{0.1f};                                                                ///< minimum momentum which is required by tracks
  float mMomMax{5.f};                                                                 ///< maximum momentum which is required by tracks
  float mEtaMax{1.f};                                                                 ///< maximum accpeted eta of tracks
  int mMinClusters{50};                                                               ///< minimum number of clusters the tracks require
  std::vector<std::vector<float>> mDEdxBuffer{};                                      ///<! memory for dE/dx
  std::vector<std::tuple<unsigned char, unsigned char, unsigned char, float>> mClTrk; ///<! memory for cluster informations
  std::unique_ptr<CalPad> mGainMapRef;                                                ///<! static Gain map object used for correcting the cluster charge
  std::unique_ptr<CalibdEdxTrackTopologyPol> mCalibTrackTopologyPol;                  ///< calibration container for the cluster charge

  /// calculate truncated mean for track
  /// \param track input track which will be processed
  void processTrack(TrackTPC track);

  /// get the index (padnumber in ROC) for given pad which is needed for the filling of the CalDet object
  /// \param padSub pad subset type
  /// \param padSubsetNumber index of the pad subset
  /// \param row corresponding pad row
  /// \param pad pad in row
  static int getIndex(o2::tpc::PadSubset padSub, int padSubsetNumber, const int row, const int pad) { return Mapper::instance().getPadNumber(padSub, padSubsetNumber, row, pad); }

  float getTrackTopologyCorrection(const o2::tpc::TrackTPC& track, const unsigned int region) const;

  float getTrackTopologyCorrectionPol(const o2::tpc::TrackTPC& track, const o2::tpc::ClusterNative& cl, const unsigned int region) const;

  /// get the truncated mean for input vector and the truncation range low*nCl<nCl<high*nCl
  /// \param vCharge vector containing all qmax values of the track
  /// \param low lower cluster cut of  0.05*nCluster
  /// \param high higher cluster cut of  0.6*nCluster
  std::vector<float> getTruncMean(std::vector<std::vector<float>>& vCharge, float low = 0.05f, float high = 0.6f) const;

  /// reserve memory for members
  void reserveMemory();

  void resizedEdxBuffer();

  int getdEdxBufferIndex(const int region) const;
};

} // namespace tpc
} // namespace o2

#endif

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

/// \file IDCFactorization.h
/// \brief class for aggregating IDCs for the full TPC (all sectors) and factorization of aggregated IDCs
///
/// \author Matthias Kleiner <mkleiner@ikf.uni-frankfurt.de>
/// \date Apr 30, 2021

#ifndef ALICEO2_IDCFACTORIZATION_H_
#define ALICEO2_IDCFACTORIZATION_H_

#include <vector>
#include "Rtypes.h"
#include "TPCBase/Mapper.h"
#include "TPCCalibration/IDCContainer.h"
#include "TPCCalibration/IDCGroupHelperSector.h"
#include "DataFormatsTPC/Defs.h"

#if (defined(WITH_OPENMP) || defined(_OPENMP)) && !defined(__CLING__)
#include <omp.h>
#endif

namespace o2::tpc
{

template <class T>
class CalDet;

class IDCFactorization : public IDCGroupHelperSector
{
 public:
  /// constructor
  /// \param groupPads number of pads in pad direction which will be grouped for all regions
  /// \param groupRows number of pads in row direction which will be grouped for all regions
  /// \param groupLastRowsThreshold minimum number of pads in row direction for the last group in row direction for all regions
  /// \param groupLastPadsThreshold minimum number of pads in pad direction for the last group in pad direction for all regions
  /// \param timeFrames number of time frames which will be stored
  /// \param timeframesDeltaIDC number of time frames stored for each DeltaIDC object
  IDCFactorization(const std::array<unsigned char, Mapper::NREGIONS>& groupPads, const std::array<unsigned char, Mapper::NREGIONS>& groupRows, const std::array<unsigned char, Mapper::NREGIONS>& groupLastRowsThreshold, const std::array<unsigned char, Mapper::NREGIONS>& groupLastPadsThreshold, const unsigned int groupNotnPadsSectorEdges, const unsigned int timeFrames, const unsigned int timeframesDeltaIDC, const std::vector<uint32_t>& crus);

  /// default constructor for ROOT I/O
  IDCFactorization() = default;

  /// calculate I_0(r,\phi) = <I(r,\phi,t)>_t
  /// calculate I_1(t) = <I(r,\phi,t) / I_0(r,\phi)>_{r,\phi}
  /// calculate \Delta I(r,\phi,t) = I(r,\phi,t) / ( I_0(r,\phi) * I_1(t) )
  /// \param norm normalize IDCs to pad size
  void factorizeIDCs(const bool norm);

  /// \return returns the stored grouped and integrated IDC
  /// \param sector sector
  /// \param region region
  /// \param grow row in the region of the grouped IDCs
  /// \param gpad pad number of the grouped IDCs
  /// \param integrationInterval integration interval
  float getIDCValGrouped(const unsigned int sector, const unsigned int region, const unsigned int grow, unsigned int gpad, unsigned int integrationInterval) const { return mIDCs[sector * Mapper::NREGIONS + region][integrationInterval][mOffsRow[region][grow] + gpad]; }

  /// \return returns the stored value for local ungrouped pad row and ungrouped pad
  /// \param sector sector
  /// \param region region
  /// \param urow row of the ungrouped IDCs
  /// \param upad pad number of the ungrouped IDCs
  /// \param integrationInterval integration interval
  float getIDCValUngrouped(const unsigned int sector, const unsigned int region, unsigned int urow, unsigned int upad, unsigned int integrationInterval) const;

  /// \return returns the stored IDC0 value for local ungrouped pad row and ungrouped pad
  /// \param sector sector
  /// \param region region
  /// \param urow row of the ungrouped IDCs
  /// \param upad pad number of the ungrouped IDCs
  float getIDCZeroVal(const unsigned int sector, const unsigned int region, unsigned int urow, unsigned int upad) const { return mIDCZero.getValueIDCZero(Sector(sector).side(), getIndexUngrouped(sector, region, urow, upad, 0)); }

  /// \return returns the stored DeltaIDC value for local ungrouped pad row and ungrouped pad
  /// \param sector sector
  /// \param region region
  /// \param urow row of the ungrouped IDCs
  /// \param upad pad number of the ungrouped IDCs
  /// \param chunk chunk of the Delta IDC (can be obtained with getLocalIntegrationInterval())
  /// \param localintegrationInterval local integration interval for chunk (can be obtained with getLocalIntegrationInterval())
  float getIDCDeltaVal(const unsigned int sector, const unsigned int region, unsigned int urow, unsigned int upad, unsigned int chunk, unsigned int localintegrationInterval) const { return mIDCDelta[chunk].getValue(Sector(sector).side(), getIndexUngrouped(sector, region, urow, upad, localintegrationInterval)); }

  /// \return returns index of integration interval in the chunk from global integration interval
  /// \param region TPC region
  /// \param integrationInterval integration interval
  /// \param chunk which will be set in the function
  /// \param localintegrationInterval local integration interval for chunk which will be set in the function
  void getLocalIntegrationInterval(const unsigned int region, const unsigned int integrationInterval, unsigned int& chunk, unsigned int& localintegrationInterval) const;

  /// \return returns number of timeframes for which the IDCs are stored
  unsigned int getNTimeframes() const { return mTimeFrames; }

  /// \return returns the number of stored integration intervals for given Delta IDC chunk
  /// \param chunk chunk of Delta IDC
  unsigned long getNIntegrationIntervals(const unsigned int chunk, const int cru) const;

  /// \return returns the total number of stored integration intervals
  unsigned long getNIntegrationIntervals(const int cru) const;

  /// \return returns stored IDC0 I_0(r,\phi) = <I(r,\phi,t)>_t
  /// \param side TPC side
  const std::vector<float>& getIDCZero(const o2::tpc::Side side) const { return mIDCZero.mIDCZero[side]; }

  /// \return returns stored IDC1 I_1(t) = <I(r,\phi,t) / I_0(r,\phi)>_{r,\phi}
  /// \param side TPC side
  const std::vector<float>& getIDCOne(const o2::tpc::Side side) const { return mIDCOne.mIDCOne[side]; }

  /// \return returns stored IDCDelta \Delta I(r,\phi,t) = I(r,\phi,t) / ( I_0(r,\phi) * I_1(t) )
  /// \param side TPC side
  /// \param chunk chunk of Delta IDC
  const std::vector<float>& getIDCDeltaUncompressed(const o2::tpc::Side side, const unsigned int chunk) const { return mIDCDelta[chunk].getIDCDelta(side); }

  /// \return returns stored IDCDelta \Delta I(r,\phi,t) = I(r,\phi,t) / ( I_0(r,\phi) * I_1(t) )
  /// \param chunk chunk of Delta IDC
  const auto& getIDCDeltaUncompressed(const unsigned int chunk) const& { return mIDCDelta[chunk]; }

  /// \return returns returns stored IDCDelta \Delta I(r,\phi,t) = I(r,\phi,t) / ( I_0(r,\phi) * I_1(t) ) using move semantics
  auto getIDCDeltaUncompressed(const unsigned int chunk) && { return std::move(mIDCDelta[chunk]); }

  /// \return creates and returns medium compressed IDCDelta \Delta I(r,\phi,t) = I(r,\phi,t) / ( I_0(r,\phi) * I_1(t) )
  /// \param chunk chunk of Delta IDC
  auto getIDCDeltaMediumCompressed(const unsigned int chunk) const { return IDCDeltaCompressionHelper<unsigned short>::getCompressedIDCs(mIDCDelta[chunk]); }

  /// \return creates and returns high compressed IDCDelta \Delta I(r,\phi,t) = I(r,\phi,t) / ( I_0(r,\phi) * I_1(t) )
  /// \param chunk chunk of Delta IDC
  auto getIDCDeltaHighCompressed(const unsigned int chunk) const { return IDCDeltaCompressionHelper<unsigned char>::getCompressedIDCs(mIDCDelta[chunk]); }

  /// \return returns number of chunks for Delta IDCs
  unsigned int getNChunks() const { return mIDCDelta.size(); }

  /// \return returns struct containing IDC0
  const auto& getIDCZero() const { return mIDCZero; }

  /// \return returns struct containing IDC1
  const auto& getIDCOne() const& { return mIDCOne; }

  /// \return returns struct containing IDC1 using move semantics
  auto getIDCOne() && { return std::move(mIDCOne); }

  /// \return returns grouped IDCs
  const auto& getIDCs() const { return mIDCs; }

  // get number of TFs in which the DeltaIDCs are split/stored
  unsigned int getTimeFramesDeltaIDC() const { return mTimeFramesDeltaIDC; }

  /// \return returns the number of threads used for some of the calculations
  static int getNThreads() { return sNThreads; }

  /// set the IDC data
  /// \param idcs vector containing the IDCs
  /// \param cru CRU
  /// \param timeframe time frame of the IDCs
  void setIDCs(std::vector<float>&& idcs, const unsigned int cru, const unsigned int timeframe) { mIDCs[cru][timeframe] = std::move(idcs); }

  /// set the number of threads used for some of the calculations
  /// \param nThreads number of threads
  static void setNThreads(const int nThreads) { sNThreads = nThreads; }

  /// \param minIDCDeltaValue minimum IDC delta value for compressed IDC delta
  static void setMinCompressedIDCDelta(const float minIDCDeltaValue) { o2::conf::ConfigurableParam::setValue<float>("TPCIDCCompressionParam", "minIDCDeltaValue", minIDCDeltaValue); }

  /// \param maxIDCDeltaValue maximum IDC delta value for compressed IDC delta
  static void setMaxCompressedIDCDelta(const float maxIDCDeltaValue) { o2::conf::ConfigurableParam::setValue<float>("TPCIDCCompressionParam", "maxIDCDeltaValue", maxIDCDeltaValue); }

  /// draw IDCs for one sector for one integration interval
  /// \param sector sector which will be drawn
  /// \param integrationInterval which will be drawn
  /// \param filename name of the output file. If empty the canvas is drawn.
  void drawIDCsSector(const unsigned int sector, const unsigned int integrationInterval, const float minZ = 0, const float maxZ = -1, const std::string filename = "IDCsSector.pdf") const { drawIDCHelper(false, Sector(sector), integrationInterval, filename, minZ, maxZ); }

  /// draw IDC zero I_0(r,\phi) = <I(r,\phi,t)>_t
  /// \param sector sector which will be drawn
  /// \param filename name of the output file. If empty the canvas is drawn.
  void drawIDCZeroSector(const unsigned int sector, const float minZ = 0, const float maxZ = -1, const std::string filename = "IDCZeroSector.pdf") const { drawIDCZeroHelper(false, Sector(sector), filename, minZ, maxZ); }

  /// draw IDCDelta for one sector for one integration interval
  /// \param sector sector which will be drawn
  /// \param integrationInterval which will be drawn
  /// \param compression compression of Delta IDCs. (setMaxCompressedIDCDelta() should be called first in case of non standard compression parameter)
  /// \param filename name of the output file. If empty the canvas is drawn.
  void drawIDCDeltaSector(const unsigned int sector, const unsigned int integrationInterval, const float minZ = 0, const float maxZ = -1, const IDCDeltaCompression compression = IDCDeltaCompression::NO, const std::string filename = "IDCDeltaSector.pdf") const { drawIDCDeltaHelper(false, Sector(sector), integrationInterval, compression, filename, minZ, maxZ); }

  /// draw IDCs for one side for one integration interval
  /// \param side side which will be drawn
  /// \param integrationInterval which will be drawn
  /// \param filename name of the output file. If empty the canvas is drawn.
  void drawIDCsSide(const o2::tpc::Side side, const unsigned int integrationInterval, const float minZ = 0, const float maxZ = -1, const std::string filename = "IDCsSide.pdf") const { drawIDCHelper(true, side == Side::A ? Sector(0) : Sector(Sector::MAXSECTOR - 1), integrationInterval, filename, minZ, maxZ); }

  /// draw IDC zero I_0(r,\phi) = <I(r,\phi,t)>_t
  /// \param side side which will be drawn
  /// \param filename name of the output file. If empty the canvas is drawn.
  void drawIDCZeroSide(const o2::tpc::Side side, const float minZ = 0, const float maxZ = -1, const std::string filename = "IDCZeroSide.pdf") const { drawIDCZeroHelper(true, side == Side::A ? Sector(0) : Sector(Sector::MAXSECTOR - 1), filename, minZ, maxZ); }

  /// draw IDCDelta for one side for one integration interval
  /// \param side side which will be drawn
  /// \param integrationInterval which will be drawn
  /// \param compression compression of Delta IDCs. (setMaxCompressedIDCDelta() should be called first in case of non standard compression parameter)
  /// \param filename name of the output file. If empty the canvas is drawn.
  void drawIDCDeltaSide(const o2::tpc::Side side, const unsigned int integrationInterval, const float minZ = 0, const float maxZ = -1, const IDCDeltaCompression compression = IDCDeltaCompression::NO, const std::string filename = "IDCDeltaSide.pdf") const { drawIDCDeltaHelper(true, side == Side::A ? Sector(0) : Sector(Sector::MAXSECTOR - 1), integrationInterval, compression, filename, minZ, maxZ); }

  /// draw the status map for the flags (for debugging) for a sector
  /// \param sector sector which will be drawn
  /// \flag flag which will be drawn
  /// \param filename name of the output file. If empty the canvas is drawn.
  void drawPadStatusFlagsMapSector(const unsigned int sector, const PadFlags flag = PadFlags::flagSkip, const std::string filename = "PadStatusFlags_Sector.pdf") const { drawPadFlagMap(false, Sector(sector), filename, flag); }

  /// draw the status map for the flags (for debugging) for a full side
  /// \param side side which will be drawn
  /// \flag flag which will be drawn
  /// \param filename name of the output file. If empty the canvas is drawn.
  void drawPadStatusFlagsMapSide(const o2::tpc::Side side, const PadFlags flag = PadFlags::flagSkip, const std::string filename = "PadStatusFlags_Side.pdf") const { drawPadFlagMap(true, side == Side::A ? Sector(0) : Sector(Sector::MAXSECTOR - 1), filename, flag); }

  /// dump object to disc
  /// \param outFileName name of the output file
  /// \param outName name of the object in the output file
  void dumpToFile(const char* outFileName = "IDCFactorized.root", const char* outName = "IDCFactorized") const;

  /// \param integrationIntervals number of integration intervals which will be dumped to the tree (-1: all integration intervalls)
  /// \param outFileName name of the output file
  void dumpToTree(int integrationIntervals = -1, const char* outFileName = "IDCTree.root") const;

  /// \returns vector containing the number of integration intervals for each stored TF
  std::vector<unsigned int> getIntegrationIntervalsPerTF(const unsigned int region = 0) const;

  /// resetting aggregated IDCs
  void reset();

  /// setting a gain map from a file
  /// \param inpFile input file containing some caldet
  /// \param mapName name of the caldet
  void setGainMap(const char* inpFile, const char* mapName);

  /// setting a gain map from a file
  void setGainMap(const CalDet<float>& gainmap);

  /// setting a map containing status flags for each pad
  /// \param inpFile input file containing the object
  /// \param mapName name of the caldet
  void setPadFlagMap(const char* inpFile, const char* mapName);

  /// setting a map containing status flags for each pad
  void setPadFlagMap(const CalDet<PadFlags>& flagmap);

  /// writing the pad status map to file
  /// \param outFile output file name
  /// \param mapName output name of the object
  void dumpPadFlagMap(const char* outFile, const char* mapName);

  /// \return returns pointer to pad status map
  CalDet<PadFlags>* getPadStatusMapPtr() const { return mPadFlagsMap.get(); }

  /// \return returns unique_ptr to pad status map
  std::unique_ptr<CalDet<PadFlags>> getPadStatusMap() { return std::move(mPadFlagsMap); }

 private:
  const unsigned int mTimeFrames{};                                 ///< number of timeframes which are stored
  const unsigned int mTimeFramesDeltaIDC{};                         ///< number of timeframes of which Delta IDCs are stored
  std::array<std::vector<std::vector<float>>, CRU::MaxCRU> mIDCs{}; ///< grouped and integrated IDCs for the whole TPC. CRU -> time frame -> IDCs
  IDCZero mIDCZero{};                                               ///< I_0(r,\phi) = <I(r,\phi,t)>_t
  IDCOne mIDCOne{};                                                 ///< I_1(t) = <I(r,\phi,t) / I_0(r,\phi)>_{r,\phi}
  std::vector<IDCDelta<float>> mIDCDelta{};                         ///< uncompressed: chunk -> Delta IDC: \Delta I(r,\phi,t) = I(r,\phi,t) / ( I_0(r,\phi) * I_1(t) )
  inline static int sNThreads{1};                                   ///< number of threads which are used during the calculations
  std::unique_ptr<CalDet<float>> mGainMap;                          ///<! static Gain map object used for filling missing IDC_0 values
  std::unique_ptr<CalDet<PadFlags>> mPadFlagsMap;                   ///< status flag for each pad (i.e. if the pad is dead)
  bool mInputGrouped{false};                                        ///< flag which is set to true if the input IDCs are grouped (checked via the grouping parameters from the constructor)
  const std::vector<uint32_t> mCRUs{};                              ///< CRUs to process in this instance

  /// calculate I_0(r,\phi) = <I(r,\phi,t)>_t
  void calcIDCZero(const bool norm);

  /// fill I_0 values in case of dead pads,FECs etc.
  void fillIDCZeroDeadPads();

  /// create status map for pads which are dead or delivering extremly high values (static outliers will be mapped)
  void createStatusMap();

  /// calculate I_1(t) = <I(r,\phi,t) / I_0(r,\phi)>_{r,\phi}
  void calcIDCOne();

  /// calculate \Delta I(r,\phi,t) = I(r,\phi,t) / ( I_0(r,\phi) * I_1(t) )
  void calcIDCDelta();

  /// helper function for drawing IDCDelta
  void drawIDCDeltaHelper(const bool type, const Sector sector, const unsigned int integrationInterval, const IDCDeltaCompression compression, const std::string filename, const float minZ, const float maxZ) const;

  /// helper function for drawing IDCs
  void drawIDCHelper(const bool type, const Sector sector, const unsigned int integrationInterval, const std::string filename, const float minZ, const float maxZ) const;

  /// helper function for drawing IDCZero
  void drawIDCZeroHelper(const bool type, const Sector sector, const std::string filename, const float minZ, const float maxZ) const;

  /// get time frame and index of integrationInterval in the TF
  void getTF(const unsigned int region, unsigned int integrationInterval, unsigned int& timeFrame, unsigned int& interval) const;

  /// \returns chunk from timeframe
  unsigned int getChunk(const unsigned int timeframe) const;

  /// \return returns number of TFs for given chunk
  unsigned int getNTFsPerChunk(const unsigned int chunk) const;

  /// \return returns the median of a vector
  float getMedian(std::vector<float>& values);

  /// helper function for drawing
  void drawPadFlagMap(const bool type, const Sector sector, const std::string filename, const PadFlags flag) const;

  ClassDefNV(IDCFactorization, 1)
};

} // namespace o2::tpc

#endif

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

/// @file   GPUWorkflowSpec.cxx
/// @author Matthias Richter
/// @since  2018-04-18
/// @brief  Processor spec for running TPC CA tracking

#include "GPUWorkflow/GPUWorkflowSpec.h"
#include "Headers/DataHeader.h"
#include "Framework/WorkflowSpec.h" // o2::framework::mergeInputs
#include "Framework/DataRefUtils.h"
#include "Framework/DataSpecUtils.h"
#include "Framework/DeviceSpec.h"
#include "Framework/ControlService.h"
#include "Framework/ConfigParamRegistry.h"
#include "Framework/InputRecordWalker.h"
#include "Framework/SerializationMethods.h"
#include "Framework/Logger.h"
#include "Framework/CallbackService.h"
#include "Framework/CCDBParamSpec.h"
#include "DataFormatsTPC/TPCSectorHeader.h"
#include "DataFormatsTPC/ClusterNative.h"
#include "DataFormatsTPC/CompressedClusters.h"
#include "DataFormatsTPC/Helpers.h"
#include "DataFormatsTPC/ZeroSuppression.h"
#include "DataFormatsTPC/WorkflowHelper.h"
#include "TPCReconstruction/TPCTrackingDigitsPreCheck.h"
#include "TPCReconstruction/TPCFastTransformHelperO2.h"
#include "DataFormatsTPC/Digit.h"
#include "TPCFastTransform.h"
#include "DPLUtils/DPLRawParser.h"
#include "DPLUtils/DPLRawPageSequencer.h"
#include "DetectorsBase/MatLayerCylSet.h"
#include "DetectorsBase/Propagator.h"
#include "DetectorsBase/GeometryManager.h"
#include "DetectorsRaw/HBFUtils.h"
#include "CommonUtils/NameConf.h"
#include "TPCBase/RDHUtils.h"
#include "GPUO2InterfaceConfiguration.h"
#include "GPUO2InterfaceQA.h"
#include "GPUO2Interface.h"
#include "CalibdEdxContainer.h"
#include "TPCPadGainCalib.h"
#include "display/GPUDisplayInterface.h"
#include "DataFormatsParameters/GRPObject.h"
#include "TPCBase/Sector.h"
#include "TPCBase/Utils.h"
#include "TPCBase/CDBInterface.h"
#include "SimulationDataFormat/ConstMCTruthContainer.h"
#include "SimulationDataFormat/MCCompLabel.h"
#include "Algorithm/Parser.h"
#include "DataFormatsGlobalTracking/RecoContainer.h"
#include "DataFormatsTRD/RecoInputContainer.h"
#include "TRDBase/Geometry.h"
#include "TRDBase/GeometryFlat.h"
#include <filesystem>
#include <memory> // for make_shared
#include <vector>
#include <iomanip>
#include <stdexcept>
#include <regex>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <chrono>
#include "GPUReconstructionConvert.h"
#include "DetectorsRaw/RDHUtils.h"
#include <TStopwatch.h>
#include <TObjArray.h>
#include <TH1F.h>
#include <TH2F.h>
#include <TH1D.h>

using namespace o2::framework;
using namespace o2::header;
using namespace o2::gpu;
using namespace o2::base;
using namespace o2::dataformats;
using namespace o2::tpc;

namespace o2::gpu
{

using ClusterGroupParser = o2::algorithm::ForwardParser<ClusterGroupHeader>;
struct ProcessAttributes {
  std::unique_ptr<ClusterGroupParser> parser;
  std::unique_ptr<GPUO2Interface> tracker;
  std::unique_ptr<GPUDisplayFrontendInterface> displayFrontend;
  std::unique_ptr<TPCFastTransform> fastTransform;
  std::unique_ptr<TPCPadGainCalib> tpcPadGainCalib;
  std::unique_ptr<TPCPadGainCalib> tpcPadGainCalibBufferNew;
  std::unique_ptr<o2::tpc::CalibdEdxContainer> dEdxCalibContainer;
  std::unique_ptr<o2::tpc::CalibdEdxContainer> dEdxCalibContainerBufferNew;
  std::unique_ptr<o2::trd::GeometryFlat> trdGeometry;
  std::unique_ptr<GPUO2InterfaceConfiguration> config;
  int qaTaskMask = 0;
  std::unique_ptr<GPUO2InterfaceQA> qa;
  std::vector<int> clusterOutputIds;
  unsigned long outputBufferSize = 0;
  unsigned long tpcSectorMask = 0;
  int verbosity = 0;
  bool readyToQuit = false;
  bool allocateOutputOnTheFly = false;
  bool suppressOutput = false;
  bool updateGainMapCCDB = true;
  bool disableCalibUpdates = false;
  o2::gpu::GPUSettingsTF tfSettings;
};

/// initialize TPC options from command line
void initFunctionTPC(ProcessAttributes* processAttributes, const GPUSettingsO2& confParam);

/// storing new calib objects in buffer
void finaliseCCDBTPC(ProcessAttributes* processAttributes, gpuworkflow::Config const& specconfig, ConcreteDataMatcher& matcher, void* obj);

/// asking for newer calib objects
void fetchCalibsCCDBTPC(ProcessAttributes* processAttributes, gpuworkflow::Config const& specconfig, ProcessingContext& pc);

/// storing the new calib objects by overwritting the old calibs
void storeUpdatedCalibsTPCPtrs(ProcessAttributes* processAttributes);

DataProcessorSpec getGPURecoWorkflowSpec(gpuworkflow::CompletionPolicyData* policyData, gpuworkflow::Config const& specconfig, std::vector<int> const& tpcsectors, unsigned long tpcSectorMask, std::string processorName)
{
  if (specconfig.outputCAClusters && !specconfig.caClusterer && !specconfig.decompressTPC) {
    throw std::runtime_error("inconsistent configuration: cluster output is only possible if CA clusterer is activated");
  }

  static TStopwatch timer;

  constexpr static size_t NSectors = Sector::MAXSECTOR;
  constexpr static size_t NEndpoints = 20; // TODO: get from mapper?
  auto processAttributes = std::make_shared<ProcessAttributes>();
  processAttributes->tpcSectorMask = tpcSectorMask;

  auto initFunction = [processAttributes, specconfig](InitContext& ic) {
    processAttributes->config.reset(new GPUO2InterfaceConfiguration);
    GPUO2InterfaceConfiguration& config = *processAttributes->config.get();
    GPUSettingsO2 confParam;
    {
      auto& parser = processAttributes->parser;
      auto& tracker = processAttributes->tracker;
      parser = std::make_unique<ClusterGroupParser>();
      tracker = std::make_unique<GPUO2Interface>();

      // Create configuration object and fill settings
      const auto grp = o2::parameters::GRPObject::loadFrom();
      o2::base::GeometryManager::loadGeometry();
      o2::base::Propagator::initFieldFromGRP();
      if (!grp) {
        throw std::runtime_error("Failed to initialize run parameters from GRP");
      }
      config.configGRP.solenoidBz = 5.00668f * grp->getL3Current() / 30000.;
      config.configGRP.continuousMaxTimeBin = grp->isDetContinuousReadOut(o2::detectors::DetID::TPC) ? -1 : 0; // Number of timebins in timeframe if continuous, 0 otherwise
      processAttributes->tfSettings.hasNHBFPerTF = 1;
      processAttributes->tfSettings.nHBFPerTF = grp->getNHBFPerTF();
      processAttributes->tfSettings.hasRunStartOrbit = 1;
      processAttributes->tfSettings.runStartOrbit = grp->getFirstOrbit();
      processAttributes->tfSettings.hasSimStartOrbit = 1;
      auto& hbfu = o2::raw::HBFUtils::Instance();
      processAttributes->tfSettings.simStartOrbit = hbfu.getFirstIRofTF(o2::InteractionRecord(0, hbfu.orbitFirstSampled)).orbit;

      LOG(info) << "Initializing run paramerers from GRP bz=" << config.configGRP.solenoidBz << " cont=" << grp->isDetContinuousReadOut(o2::detectors::DetID::TPC);

      confParam = config.ReadConfigurableParam();
      processAttributes->allocateOutputOnTheFly = confParam.allocateOutputOnTheFly;
      processAttributes->outputBufferSize = confParam.outputBufferSize;
      processAttributes->suppressOutput = (confParam.dump == 2);
      processAttributes->disableCalibUpdates = confParam.disableCalibUpdates;
      config.configInterface.dumpEvents = confParam.dump;
      if (confParam.display) {
        processAttributes->displayFrontend.reset(GPUDisplayFrontendInterface::getFrontend(config.configDisplay.displayFrontend.c_str()));
        config.configProcessing.eventDisplay = processAttributes->displayFrontend.get();
        if (config.configProcessing.eventDisplay != nullptr) {
          LOG(info) << "Event display enabled";
        } else {
          throw std::runtime_error("GPU Event Display frontend could not be created!");
        }
      }

      if (config.configGRP.continuousMaxTimeBin == -1) {
        config.configGRP.continuousMaxTimeBin = (processAttributes->tfSettings.nHBFPerTF * o2::constants::lhc::LHCMaxBunches + 2 * o2::tpc::constants::LHCBCPERTIMEBIN - 2) / o2::tpc::constants::LHCBCPERTIMEBIN;
      }
      if (config.configProcessing.deviceNum == -2) {
        int myId = ic.services().get<const o2::framework::DeviceSpec>().inputTimesliceId;
        int idMax = ic.services().get<const o2::framework::DeviceSpec>().maxInputTimeslices;
        config.configProcessing.deviceNum = myId;
        LOG(info) << "GPU device number selected from pipeline id: " << myId << " / " << idMax;
      }
      if (config.configProcessing.debugLevel >= 3 && processAttributes->verbosity == 0) {
        processAttributes->verbosity = 1;
      }
      config.configProcessing.runMC = specconfig.processMC;
      if (specconfig.outputQA) {
        if (!specconfig.processMC && !config.configQA.clusterRejectionHistograms) {
          throw std::runtime_error("Need MC information to create QA plots");
        }
        if (!specconfig.processMC) {
          config.configQA.noMC = true;
        }
        config.configQA.shipToQC = true;
        if (!config.configProcessing.runQA) {
          config.configQA.enableLocalOutput = false;
          processAttributes->qaTaskMask = (specconfig.processMC ? 15 : 0) | (config.configQA.clusterRejectionHistograms ? 32 : 0);
          config.configProcessing.runQA = -processAttributes->qaTaskMask;
        }
      }
      config.configReconstruction.tpc.nWaysOuter = true;
      config.configInterface.outputToExternalBuffers = true;
      if (confParam.synchronousProcessing) {
        config.configReconstruction.useMatLUT = false;
      }

      // Configure the "GPU workflow" i.e. which steps we run on the GPU (or CPU)
      if (specconfig.outputTracks || specconfig.outputCompClusters || specconfig.outputCompClustersFlat) {
        config.configWorkflow.steps.set(GPUDataTypes::RecoStep::TPCConversion,
                                        GPUDataTypes::RecoStep::TPCSliceTracking,
                                        GPUDataTypes::RecoStep::TPCMerging);
        config.configWorkflow.outputs.set(GPUDataTypes::InOutType::TPCMergedTracks);
        config.configWorkflow.steps.setBits(GPUDataTypes::RecoStep::TPCdEdx, confParam.rundEdx == -1 ? !confParam.synchronousProcessing : confParam.rundEdx);
      }
      if (specconfig.outputCompClusters || specconfig.outputCompClustersFlat) {
        config.configWorkflow.steps.setBits(GPUDataTypes::RecoStep::TPCCompression, true);
        config.configWorkflow.outputs.setBits(GPUDataTypes::InOutType::TPCCompressedClusters, true);
      }
      config.configWorkflow.inputs.set(GPUDataTypes::InOutType::TPCClusters);
      if (specconfig.caClusterer) { // Override some settings if we have raw data as input
        config.configWorkflow.inputs.set(GPUDataTypes::InOutType::TPCRaw);
        config.configWorkflow.steps.setBits(GPUDataTypes::RecoStep::TPCClusterFinding, true);
        config.configWorkflow.outputs.setBits(GPUDataTypes::InOutType::TPCClusters, true);
      }
      if (specconfig.decompressTPC) {
        config.configWorkflow.steps.setBits(GPUDataTypes::RecoStep::TPCCompression, false);
        config.configWorkflow.steps.setBits(GPUDataTypes::RecoStep::TPCDecompression, true);
        config.configWorkflow.inputs.set(GPUDataTypes::InOutType::TPCCompressedClusters);
        config.configWorkflow.outputs.setBits(GPUDataTypes::InOutType::TPCClusters, true);
        config.configWorkflow.outputs.setBits(GPUDataTypes::InOutType::TPCCompressedClusters, false);
        if (processAttributes->tpcSectorMask != 0xFFFFFFFFF) {
          throw std::invalid_argument("Cannot run TPC decompression with a sector mask");
        }
      }
      if (specconfig.runTRDTracking) {
        config.configWorkflow.inputs.setBits(GPUDataTypes::InOutType::TRDTracklets, true);
        config.configWorkflow.steps.setBits(GPUDataTypes::RecoStep::TRDTracking, true);
      }
      if (specconfig.outputSharedClusterMap) {
        config.configProcessing.outputSharedClusterMap = true;
      }
      config.configProcessing.createO2Output = specconfig.outputTracks ? 2 : 0; // Skip GPU-formatted output if QA is not requested

      // Create and forward data objects for TPC transformation, material LUT, ...
      if (confParam.transformationFile.size()) {
        processAttributes->fastTransform = nullptr;
        LOG(info) << "Reading TPC transformation map from file " << confParam.transformationFile;
        config.configCalib.fastTransform = TPCFastTransform::loadFromFile(confParam.transformationFile.c_str());
      } else {
        processAttributes->fastTransform = std::move(TPCFastTransformHelperO2::instance()->create(0));
        config.configCalib.fastTransform = processAttributes->fastTransform.get();
      }
      if (config.configCalib.fastTransform == nullptr) {
        throw std::invalid_argument("GPU workflow: initialization of the TPC transformation failed");
      }

      if (confParam.matLUTFile.size()) {
        LOGP(info, "Loading matlut file {}", confParam.matLUTFile.c_str());
        config.configCalib.matLUT = o2::base::MatLayerCylSet::loadFromFile(confParam.matLUTFile.c_str());
        if (config.configCalib.matLUT == nullptr) {
          LOGF(fatal, "Error loading matlut file");
        }
      }

      // initialize TPC calib objects
      initFunctionTPC(processAttributes.get(), confParam);

      config.configCalib.o2Propagator = Propagator::Instance();

      if (specconfig.readTRDtracklets) {
        auto gm = o2::trd::Geometry::instance();
        gm->createPadPlaneArray();
        gm->createClusterMatrixArray();
        processAttributes->trdGeometry = std::make_unique<o2::trd::GeometryFlat>(*gm);
        config.configCalib.trdGeometry = processAttributes->trdGeometry.get();
      }

      if (confParam.printSettings) {
        config.PrintParam();
      }

      // Configuration is prepared, initialize the tracker.
      if (tracker->Initialize(config) != 0) {
        throw std::invalid_argument("GPU Reconstruction initialization failed");
      }
      if (specconfig.outputQA) {
        processAttributes->qa = std::make_unique<GPUO2InterfaceQA>(processAttributes->config.get());
      }
      timer.Stop();
      timer.Reset();
    }

    auto& callbacks = ic.services().get<CallbackService>();
    callbacks.set(CallbackService::Id::RegionInfoCallback, [&processAttributes, confParam](FairMQRegionInfo const& info) {
      if (info.size == 0) {
        return;
      }
      if (confParam.registerSelectedSegmentIds != -1 && info.managed && info.id != (unsigned int)confParam.registerSelectedSegmentIds) {
        return;
      }
      int fd = 0;
      if (confParam.mutexMemReg) {
        mode_t mask = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
        fd = open("/tmp/o2_gpu_memlock_mutex.lock", O_RDWR | O_CREAT | O_CLOEXEC, mask);
        if (fd == -1) {
          throw std::runtime_error("Error opening lock file");
        }
        fchmod(fd, mask);
        if (lockf(fd, F_LOCK, 0)) {
          throw std::runtime_error("Error locking file");
        }
      }
      auto& tracker = processAttributes->tracker;
      std::chrono::time_point<std::chrono::high_resolution_clock> start, end;
      if (confParam.benchmarkMemoryRegistration) {
        start = std::chrono::high_resolution_clock::now();
      }
      if (tracker->registerMemoryForGPU(info.ptr, info.size)) {
        throw std::runtime_error("Error registering memory for GPU");
      }
      if (confParam.benchmarkMemoryRegistration) {
        end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed_seconds = end - start;
        LOG(info) << "Memory registration time (0x" << info.ptr << ", " << info.size << " bytes): " << elapsed_seconds.count() << " s";
      }
      if (confParam.mutexMemReg) {
        if (lockf(fd, F_ULOCK, 0)) {
          throw std::runtime_error("Error unlocking file");
        }
        close(fd);
      }
    });

    // the callback to be set as hook at stop of processing for the framework
    auto printTiming = []() {
      LOGF(info, "TPC CATracker total timing: Cpu: %.3e Real: %.3e s in %d slots", timer.CpuTime(), timer.RealTime(), timer.Counter() - 1);
    };
    ic.services().get<CallbackService>().set(CallbackService::Id::Stop, printTiming);

    auto finaliseCCDB = [processAttributes, specconfig](ConcreteDataMatcher& matcher, void* obj) {
      // updating TPC calibration objects
      finaliseCCDBTPC(processAttributes.get(), specconfig, matcher, obj);
    };

    ic.services().get<CallbackService>().set(CallbackService::Id::CCDBDeserialised, finaliseCCDB);

    auto processingFct = [processAttributes, specconfig](ProcessingContext& pc) {
      if (processAttributes->readyToQuit) {
        return;
      }
      auto cput = timer.CpuTime();
      auto realt = timer.RealTime();
      timer.Start(false);
      auto& tracker = processAttributes->tracker;
      auto& verbosity = processAttributes->verbosity;
      std::vector<gsl::span<const char>> inputs;

      const CompressedClustersFlat* pCompClustersFlat = nullptr;
      size_t compClustersFlatDummyMemory[(sizeof(CompressedClustersFlat) + sizeof(size_t) - 1) / sizeof(size_t)];
      CompressedClustersFlat& compClustersFlatDummy = reinterpret_cast<CompressedClustersFlat&>(compClustersFlatDummyMemory);
      CompressedClusters compClustersDummy;
      o2::gpu::GPUTrackingInOutZS tpcZS;
      std::vector<const void*> tpcZSmetaPointers[GPUTrackingInOutZS::NSLICES][GPUTrackingInOutZS::NENDPOINTS];
      std::vector<unsigned int> tpcZSmetaSizes[GPUTrackingInOutZS::NSLICES][GPUTrackingInOutZS::NENDPOINTS];
      const void** tpcZSmetaPointers2[GPUTrackingInOutZS::NSLICES][GPUTrackingInOutZS::NENDPOINTS];
      const unsigned int* tpcZSmetaSizes2[GPUTrackingInOutZS::NSLICES][GPUTrackingInOutZS::NENDPOINTS];
      std::array<unsigned int, NEndpoints * NSectors> tpcZSonTheFlySizes;
      gsl::span<const ZeroSuppressedContainer8kb> inputZS;

      bool getWorkflowTPCInput_clusters = false, getWorkflowTPCInput_mc = false, getWorkflowTPCInput_digits = false;

      // unsigned int totalZSPages = 0;
      if (specconfig.processMC) {
        getWorkflowTPCInput_mc = true;
      }
      if (!specconfig.decompressTPC && !specconfig.caClusterer) {
        getWorkflowTPCInput_clusters = true;
      }
      if (!specconfig.decompressTPC && specconfig.caClusterer && ((!specconfig.zsOnTheFly || specconfig.processMC) && !specconfig.zsDecoder)) {
        getWorkflowTPCInput_digits = true;
      }

      if (specconfig.zsOnTheFly || specconfig.zsDecoder) {
        for (unsigned int i = 0; i < GPUTrackingInOutZS::NSLICES; i++) {
          for (unsigned int j = 0; j < GPUTrackingInOutZS::NENDPOINTS; j++) {
            tpcZSmetaPointers[i][j].clear();
            tpcZSmetaSizes[i][j].clear();
          }
        }
      }
      if (specconfig.zsOnTheFly) {
        tpcZSonTheFlySizes = {0};
        // tpcZSonTheFlySizes: #zs pages per endpoint:
        std::vector<InputSpec> filter = {{"check", ConcreteDataTypeMatcher{gDataOriginTPC, "ZSSIZES"}, Lifetime::Timeframe}};
        bool recv = false, recvsizes = false;
        for (auto const& ref : InputRecordWalker(pc.inputs(), filter)) {
          if (recvsizes) {
            throw std::runtime_error("Received multiple ZSSIZES data");
          }
          tpcZSonTheFlySizes = pc.inputs().get<std::array<unsigned int, NEndpoints * NSectors>>(ref);
          recvsizes = true;
        }
        // zs pages
        std::vector<InputSpec> filter2 = {{"check", ConcreteDataTypeMatcher{gDataOriginTPC, "TPCZS"}, Lifetime::Timeframe}};
        for (auto const& ref : InputRecordWalker(pc.inputs(), filter2)) {
          if (recv) {
            throw std::runtime_error("Received multiple TPCZS data");
          }
          inputZS = pc.inputs().get<gsl::span<ZeroSuppressedContainer8kb>>(ref);
          recv = true;
        }
        if (!recv || !recvsizes) {
          throw std::runtime_error("TPC ZS on the fly data not received");
        }

        unsigned int offset = 0;
        for (unsigned int i = 0; i < NSectors; i++) {
          unsigned int pageSector = 0;
          for (unsigned int j = 0; j < NEndpoints; j++) {
            pageSector += tpcZSonTheFlySizes[i * NEndpoints + j];
            offset += tpcZSonTheFlySizes[i * NEndpoints + j];
          }
          if (verbosity >= 1) {
            LOG(info) << "GOT ZS on the fly pages FOR SECTOR " << i << " ->  pages: " << pageSector;
          }
        }
      }
      if (specconfig.zsDecoder) {
        std::vector<InputSpec> filter = {{"check", ConcreteDataTypeMatcher{gDataOriginTPC, "RAWDATA"}, Lifetime::Timeframe}};
        auto isSameRdh = [](const char* left, const char* right) -> bool {
          return o2::raw::RDHUtils::getFEEID(left) == o2::raw::RDHUtils::getFEEID(right);
        };
        auto insertPages = [&tpcZSmetaPointers, &tpcZSmetaSizes](const char* ptr, size_t count) -> void {
          int rawcru = rdh_utils::getCRU(ptr);
          int rawendpoint = rdh_utils::getEndPoint(ptr);
          tpcZSmetaPointers[rawcru / 10][(rawcru % 10) * 2 + rawendpoint].emplace_back(ptr);
          tpcZSmetaSizes[rawcru / 10][(rawcru % 10) * 2 + rawendpoint].emplace_back(count);
        };
        // the sequencer processes all inputs matching the filter and finds sequences of consecutive
        // raw pages based on the matcher predicate, and calls the inserter for each sequence
        DPLRawPageSequencer(pc.inputs(), filter)(isSameRdh, insertPages);

        int totalCount = 0;
        for (unsigned int i = 0; i < GPUTrackingInOutZS::NSLICES; i++) {
          for (unsigned int j = 0; j < GPUTrackingInOutZS::NENDPOINTS; j++) {
            tpcZSmetaPointers2[i][j] = tpcZSmetaPointers[i][j].data();
            tpcZSmetaSizes2[i][j] = tpcZSmetaSizes[i][j].data();
            tpcZS.slice[i].zsPtr[j] = tpcZSmetaPointers2[i][j];
            tpcZS.slice[i].nZSPtr[j] = tpcZSmetaSizes2[i][j];
            tpcZS.slice[i].count[j] = tpcZSmetaPointers[i][j].size();
            totalCount += tpcZSmetaPointers[i][j].size();
          }
        }
      } else if (specconfig.decompressTPC) {
        if (specconfig.decompressTPCFromROOT) {
          compClustersDummy = *pc.inputs().get<CompressedClustersROOT*>("input");
          compClustersFlatDummy.setForward(&compClustersDummy);
          pCompClustersFlat = &compClustersFlatDummy;
        } else {
          pCompClustersFlat = pc.inputs().get<CompressedClustersFlat*>("input").get();
        }
      } else if (!specconfig.zsOnTheFly) {
        if (verbosity) {
          LOGF(info, "running tracking for sector(s) 0x%09x", processAttributes->tpcSectorMask);
        }
      }

      const auto& inputsClustersDigits = getWorkflowTPCInput(pc, verbosity, getWorkflowTPCInput_mc, getWorkflowTPCInput_clusters, processAttributes->tpcSectorMask, getWorkflowTPCInput_digits);
      GPUTrackingInOutPointers ptrs;

      o2::globaltracking::RecoContainer inputTracksTRD;
      decltype(o2::trd::getRecoInputContainer(pc, &ptrs, &inputTracksTRD)) trdInputContainer;
      if (specconfig.readTRDtracklets) {
        o2::globaltracking::DataRequest dataRequestTRD;
        dataRequestTRD.requestTracks(o2::dataformats::GlobalTrackID::getSourcesMask(o2::dataformats::GlobalTrackID::NONE), false);
        inputTracksTRD.collectData(pc, dataRequestTRD);
        trdInputContainer = std::move(o2::trd::getRecoInputContainer(pc, &ptrs, &inputTracksTRD));
      }

      void* ptrEp[NSectors * NEndpoints] = {};
      bool doInputDigits = false, doInputDigitsMC = false;
      if (specconfig.decompressTPC) {
        ptrs.tpcCompressedClusters = pCompClustersFlat;
      } else if (specconfig.zsOnTheFly) {
        const unsigned long long int* buffer = reinterpret_cast<const unsigned long long int*>(&inputZS[0]);
        o2::gpu::GPUReconstructionConvert::RunZSEncoderCreateMeta(buffer, tpcZSonTheFlySizes.data(), *&ptrEp, &tpcZS);
        ptrs.tpcZS = &tpcZS;
        doInputDigits = doInputDigitsMC = specconfig.processMC;
      } else if (specconfig.zsDecoder) {
        ptrs.tpcZS = &tpcZS;
        if (specconfig.processMC) {
          throw std::runtime_error("Cannot process MC information, none available");
        }
      } else if (specconfig.caClusterer) {
        doInputDigits = true;
        doInputDigitsMC = specconfig.processMC;
      } else {
        ptrs.clustersNative = &inputsClustersDigits->clusterIndex;
      }

      GPUTrackingInOutDigits tpcDigitsMap;
      GPUTPCDigitsMCInput tpcDigitsMapMC;
      if (doInputDigits) {
        ptrs.tpcPackedDigits = &tpcDigitsMap;
        if (doInputDigitsMC) {
          tpcDigitsMap.tpcDigitsMC = &tpcDigitsMapMC;
        }
        for (unsigned int i = 0; i < NSectors; i++) {
          tpcDigitsMap.tpcDigits[i] = inputsClustersDigits->inputDigits[i].data();
          tpcDigitsMap.nTPCDigits[i] = inputsClustersDigits->inputDigits[i].size();
          if (doInputDigitsMC) {
            tpcDigitsMapMC.v[i] = inputsClustersDigits->inputDigitsMCPtrs[i];
          }
        }
      }

      // a byte size resizable vector object, the DataAllocator returns reference to internal object
      // initialize optional pointer to the vector object
      TPCSectorHeader clusterOutputSectorHeader{0};
      if (processAttributes->clusterOutputIds.size() > 0) {
        clusterOutputSectorHeader.sectorBits = processAttributes->tpcSectorMask;
        // subspecs [0, NSectors - 1] are used to identify sector data, we use NSectors to indicate the full TPC
        clusterOutputSectorHeader.activeSectors = processAttributes->tpcSectorMask;
      }

      GPUInterfaceOutputs outputRegions;
      using outputDataType = char;
      using outputBufferUninitializedVector = std::decay_t<decltype(pc.outputs().make<DataAllocator::UninitializedVector<outputDataType>>(Output{"", "", 0}))>;
      using outputBufferType = std::pair<std::optional<std::reference_wrapper<outputBufferUninitializedVector>>, outputDataType*>;
      std::vector<outputBufferType> outputBuffers(GPUInterfaceOutputs::count(), {std::nullopt, nullptr});

      auto setOutputAllocator = [&specconfig, &outputBuffers, &outputRegions, &processAttributes, &pc, verbosity](const char* name, bool condition, GPUOutputControl& region, auto&& outputSpec, size_t offset = 0) {
        if (condition) {
          auto& buffer = outputBuffers[outputRegions.getIndex(region)];
          if (processAttributes->allocateOutputOnTheFly) {
            region.allocator = [name, &buffer, &pc, outputSpec = std::move(outputSpec), verbosity, offset](size_t size) -> void* {
              size += offset;
              if (verbosity) {
                LOG(info) << "ALLOCATING " << size << " bytes for " << std::get<DataOrigin>(outputSpec).template as<std::string>() << "/" << std::get<DataDescription>(outputSpec).template as<std::string>() << "/" << std::get<2>(outputSpec);
              }
              std::chrono::time_point<std::chrono::high_resolution_clock> start, end;
              if (verbosity) {
                start = std::chrono::high_resolution_clock::now();
              }
              buffer.first.emplace(pc.outputs().make<DataAllocator::UninitializedVector<outputDataType>>(std::make_from_tuple<Output>(outputSpec), size));
              if (verbosity) {
                end = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double> elapsed_seconds = end - start;
                LOG(info) << "Allocation time for " << name << " (" << size << " bytes)" << ": " << elapsed_seconds.count() << "s";
              }
              return (buffer.second = buffer.first->get().data()) + offset;
            };
          } else {
            buffer.first.emplace(pc.outputs().make<DataAllocator::UninitializedVector<outputDataType>>(std::make_from_tuple<Output>(outputSpec), processAttributes->outputBufferSize));
            region.ptrBase = (buffer.second = buffer.first->get().data()) + offset;
            region.size = buffer.first->get().size() - offset;
          }
        }
      };

      auto downSizeBuffer = [](outputBufferType& buffer, size_t size) {
        if (!buffer.first) {
          return;
        }
        if (buffer.first->get().size() < size) {
          throw std::runtime_error("Invalid buffer size requested");
        }
        buffer.first->get().resize(size);
        if (size && buffer.first->get().data() != buffer.second) {
          throw std::runtime_error("Inconsistent buffer address after downsize");
        }
      };

      /*auto downSizeBufferByName = [&outputBuffers, &outputRegions, &downSizeBuffer](GPUOutputControl& region, size_t size) {
        auto& buffer = outputBuffers[outputRegions.getIndex(region)];
        downSizeBuffer(buffer, size);
      };*/

      auto downSizeBufferToSpan = [&outputBuffers, &outputRegions, &downSizeBuffer](GPUOutputControl& region, auto span) {
        auto& buffer = outputBuffers[outputRegions.getIndex(region)];
        if (!buffer.first) {
          return;
        }
        if (span.size() && buffer.second != (char*)span.data()) {
          throw std::runtime_error("Buffer does not match span");
        }
        downSizeBuffer(buffer, span.size() * sizeof(*span.data()));
      };

      setOutputAllocator("COMPCLUSTERSFLAT", specconfig.outputCompClustersFlat, outputRegions.compressedClusters, std::make_tuple(gDataOriginTPC, (DataDescription) "COMPCLUSTERSFLAT", 0));
      setOutputAllocator("CLUSTERNATIVE", processAttributes->clusterOutputIds.size() > 0, outputRegions.clustersNative, std::make_tuple(gDataOriginTPC, specconfig.sendClustersPerSector ? (DataDescription) "CLUSTERNATIVETMP" : (DataDescription) "CLUSTERNATIVE", NSectors, Lifetime::Timeframe, clusterOutputSectorHeader), sizeof(ClusterCountIndex));
      setOutputAllocator("CLSHAREDMAP", specconfig.outputSharedClusterMap, outputRegions.sharedClusterMap, std::make_tuple(gDataOriginTPC, (DataDescription) "CLSHAREDMAP", 0));
      setOutputAllocator("TRACKS", specconfig.outputTracks, outputRegions.tpcTracksO2, std::make_tuple(gDataOriginTPC, (DataDescription) "TRACKS", 0));
      setOutputAllocator("CLUSREFS", specconfig.outputTracks, outputRegions.tpcTracksO2ClusRefs, std::make_tuple(gDataOriginTPC, (DataDescription) "CLUSREFS", 0));
      setOutputAllocator("TRACKSMCLBL", specconfig.outputTracks && specconfig.processMC, outputRegions.tpcTracksO2Labels, std::make_tuple(gDataOriginTPC, (DataDescription) "TRACKSMCLBL", 0));
      ClusterNativeHelper::ConstMCLabelContainerViewWithBuffer clustersMCBuffer;
      if (specconfig.processMC && specconfig.caClusterer) {
        outputRegions.clusterLabels.allocator = [&clustersMCBuffer](size_t size) -> void* { return &clustersMCBuffer; };
      }

      const auto* dh = o2::header::get<o2::header::DataHeader*>(pc.inputs().getFirstValid(true).header);
      processAttributes->tfSettings.tfStartOrbit = dh->firstTForbit;
      processAttributes->tfSettings.hasTfStartOrbit = 1;
      ptrs.settingsTF = &processAttributes->tfSettings;

      if (processAttributes->tpcSectorMask != 0xFFFFFFFFF) {
        // Clean out the unused sectors, such that if they were present by chance, they are not processed, and if the values are uninitialized, we should not crash
        for (unsigned int i = 0; i < NSectors; i++) {
          if (!(processAttributes->tpcSectorMask & (1ul << i))) {
            if (ptrs.tpcZS) {
              for (unsigned int j = 0; j < GPUTrackingInOutZS::NENDPOINTS; j++) {
                tpcZS.slice[i].zsPtr[j] = nullptr;
                tpcZS.slice[i].nZSPtr[j] = nullptr;
                tpcZS.slice[i].count[j] = 0;
              }
            }
          }
        }
      }

      if ((int)(ptrs.tpcZS != nullptr) + (int)(ptrs.tpcPackedDigits != nullptr && (ptrs.tpcZS == nullptr || ptrs.tpcPackedDigits->tpcDigitsMC == nullptr)) + (int)(ptrs.clustersNative != nullptr) + (int)(ptrs.tpcCompressedClusters != nullptr) != 1) {
        throw std::runtime_error("Invalid input for gpu tracking");
      }

      const auto& holdData = TPCTrackingDigitsPreCheck::runPrecheck(&ptrs, processAttributes->config.get());

      // check for updates of TPC calibration objects
      fetchCalibsCCDBTPC(processAttributes.get(), specconfig, pc);

      int retVal = tracker->RunTracking(&ptrs, &outputRegions);

      // setting TPC calibration objects
      storeUpdatedCalibsTPCPtrs(processAttributes.get());

      tracker->Clear(false);

      if (processAttributes->suppressOutput) {
        return;
      }
      bool createEmptyOutput = false;
      if (retVal != 0) {
        if (retVal == 3 && processAttributes->config->configProcessing.ignoreNonFatalGPUErrors) {
          LOG(error) << "GPU Reconstruction aborted with non fatal error code, ignoring";
          createEmptyOutput = true;
        } else {
          throw std::runtime_error("tracker returned error code " + std::to_string(retVal));
        }
      }

      std::unique_ptr<ClusterNativeAccess> tmpEmptyClNative;
      if (createEmptyOutput) {
        memset(&ptrs, 0, sizeof(ptrs));
        for (unsigned int i = 0; i < outputRegions.count(); i++) {
          if (outputBuffers[i].first) {
            size_t toSize = 0;
            if (i == outputRegions.getIndex(outputRegions.compressedClusters)) {
              toSize = sizeof(*ptrs.tpcCompressedClusters);
            } else if (i == outputRegions.getIndex(outputRegions.clustersNative)) {
              toSize = sizeof(ClusterCountIndex);
            }
            outputBuffers[i].first->get().resize(toSize);
            outputBuffers[i].second = outputBuffers[i].first->get().data();
            if (toSize) {
              memset(outputBuffers[i].second, 0, toSize);
            }
          }
        }
        tmpEmptyClNative = std::make_unique<ClusterNativeAccess>();
        memset(tmpEmptyClNative.get(), 0, sizeof(*tmpEmptyClNative));
        ptrs.clustersNative = tmpEmptyClNative.get();
        if (specconfig.processMC) {
          MCLabelContainer cont;
          cont.flatten_to(clustersMCBuffer.first);
          clustersMCBuffer.second = clustersMCBuffer.first;
          tmpEmptyClNative->clustersMCTruth = &clustersMCBuffer.second;
        }
      } else {
        gsl::span<const o2::tpc::TrackTPC> spanOutputTracks = {ptrs.outputTracksTPCO2, ptrs.nOutputTracksTPCO2};
        gsl::span<const uint32_t> spanOutputClusRefs = {ptrs.outputClusRefsTPCO2, ptrs.nOutputClusRefsTPCO2};
        gsl::span<const o2::MCCompLabel> spanOutputTracksMCTruth = {ptrs.outputTracksTPCO2MC, ptrs.outputTracksTPCO2MC ? ptrs.nOutputTracksTPCO2 : 0};
        if (!processAttributes->allocateOutputOnTheFly) {
          for (unsigned int i = 0; i < outputRegions.count(); i++) {
            if (outputRegions.asArray()[i].ptrBase) {
              if (outputRegions.asArray()[i].size == 1) {
                throw std::runtime_error("Preallocated buffer size exceeded");
              }
              outputRegions.asArray()[i].checkCurrent();
              downSizeBuffer(outputBuffers[i], (char*)outputRegions.asArray()[i].ptrCurrent - (char*)outputBuffers[i].second);
            }
          }
        }
        downSizeBufferToSpan(outputRegions.tpcTracksO2, spanOutputTracks);
        downSizeBufferToSpan(outputRegions.tpcTracksO2ClusRefs, spanOutputClusRefs);
        downSizeBufferToSpan(outputRegions.tpcTracksO2Labels, spanOutputTracksMCTruth);

        if (processAttributes->clusterOutputIds.size() > 0 && (void*)ptrs.clustersNative->clustersLinear != (void*)(outputBuffers[outputRegions.getIndex(outputRegions.clustersNative)].second + sizeof(ClusterCountIndex))) {
          throw std::runtime_error("cluster native output ptrs out of sync"); // sanity check
        }
      }

      LOG(info) << "found " << ptrs.nOutputTracksTPCO2 << " track(s)";

      if (specconfig.outputCompClusters) {
        CompressedClustersROOT compressedClusters = *ptrs.tpcCompressedClusters;
        pc.outputs().snapshot(Output{gDataOriginTPC, "COMPCLUSTERS", 0}, ROOTSerialized<CompressedClustersROOT const>(compressedClusters));
      }

      if (processAttributes->clusterOutputIds.size() > 0) {
        ClusterNativeAccess const& accessIndex = *ptrs.clustersNative;
        if (specconfig.sendClustersPerSector) {
          // Clusters are shipped by sector, we are copying into per-sector buffers (anyway only for ROOT output)
          for (unsigned int i = 0; i < NSectors; i++) {
            if (processAttributes->tpcSectorMask & (1ul << i)) {
              DataHeader::SubSpecificationType subspec = i;
              clusterOutputSectorHeader.sectorBits = (1ul << i);
              char* buffer = pc.outputs().make<char>({gDataOriginTPC, "CLUSTERNATIVE", subspec, Lifetime::Timeframe, {clusterOutputSectorHeader}}, accessIndex.nClustersSector[i] * sizeof(*accessIndex.clustersLinear) + sizeof(ClusterCountIndex)).data();
              ClusterCountIndex* outIndex = reinterpret_cast<ClusterCountIndex*>(buffer);
              memset(outIndex, 0, sizeof(*outIndex));
              for (int j = 0; j < o2::tpc::constants::MAXGLOBALPADROW; j++) {
                outIndex->nClusters[i][j] = accessIndex.nClusters[i][j];
              }
              memcpy(buffer + sizeof(*outIndex), accessIndex.clusters[i][0], accessIndex.nClustersSector[i] * sizeof(*accessIndex.clustersLinear));
              if (specconfig.processMC && accessIndex.clustersMCTruth) {
                MCLabelContainer cont;
                for (unsigned int j = 0; j < accessIndex.nClustersSector[i]; j++) {
                  const auto& labels = accessIndex.clustersMCTruth->getLabels(accessIndex.clusterOffset[i][0] + j);
                  for (const auto& label : labels) {
                    cont.addElement(j, label);
                  }
                }
                ConstMCLabelContainer contflat;
                cont.flatten_to(contflat);
                pc.outputs().snapshot({gDataOriginTPC, "CLNATIVEMCLBL", subspec, Lifetime::Timeframe, {clusterOutputSectorHeader}}, contflat);
              }
            }
          }
        } else {
          // Clusters are shipped as single message, fill ClusterCountIndex
          DataHeader::SubSpecificationType subspec = NSectors;
          ClusterCountIndex* outIndex = reinterpret_cast<ClusterCountIndex*>(outputBuffers[outputRegions.getIndex(outputRegions.clustersNative)].second);
          static_assert(sizeof(ClusterCountIndex) == sizeof(accessIndex.nClusters));
          memcpy(outIndex, &accessIndex.nClusters[0][0], sizeof(ClusterCountIndex));
          if (specconfig.processMC && specconfig.caClusterer && accessIndex.clustersMCTruth) {
            pc.outputs().snapshot({gDataOriginTPC, "CLNATIVEMCLBL", subspec, Lifetime::Timeframe, {clusterOutputSectorHeader}}, clustersMCBuffer.first);
          }
        }
      }
      if (specconfig.outputQA) {
        TObjArray out;
        auto getoutput = [createEmptyOutput](auto ptr) { return ptr && !createEmptyOutput ? *ptr : std::decay_t<decltype(*ptr)>(); };
        std::vector<TH1F> copy1 = getoutput(outputRegions.qa.hist1); // Internally, this will also be used as output, so we need a non-const copy
        std::vector<TH2F> copy2 = getoutput(outputRegions.qa.hist2);
        std::vector<TH1D> copy3 = getoutput(outputRegions.qa.hist3);
        processAttributes->qa->postprocessExternal(copy1, copy2, copy3, out, processAttributes->qaTaskMask ? processAttributes->qaTaskMask : -1);
        pc.outputs().snapshot({gDataOriginTPC, "TRACKINGQA", 0, Lifetime::Timeframe}, out);
        processAttributes->qa->cleanup();
      }
      timer.Stop();
      LOG(info) << "GPU Reoncstruction time for this TF " << timer.CpuTime() - cput << " s (cpu), " << timer.RealTime() - realt << " s (wall)";
    };

    return processingFct;
  };

  // FIXME: find out how to handle merge inputs in a simple and intuitive way
  // changing the binding name of the input in order to identify inputs by unique labels
  // in the processing. Think about how the processing can be made agnostic of input size,
  // e.g. by providing a span of inputs under a certain label
  auto createInputSpecs = [&tpcsectors, &specconfig, policyData]() {
    Inputs inputs;
    if (specconfig.outputTracks) {
      // loading calibration objects from the CCDB
      inputs.emplace_back("tpcgain", gDataOriginTPC, "PADGAINFULL", 0, Lifetime::Condition, ccdbParamSpec(CDBTypeMap.at(CDBType::CalPadGainFull)));
      inputs.emplace_back("tpcgainresidual", gDataOriginTPC, "PADGAINRESIDUAL", 0, Lifetime::Condition, ccdbParamSpec(CDBTypeMap.at(CDBType::CalPadGainResidual)));
      inputs.emplace_back("tpctimegain", gDataOriginTPC, "TIMEGAIN", 0, Lifetime::Condition, ccdbParamSpec(CDBTypeMap.at(CDBType::CalTimeGain)));
      inputs.emplace_back("tpctopologygain", gDataOriginTPC, "TOPOLOGYGAIN", 0, Lifetime::Condition, ccdbParamSpec(CDBTypeMap.at(CDBType::CalTopologyGain)));
      inputs.emplace_back("tpcthreshold", gDataOriginTPC, "PADTHRESHOLD", 0, Lifetime::Condition, ccdbParamSpec("TPC/Config/FEEPad"));
    }
    if (specconfig.decompressTPC) {
      inputs.emplace_back(InputSpec{"input", ConcreteDataTypeMatcher{gDataOriginTPC, specconfig.decompressTPCFromROOT ? o2::header::DataDescription("COMPCLUSTERS") : o2::header::DataDescription("COMPCLUSTERSFLAT")}, Lifetime::Timeframe});
    } else if (specconfig.caClusterer) {
      // if the output type are tracks, then the input spec for the gain map is already defined
      if (!specconfig.outputTracks) {
        inputs.emplace_back("tpcgain", gDataOriginTPC, "PADGAINFULL", 0, Lifetime::Condition, ccdbParamSpec(CDBTypeMap.at(CDBType::CalPadGainFull)));
      }

      // We accept digits and MC labels also if we run on ZS Raw data, since they are needed for MC label propagation
      if ((!specconfig.zsOnTheFly || specconfig.processMC) && !specconfig.zsDecoder) {
        inputs.emplace_back(InputSpec{"input", ConcreteDataTypeMatcher{gDataOriginTPC, "DIGITS"}, Lifetime::Timeframe});
        policyData->emplace_back(o2::framework::InputSpec{"digits", o2::framework::ConcreteDataTypeMatcher{"TPC", "DIGITS"}});
      }
    } else {
      inputs.emplace_back(InputSpec{"input", ConcreteDataTypeMatcher{gDataOriginTPC, "CLUSTERNATIVE"}, Lifetime::Timeframe});
      policyData->emplace_back(o2::framework::InputSpec{"clusters", o2::framework::ConcreteDataTypeMatcher{"TPC", "CLUSTERNATIVE"}});
    }
    if (specconfig.processMC) {
      if (specconfig.caClusterer) {
        if (!specconfig.zsDecoder) {
          inputs.emplace_back(InputSpec{"mclblin", ConcreteDataTypeMatcher{gDataOriginTPC, "DIGITSMCTR"}, Lifetime::Timeframe});
          policyData->emplace_back(o2::framework::InputSpec{"digitsmc", o2::framework::ConcreteDataTypeMatcher{"TPC", "DIGITSMCTR"}});
        }
      } else {
        inputs.emplace_back(InputSpec{"mclblin", ConcreteDataTypeMatcher{gDataOriginTPC, "CLNATIVEMCLBL"}, Lifetime::Timeframe});
        policyData->emplace_back(o2::framework::InputSpec{"clustersmc", o2::framework::ConcreteDataTypeMatcher{"TPC", "CLNATIVEMCLBL"}});
      }
    }

    if (specconfig.zsDecoder) {
      // All ZS raw data is published with subspec 0 by the o2-raw-file-reader-workflow and DataDistribution
      // creates subspec fom CRU and endpoint id, we create one single input route subscribing to all TPC/RAWDATA
      inputs.emplace_back(InputSpec{"zsraw", ConcreteDataTypeMatcher{"TPC", "RAWDATA"}, Lifetime::Optional});
      if (specconfig.askDISTSTF) {
        inputs.emplace_back("stdDist", "FLP", "DISTSUBTIMEFRAME", 0, Lifetime::Timeframe);
      }
    }
    if (specconfig.zsOnTheFly) {
      inputs.emplace_back(InputSpec{"zsinput", ConcreteDataTypeMatcher{"TPC", "TPCZS"}, Lifetime::Timeframe});
      inputs.emplace_back(InputSpec{"zsinputsizes", ConcreteDataTypeMatcher{"TPC", "ZSSIZES"}, Lifetime::Timeframe});
    }
    if (specconfig.readTRDtracklets) {
      inputs.emplace_back("trdctracklets", o2::header::gDataOriginTRD, "CTRACKLETS", 0, Lifetime::Timeframe);
      inputs.emplace_back("trdtracklets", o2::header::gDataOriginTRD, "TRACKLETS", 0, Lifetime::Timeframe);
      inputs.emplace_back("trdtriggerrec", o2::header::gDataOriginTRD, "TRKTRGRD", 0, Lifetime::Timeframe);
      inputs.emplace_back("trdtrigrecmask", o2::header::gDataOriginTRD, "TRIGRECMASK", 0, Lifetime::Timeframe);
    }
    return inputs;
  };

  auto createOutputSpecs = [&specconfig, &tpcsectors, &processAttributes]() {
    std::vector<OutputSpec> outputSpecs;
    if (specconfig.outputTracks) {
      outputSpecs.emplace_back(gDataOriginTPC, "TRACKS", 0, Lifetime::Timeframe);
      outputSpecs.emplace_back(gDataOriginTPC, "CLUSREFS", 0, Lifetime::Timeframe);
    }
    if (specconfig.processMC && specconfig.outputTracks) {
      outputSpecs.emplace_back(gDataOriginTPC, "TRACKSMCLBL", 0, Lifetime::Timeframe);
    }
    if (specconfig.outputCompClusters) {
      outputSpecs.emplace_back(gDataOriginTPC, "COMPCLUSTERS", 0, Lifetime::Timeframe);
    }
    if (specconfig.outputCompClustersFlat) {
      outputSpecs.emplace_back(gDataOriginTPC, "COMPCLUSTERSFLAT", 0, Lifetime::Timeframe);
    }
    if (specconfig.outputCAClusters) {
      for (auto const& sector : tpcsectors) {
        processAttributes->clusterOutputIds.emplace_back(sector);
      }
      outputSpecs.emplace_back(gDataOriginTPC, "CLUSTERNATIVE", specconfig.sendClustersPerSector ? 0 : NSectors, Lifetime::Timeframe);
      if (specconfig.sendClustersPerSector) {
        outputSpecs.emplace_back(gDataOriginTPC, "CLUSTERNATIVETMP", NSectors, Lifetime::Timeframe); // Dummy buffer the TPC tracker writes the inital linear clusters to
        for (const auto sector : tpcsectors) {
          outputSpecs.emplace_back(gDataOriginTPC, "CLUSTERNATIVE", sector, Lifetime::Timeframe);
        }
      } else {
        outputSpecs.emplace_back(gDataOriginTPC, "CLUSTERNATIVE", NSectors, Lifetime::Timeframe);
      }
      if (specconfig.processMC) {
        if (specconfig.sendClustersPerSector) {
          for (const auto sector : tpcsectors) {
            outputSpecs.emplace_back(gDataOriginTPC, "CLNATIVEMCLBL", sector, Lifetime::Timeframe);
          }
        } else {
          outputSpecs.emplace_back(gDataOriginTPC, "CLNATIVEMCLBL", NSectors, Lifetime::Timeframe);
        }
      }
    }
    if (specconfig.outputSharedClusterMap) {
      outputSpecs.emplace_back(gDataOriginTPC, "CLSHAREDMAP", 0, Lifetime::Timeframe);
    }
    if (specconfig.outputQA) {
      outputSpecs.emplace_back(gDataOriginTPC, "TRACKINGQA", 0, Lifetime::Timeframe);
    }
    return outputSpecs;
  };

  return DataProcessorSpec{processorName, // process id
                           {createInputSpecs()},
                           {createOutputSpecs()},
                           AlgorithmSpec(initFunction)};
}

void initFunctionTPC(ProcessAttributes* processAttributes, const GPUSettingsO2& confParam)
{
  processAttributes->dEdxCalibContainer.reset(new o2::tpc::CalibdEdxContainer());

  if (confParam.dEdxDisableTopologyPol) {
    LOGP(info, "Disabling loading of track topology correction using polynomials from CCDB");
    processAttributes->dEdxCalibContainer->disableCorrectionCCDB(o2::tpc::CalibsdEdx::CalTopologyPol);
  }

  if (confParam.dEdxDisableThresholdMap) {
    LOGP(info, "Disabling loading of threshold map from CCDB");
    processAttributes->dEdxCalibContainer->disableCorrectionCCDB(o2::tpc::CalibsdEdx::CalThresholdMap);
  }

  if (confParam.dEdxDisableGainMap) {
    LOGP(info, "Disabling loading of gain map from CCDB");
    processAttributes->dEdxCalibContainer->disableCorrectionCCDB(o2::tpc::CalibsdEdx::CalGainMap);
  }

  if (confParam.dEdxDisableResidualGainMap) {
    LOGP(info, "Disabling loading of residual gain map from CCDB");
    processAttributes->dEdxCalibContainer->disableCorrectionCCDB(o2::tpc::CalibsdEdx::CalResidualGainMap);
  }

  if (confParam.dEdxDisableResidualGain) {
    LOGP(info, "Disabling loading of residual gain calibration from CCDB");
    processAttributes->dEdxCalibContainer->disableCorrectionCCDB(o2::tpc::CalibsdEdx::CalTimeGain);
  }

  if (confParam.dEdxUseFullGainMap) {
    LOGP(info, "Using the full gain map for correcting the cluster charge during calculation of the dE/dx");
    processAttributes->dEdxCalibContainer->setUsageOfFullGainMap(true);
  }

  if (confParam.gainCalibDisableCCDB) {
    LOGP(info, "Disabling loading the TPC pad gain calibration from the CCDB");
    processAttributes->updateGainMapCCDB = false;
  }

  // load from file
  if (!confParam.dEdxPolTopologyCorrFile.empty() || !confParam.dEdxCorrFile.empty() || !confParam.dEdxSplineTopologyCorrFile.empty()) {
    if (!confParam.dEdxPolTopologyCorrFile.empty()) {
      LOGP(info, "Loading dE/dx polynomial track topology correction from file: {}", confParam.dEdxPolTopologyCorrFile);
      processAttributes->dEdxCalibContainer->loadPolTopologyCorrectionFromFile(confParam.dEdxPolTopologyCorrFile);

      LOGP(info, "Disabling loading of track topology correction using polynomials from CCDB as it was already loaded from input file");
      processAttributes->dEdxCalibContainer->disableCorrectionCCDB(o2::tpc::CalibsdEdx::CalTopologyPol);

      if (std::filesystem::exists(confParam.thresholdCalibFile)) {
        LOG(info) << "Loading tpc zero supression map from file " << confParam.thresholdCalibFile;
        const auto* thresholdMap = o2::tpc::utils::readCalPads(confParam.thresholdCalibFile, "ThresholdMap")[0];
        processAttributes->dEdxCalibContainer->setZeroSupresssionThreshold(*thresholdMap);

        LOGP(info, "Disabling loading of threshold map from CCDB as it was already loaded from input file");
        processAttributes->dEdxCalibContainer->disableCorrectionCCDB(o2::tpc::CalibsdEdx::CalThresholdMap);
      } else {
        if (not confParam.thresholdCalibFile.empty()) {
          LOG(warn) << "Couldn't find tpc zero supression file " << confParam.thresholdCalibFile << ". Not setting any zero supression.";
        }
        LOG(info) << "Setting default zero supression map";
        processAttributes->dEdxCalibContainer->setDefaultZeroSupresssionThreshold();
      }
    } else if (!confParam.dEdxSplineTopologyCorrFile.empty()) {
      LOGP(info, "Loading dE/dx spline track topology correction from file: {}", confParam.dEdxSplineTopologyCorrFile);
      processAttributes->dEdxCalibContainer->loadSplineTopologyCorrectionFromFile(confParam.dEdxSplineTopologyCorrFile);

      LOGP(info, "Disabling loading of track topology correction using polynomials from CCDB as splines were loaded from input file");
      processAttributes->dEdxCalibContainer->disableCorrectionCCDB(o2::tpc::CalibsdEdx::CalTopologyPol);
    }
    if (!confParam.dEdxCorrFile.empty()) {
      LOGP(info, "Loading dEdx correction from file: {}", confParam.dEdxCorrFile);
      processAttributes->dEdxCalibContainer->loadResidualCorrectionFromFile(confParam.dEdxCorrFile);

      LOGP(info, "Disabling loading of residual gain calibration from CCDB as it was already loaded from input file");
      processAttributes->dEdxCalibContainer->disableCorrectionCCDB(o2::tpc::CalibsdEdx::CalTimeGain);
    }
  }

  if (confParam.dEdxPolTopologyCorrFile.empty() && confParam.dEdxSplineTopologyCorrFile.empty()) {
    // setting default topology correction to allocate enough memory
    LOG(info) << "Setting default dE/dx polynomial track topology correction to allocate enough memory";
    processAttributes->dEdxCalibContainer->setDefaultPolTopologyCorrection();
  }

  GPUO2InterfaceConfiguration& config = *processAttributes->config.get();
  config.configCalib.dEdxCalibContainer = processAttributes->dEdxCalibContainer.get();

  if (std::filesystem::exists(confParam.gainCalibFile)) {
    LOG(info) << "Loading tpc gain correction from file " << confParam.gainCalibFile;
    const auto* gainMap = o2::tpc::utils::readCalPads(confParam.gainCalibFile, "GainMap")[0];
    processAttributes->tpcPadGainCalib = GPUO2Interface::getPadGainCalib(*gainMap);

    LOGP(info, "Disabling loading the TPC gain correction map from the CCDB as it was already loaded from input file");
    processAttributes->updateGainMapCCDB = false;
  } else {
    if (not confParam.gainCalibFile.empty()) {
      LOG(warn) << "Couldn't find tpc gain correction file " << confParam.gainCalibFile << ". Not applying any gain correction.";
    }
    processAttributes->tpcPadGainCalib = GPUO2Interface::getPadGainCalibDefault();
    processAttributes->tpcPadGainCalib->getGainCorrection(30, 5, 5);
  }
  config.configCalib.tpcPadGain = processAttributes->tpcPadGainCalib.get();
}

void finaliseCCDBTPC(ProcessAttributes* processAttributes, gpuworkflow::Config const& specconfig, ConcreteDataMatcher& matcher, void* obj)
{
  LOGP(info, "checking for newer object....");
  const CalibdEdxContainer* dEdxCalibContainer = processAttributes->dEdxCalibContainer.get();

  auto copyCalibsToBuffer = [processAttributes, dEdxCalibContainer]() {
    if (!(processAttributes->dEdxCalibContainerBufferNew)) {
      processAttributes->dEdxCalibContainerBufferNew = std::make_unique<o2::tpc::CalibdEdxContainer>();
      processAttributes->dEdxCalibContainerBufferNew->cloneFromObject(*dEdxCalibContainer, nullptr);
    }
  };

  if (matcher == ConcreteDataMatcher(gDataOriginTPC, "PADGAINFULL", 0)) {
    LOGP(info, "Updating gain map from CCDB");
    const auto* gainMap = static_cast<o2::tpc::CalDet<float>*>(obj);

    if (dEdxCalibContainer->isCorrectionCCDB(CalibsdEdx::CalGainMap) && specconfig.outputTracks) {
      copyCalibsToBuffer();
      const float minGain = 0;
      const float maxGain = 2;
      processAttributes->dEdxCalibContainerBufferNew.get()->setGainMap(*gainMap, minGain, maxGain);
    }

    if (processAttributes->updateGainMapCCDB && specconfig.caClusterer) {
      processAttributes->tpcPadGainCalibBufferNew = GPUO2Interface::getPadGainCalib(*gainMap);
    }

  } else if (matcher == ConcreteDataMatcher(gDataOriginTPC, "PADGAINRESIDUAL", 0)) {
    LOGP(info, "Updating residual gain map from CCDB");
    copyCalibsToBuffer();
    const auto* gainMapResidual = static_cast<std::unordered_map<string, o2::tpc::CalDet<float>>*>(obj);
    const float minResidualGain = 0.7f;
    const float maxResidualGain = 1.3f;
    processAttributes->dEdxCalibContainerBufferNew.get()->setGainMapResidual(gainMapResidual->at("GainMap"), minResidualGain, maxResidualGain);
  } else if (matcher == ConcreteDataMatcher(gDataOriginTPC, "PADTHRESHOLD", 0)) {
    LOGP(info, "Updating threshold map from CCDB");
    copyCalibsToBuffer();
    const auto* thresholdMap = static_cast<std::unordered_map<string, o2::tpc::CalDet<float>>*>(obj);
    processAttributes->dEdxCalibContainerBufferNew.get()->setZeroSupresssionThreshold(thresholdMap->at("ThresholdMap"));
  } else if (matcher == ConcreteDataMatcher(gDataOriginTPC, "TOPOLOGYGAIN", 0) && !(dEdxCalibContainer->isTopologyCorrectionSplinesSet())) {
    LOGP(info, "Updating Q topology correction from CCDB");
    copyCalibsToBuffer();
    const auto* topologyCorr = static_cast<o2::tpc::CalibdEdxTrackTopologyPolContainer*>(obj);
    CalibdEdxTrackTopologyPol calibTrackTopology;
    calibTrackTopology.setFromContainer(*topologyCorr);
    processAttributes->dEdxCalibContainerBufferNew->setPolTopologyCorrection(calibTrackTopology);
  } else if (matcher == ConcreteDataMatcher(gDataOriginTPC, "TIMEGAIN", 0)) {
    LOGP(info, "Updating residual gain correction from CCDB");
    copyCalibsToBuffer();
    const auto* residualCorr = static_cast<o2::tpc::CalibdEdxCorrection*>(obj);
    processAttributes->dEdxCalibContainerBufferNew->setResidualCorrection(*residualCorr);
  }
}

void fetchCalibsCCDBTPC(ProcessAttributes* processAttributes, gpuworkflow::Config const& specconfig, ProcessingContext& pc)
{
  // update calibrations for clustering and tracking
  if ((specconfig.outputTracks || specconfig.caClusterer) && !processAttributes->disableCalibUpdates) {
    const CalibdEdxContainer* dEdxCalibContainer = processAttributes->dEdxCalibContainer.get();

    // this calibration is defined for clustering and tracking
    if (dEdxCalibContainer->isCorrectionCCDB(CalibsdEdx::CalGainMap) || processAttributes->updateGainMapCCDB) {
      pc.inputs().get<o2::tpc::CalDet<float>*>("tpcgain");
    }

    // these calibrations are only defined for the tracking
    if (specconfig.outputTracks) {
      // update the calibration objects in case they changed in the CCDB
      if (dEdxCalibContainer->isCorrectionCCDB(CalibsdEdx::CalThresholdMap)) {
        pc.inputs().get<std::unordered_map<std::string, o2::tpc::CalDet<float>>*>("tpcthreshold");
      }

      if (dEdxCalibContainer->isCorrectionCCDB(CalibsdEdx::CalResidualGainMap)) {
        pc.inputs().get<std::unordered_map<std::string, o2::tpc::CalDet<float>>*>("tpcgainresidual");
      }

      if (dEdxCalibContainer->isCorrectionCCDB(CalibsdEdx::CalTopologyPol)) {
        pc.inputs().get<o2::tpc::CalibdEdxTrackTopologyPolContainer*>("tpctopologygain");
      }

      if (dEdxCalibContainer->isCorrectionCCDB(CalibsdEdx::CalTimeGain)) {
        pc.inputs().get<o2::tpc::CalibdEdxCorrection*>("tpctimegain");
      }
    }

    if (processAttributes->dEdxCalibContainerBufferNew || processAttributes->tpcPadGainCalibBufferNew) {
      // updating the calibration object
      GPUCalibObjectsConst newTopologyCalib;

      if (processAttributes->dEdxCalibContainerBufferNew) {
        newTopologyCalib.dEdxCalibContainer = processAttributes->dEdxCalibContainerBufferNew.get();
      }

      if (processAttributes->tpcPadGainCalibBufferNew) {
        newTopologyCalib.tpcPadGain = processAttributes->tpcPadGainCalibBufferNew.get();
      }

      auto& tracker = processAttributes->tracker;
      tracker->UpdateCalibration(newTopologyCalib);
    }
  }
}

void storeUpdatedCalibsTPCPtrs(ProcessAttributes* processAttributes)
{
  if (processAttributes->dEdxCalibContainerBufferNew) {
    processAttributes->dEdxCalibContainer = std::move(processAttributes->dEdxCalibContainerBufferNew);
  }

  if (processAttributes->tpcPadGainCalibBufferNew) {
    processAttributes->tpcPadGainCalib = std::move(processAttributes->tpcPadGainCalibBufferNew);
  }
}

} // namespace o2::gpu

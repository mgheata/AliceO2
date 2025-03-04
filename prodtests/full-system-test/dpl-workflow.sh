#!/bin/bash

# ---------------------------------------------------------------------------------------------------------------------
# Get this script's directory and load common settings (use zsh first (e.g. on Mac) and fallback on `readlink -f` if zsh is not there)
MYDIR="$(dirname $(realpath $0))"
source $MYDIR/setenv.sh

if [ -z $FILEWORKDIRRUN ]; then FILEWORKDIRRUN=$FILEWORKDIR; fi              # directory where to find the run-related files (grp, collision context)

# ---------------------------------------------------------------------------------------------------------------------
#Some additional settings used in this workflow
if [[ -z $OPTIMIZED_PARALLEL_ASYNC ]]; then OPTIMIZED_PARALLEL_ASYNC=0; fi     # Enable tuned process multiplicities for async processing on the EPN
if [[ -z $CTF_DIR ]];                  then CTF_DIR=$FILEWORKDIR; fi           # Directory where to store CTFs
if [[ -z $CTF_DICT_DIR ]];             then CTF_DICT_DIR=$FILEWORKDIR; fi      # Directory of CTF dictionaries
if [[ -z $CTF_METAFILES_DIR ]];        then CTF_METAFILES_DIR="/dev/null"; fi  # Directory where to store CTF files metada, /dev/null : skip their writing
if [[ -z $RECO_NUM_NODES_WORKFLOW ]];  then RECO_NUM_NODES_WORKFLOW=250; fi    # Number of EPNs running this workflow in parallel, to increase multiplicities if necessary, by default assume we are 1 out of 250 servers
if [[ -z $CTF_MINSIZE ]];              then CTF_MINSIZE="2000000000"; fi        # accumulate CTFs until file size reached
if [[ -z $CTF_MAX_PER_FILE ]];         then CTF_MAX_PER_FILE="10000"; fi       # but no more than given number of CTFs per file
if [[ -z $IS_SIMULATED_DATA ]];        then IS_SIMULATED_DATA=1; fi            # processing simulated data

if [[ $SYNCMODE == 1 ]]; then
  if [[ -z "${WORKFLOW_DETECTORS_MATCHING+x}" ]]; then export WORKFLOW_DETECTORS_MATCHING="ITSTPC,ITSTPCTRD,ITSTPCTOF,ITSTPCTRDTOF,PRIMVTX"; fi # Select matchings that are enabled in sync mode
else
  if [[ -z "${WORKFLOW_DETECTORS_MATCHING+x}" ]]; then export WORKFLOW_DETECTORS_MATCHING="ALL"; fi # All matching / vertexing enabled in async mode
fi

workflow_has_parameter CTF && export SAVECTF=1
workflow_has_parameter GPU && { export GPUTYPE=HIP; export NGPUS=4; }

[[ -z $ITSCLUSDICT ]] && ITSCLUSDICT="${FILEWORKDIR}/ITSdictionary.bin"
[[ -z $MFTCLUSDICT ]] && MFTCLUSDICT="${FILEWORKDIR}/MFTdictionary.bin"
[[ -z $ITS_NOISE ]] && ITS_NOISE="${FILEWORKDIR}"
[[ -z $MFT_NOISE ]] && MFT_NOISE="${FILEWORKDIR}"
[[ -z $ITS_STROBE ]] && ITS_STROBE="891"
[[ -z $MFT_STROBE ]] && MFT_STROBE="198"

MID_FEEID_MAP="$FILEWORKDIR/mid-feeId_mapper.txt"
NITSDECTHREADS=2
NMFTDECTHREADS=2
# FIXME: multithreading in the itsmft reconstruction does not work
#        on macOS.
if [[ $(uname) == "Darwin" ]]; then
    NITSDECTHREADS=1
    NMFTDECTHREADS=1
fi
CTF_DICT=${CTF_DICT_DIR}/ctf_dictionary.root

ITSMFT_FILES="ITSClustererParam.dictFilePath=$ITSCLUSDICT;MFTClustererParam.dictFilePath=$MFTCLUSDICT";

LIST_OF_ASYNC_RECO_STEPS="MID MCH MFT FDD FV0 ZDC"

DISABLE_DIGIT_ROOT_INPUT="--disable-root-input"
DISABLE_DIGIT_CLUSTER_INPUT="--clusters-from-upstream"

# ---------------------------------------------------------------------------------------------------------------------
# Set active reconstruction steps (defaults added according to SYNCMODE)

has_processing_step()
{
  [[ $WORKFLOW_EXTRA_PROCESSING_STEPS =~ (^|,)"$1"(,|$) ]]
}

for i in `echo $LIST_OF_GLORECO | sed "s/,/ /g"`; do
  has_processing_step MATCH_$i && add_comma_separated WORKFLOW_DETECTORS_MATCHING $i # Enable extra matchings requested via WORKFLOW_EXTRA_PROCESSING_STEPS
done
if [[ $SYNCMODE == 1 ]]; then # Add default steps for synchronous mode
  add_comma_separated WORKFLOW_EXTRA_PROCESSING_STEPS ENTROPY_ENCODER
else # Add default steps for async mode
  for i in $LIST_OF_ASYNC_RECO_STEPS; do
    has_detector_reco $i && add_comma_separated WORKFLOW_EXTRA_PROCESSING_STEPS ${i}_RECO
  done
fi

# ---------------------------------------------------------------------------------------------------------------------
# Set general arguments
ARGS_ALL="--session ${OVERRIDE_SESSION:-default} --severity $SEVERITY --shm-segment-id $NUMAID --shm-segment-size $SHMSIZE $ARGS_ALL_EXTRA --early-forward-policy noraw"
ARGS_ALL_CONFIG="NameConf.mDirGeom=$FILEWORKDIR;NameConf.mDirMatLUT=$FILEWORKDIR;NameConf.mDirCollContext=$FILEWORKDIRRUN;NameConf.mDirGRP=$FILEWORKDIRRUN;keyval.input_dir=$FILEWORKDIR;keyval.output_dir=/dev/null;$ALL_EXTRA_CONFIG"
if [[ $EPNSYNCMODE == 1 ]]; then
  ARGS_ALL+=" --infologger-severity $INFOLOGGER_SEVERITY"
  ARGS_ALL+=" --monitoring-backend influxdb-unix:///tmp/telegraf.sock --resources-monitoring 15"
  ARGS_ALL_CONFIG+="NameConf.mCCDBServer=$GEN_TOPO_EPN_CCDB_SERVER;"
elif [[ "0$ENABLE_METRICS" != "01" ]]; then
  ARGS_ALL+=" --monitoring-backend no-op://"
fi
( [[ $EXTINPUT == 1 ]] || [[ $NUMAGPUIDS != 0 ]] ) && ARGS_ALL+=" --no-cleanup"
( [[ $GPUTYPE != "CPU" ]] || [[ $OPTIMIZED_PARALLEL_ASYNC != 0 ]] ) && ARGS_ALL+=" --shm-mlock-segment-on-creation 1"
[[ $SHMTHROW == 0 ]] && ARGS_ALL+=" --shm-throw-bad-alloc 0"
[[ ! -z $SHM_MANAGER_SHMID ]] && ARGS_ALL+=" --shm-no-cleanup on --shmid $SHM_MANAGER_SHMID"
[[ $NORATELOG == 1 ]] && ARGS_ALL+=" --fairmq-rate-logging 0"
if [[ $EPNSYNCMODE == 1 ]] || type numactl >/dev/null 2>&1 && [[ `numactl -H | grep "node . size" | wc -l` -ge 2 ]]; then
  [[ $NUMAGPUIDS != 0 ]] && ARGS_ALL+=" --child-driver 'numactl --membind $NUMAID --cpunodebind $NUMAID'"
fi
[[ ! -z $TIMEFRAME_RATE_LIMIT ]] && [[ $TIMEFRAME_RATE_LIMIT != 0 ]] && ARGS_ALL+=" --timeframes-rate-limit $TIMEFRAME_RATE_LIMIT --timeframes-rate-limit-ipcid $NUMAID"


# ---------------------------------------------------------------------------------------------------------------------
# Set some individual workflow arguments depending on configuration
GPU_INPUT=zsraw
GPU_OUTPUT=tracks,clusters
GPU_CONFIG=
GPU_CONFIG_KEY=
TOF_CONFIG=
TOF_INPUT=raw
TOF_OUTPUT=clusters
ITS_CONFIG_KEY=
TRD_CONFIG=
TRD_CONFIG_KEY=
TRD_FILTER_CONFIG=
CPV_INPUT=raw
EVE_CONFIG=" --jsons-folder $EDJSONS_DIR"
MIDDEC_CONFIG=
EMCRAW2C_CONFIG=

if [[ -z $ALPIDE_ERR_DUMPS ]]; then
  [[ $EPNSYNCMODE == 1 ]] && ALPIDE_ERR_DUMPS="1" || ALPIDE_ERR_DUMPS="0"
fi


if [[ $SYNCMODE == 1 ]]; then
  if [[ $BEAMTYPE == "PbPb" ]]; then
    ITS_CONFIG_KEY+="fastMultConfig.cutMultClusLow=30;fastMultConfig.cutMultClusHigh=2000;fastMultConfig.cutMultVtxHigh=500;"
    [[ -z ${ITS_CONFIG+x} ]] && ITS_CONFIG=" --tracking-mode sync"
    [[ -z ${PVERTEXING_CONFIG_KEY+x} ]] && PVERTEXING_CONFIG_KEY+="pvertexer.maxChi2TZDebris=2000;"
  elif [[ $BEAMTYPE == "pp" ]]; then
    ITS_CONFIG_KEY+="fastMultConfig.cutMultClusLow=-1;fastMultConfig.cutMultClusHigh=-1;fastMultConfig.cutMultVtxHigh=-1;ITSVertexerParam.phiCut=0.5;ITSVertexerParam.clusterContributorsCut=3;ITSVertexerParam.tanLambdaCut=0.2"
    [[ -z ${ITS_CONFIG+x} ]] && ITS_CONFIG=" --tracking-mode sync"
    [[ -z ${PVERTEXING_CONFIG_KEY+x} ]] && PVERTEXING_CONFIG_KEY+="pvertexer.maxChi2TZDebris=10;"
  elif [[ $BEAMTYPE == "cosmic" ]]; then
    [[ -z ${ITS_CONFIG+x} ]] && ITS_CONFIG=" --tracking-mode cosmics"
  else
    [[ -z ${ITS_CONFIG+x} ]] && ITS_CONFIG=" --tracking-mode sync"
  fi
  GPU_CONFIG_KEY+="GPU_global.synchronousProcessing=1;GPU_proc.clearO2OutputFromGPU=1;"
  TRD_CONFIG_KEY+="GPU_proc.ompThreads=1;"
  has_detector ITS && TRD_FILTER_CONFIG+=" --filter-trigrec"
else
  if [[ $BEAMTYPE == "PbPb" ]]; then
    [[ -z ${ITS_CONFIG+x} ]] && ITS_CONFIG=" --tracking-mode async"
    [[ -z ${PVERTEXING_CONFIG_KEY+x} ]] && PVERTEXING_CONFIG_KEY+="pvertexer.maxChi2TZDebris=2000;"
  elif [[ $BEAMTYPE == "pp" ]]; then
    ITS_CONFIG_KEY+="ITSVertexerParam.phiCut=0.5;ITSVertexerParam.clusterContributorsCut=3;ITSVertexerParam.tanLambdaCut=0.2"
    [[ -z ${ITS_CONFIG+x} ]] && ITS_CONFIG=" --tracking-mode async"
    [[ -z ${PVERTEXING_CONFIG_KEY+x} ]] && PVERTEXING_CONFIG_KEY+="pvertexer.maxChi2TZDebris=10;"
  elif [[ $BEAMTYPE == "cosmic" ]]; then
    [[ -z ${ITS_CONFIG+x} ]] && ITS_CONFIG=" --tracking-mode cosmics"
  else
    [[ -z ${ITS_CONFIG+x} ]] && ITS_CONFIG=" --tracking-mode async"
  fi
fi

has_processing_step ENTROPY_ENCODER && has_detector_ctf TPC && GPU_OUTPUT+=",compressed-clusters-ctf"

if [[ -z $DISABLE_ROOT_OUTPUT ]]; then
  # enable only if root output is written, because it slows down the processing
  GPU_OUTPUT+=",send-clusters-per-sector"
fi

has_detector_flp_processing CPV && CPV_INPUT=digits
! has_detector_flp_processing TOF && TOF_CONFIG+=" --ignore-dist-stf"

if [[ $EPNSYNCMODE == 1 ]]; then
  EVE_CONFIG+=" --eve-dds-collection-index 0"
  ITSMFT_FILES+=";ITSClustererParam.noiseFilePath=$ITS_NOISE;MFTClustererParam.noiseFilePath=$MFT_NOISE;ITSAlpideParam.roFrameLengthInBC=$ITS_STROBE;MFTAlpideParam.roFrameLengthInBC=$MFT_STROBE;"
  MIDDEC_CONFIG+=" --feeId-config-file \"$MID_FEEID_MAP\""
  GPU_CONFIG_KEY+="GPU_proc.tpcIncreasedMinClustersPerRow=500000;GPU_proc.ignoreNonFatalGPUErrors=1;"
  # Options for decoding current TRD real raw data (not needed for data converted from MC)
  if [[ -z $TRD_DECODER_OPTIONS ]]; then TRD_DECODER_OPTIONS=" --tracklethcheader 2 "; fi
  if [[ $EXTINPUT == 1 ]] && [[ $GPUTYPE != "CPU" ]] && [[ -z "$GPU_NUM_MEM_REG_CALLBACKS" ]]; then GPU_NUM_MEM_REG_CALLBACKS=4; fi
fi

if [[ $GPUTYPE != "CPU" && $NUMAGPUIDS != 0 ]]; then
  GPU_CONFIG_KEY+="GPU_global.registerSelectedSegmentIds=$NUMAID;"
fi

if [[ $GPUTYPE == "HIP" ]]; then
  if [[ $NUMAID == 0 ]] || [[ $NUMAGPUIDS == 0 ]]; then
    export TIMESLICEOFFSET=0
  else
    export TIMESLICEOFFSET=$NGPUS
  fi
  if [[ -z $ROCR_VISIBLE_DEVICES || $ROCR_VISIBLE_DEVICES = "0,1,2,3,4,5,6,7" ]]; then
    GPU_CONFIG_KEY+="GPU_proc.deviceNum=0;"
    GPU_CONFIG+=" --environment \"ROCR_VISIBLE_DEVICES={timeslice${TIMESLICEOFFSET}}\""
  fi
  export HSA_NO_SCRATCH_RECLAIM=1
  #export HSA_TOOLS_LIB=/opt/rocm/lib/librocm-debug-agent.so.2
else
  GPU_CONFIG_KEY+="GPU_proc.deviceNum=-2;"
fi

if [[ ! -z $GPU_NUM_MEM_REG_CALLBACKS ]]; then
  GPU_CONFIG+=" --expected-region-callbacks $GPU_NUM_MEM_REG_CALLBACKS"
fi

if [[ $GPUTYPE != "CPU" ]]; then
  GPU_CONFIG_KEY+="GPU_proc.forceMemoryPoolSize=$GPUMEMSIZE;"
  if [[ $HOSTMEMSIZE == "0" ]]; then
    HOSTMEMSIZE=$(( 1 << 30 ))
  fi
fi

if [[ $HOSTMEMSIZE != "0" ]]; then
  GPU_CONFIG_KEY+="GPU_proc.forceHostMemoryPoolSize=$HOSTMEMSIZE;"
fi

if ! has_detector_reco TOF; then
  TOF_OUTPUT=digits
fi

[[ $IS_SIMULATED_DATA == "1" ]] && EMCRAW2C_CONFIG+=" --no-mergeHGLG"

# ---------------------------------------------------------------------------------------------------------------------
# Assemble matching sources
TRD_SOURCES=
TOF_SOURCES=
TRACK_SOURCES=
has_detectors_reco ITS TPC && has_detector_matching ITSTPC && add_comma_separated TRACK_SOURCES "ITS-TPC"
has_detectors_reco TPC TRD && has_detector_matching TPCTRD && { add_comma_separated TRD_SOURCES TPC; add_comma_separated TRACK_SOURCES "TPC-TRD"; }
has_detectors_reco ITS TPC TRD && has_detector_matching ITSTPCTRD && { add_comma_separated TRD_SOURCES ITS-TPC; add_comma_separated TRACK_SOURCES "ITS-TPC-TRD"; }
has_detectors_reco TPC TOF && has_detector_matching TPCTOF && { add_comma_separated TOF_SOURCES TPC; add_comma_separated TRACK_SOURCES "TPC-TOF"; }
has_detectors_reco ITS TPC TOF && has_detector_matching ITSTPCTOF && { add_comma_separated TOF_SOURCES ITS-TPC; add_comma_separated TRACK_SOURCES "ITS-TPC-TOF"; }
has_detectors_reco TPC TRD TOF && has_detector_matching TPCTRDTOF && { add_comma_separated TOF_SOURCES TPC-TRD; add_comma_separated TRACK_SOURCES "TPC-TRD-TOF"; }
has_detectors_reco ITS TPC TRD TOF && has_detector_matching ITSTPCTRDTOF && { add_comma_separated TOF_SOURCES ITS-TPC-TRD; add_comma_separated TRACK_SOURCES "ITS-TPC-TRD-TOF"; }
has_detectors_reco MFT MCH && has_detector_matching MFTMCH && add_comma_separated TRACK_SOURCES "MFT-MCH"
for det in `echo $LIST_OF_DETECTORS | sed "s/,/ /g"`; do
  if [[ $LIST_OF_ASYNC_RECO_STEPS =~ (^| )${det}( |$) ]]; then
    has_detector ${det} && has_processing_step ${det}_RECO && add_comma_separated TRACK_SOURCES "$det"
  else
    has_detector_reco $det && add_comma_separated TRACK_SOURCES "$det"
  fi
done
[[ -z $VERTEXING_SOURCES ]] && VERTEXING_SOURCES="$TRACK_SOURCES"
PVERTEX_CONFIG="--vertexing-sources $VERTEXING_SOURCES --vertex-track-matching-sources $VERTEXING_SOURCES"

# this option requires well calibrated timing beween different detectors, at the moment suppress it
#has_detector_reco FT0 && PVERTEX_CONFIG+=" --validate-with-ft0"

# ---------------------------------------------------------------------------------------------------------------------
# Process multiplicities

# Helper function to apply scaling factors for process type (RAW/CTF/REST) and detector, or override multiplicity set for individual process externally.
N_F_REST=$MULTIPLICITY_FACTOR_REST
N_F_RAW=$MULTIPLICITY_FACTOR_RAWDECODERS
N_F_CTF=$MULTIPLICITY_FACTOR_CTFENCODERS
get_N() # USAGE: get_N [processor-name] [DETECTOR_NAME] [RAW|CTF|REST] [threads, to be used for process scaling. 0 = do not scale this one process] [optional name [FOO] of variable "$N_[FOO]" with default, default = 1]
{
  local NAME_FACTOR="N_F_$3"
  local NAME_DET="MULTIPLICITY_FACTOR_DETECTOR_$2"
  local NAME_PROC="MULTIPLICITY_FACTOR_PROCESS_${1//-/_}"
  local NAME_DEFAULT="N_$5"
  local MULT=${!NAME_PROC:-$((${!NAME_FACTOR} * ${!NAME_DET:-1} * ${!NAME_DEFAULT:-1}))}
  if [[ "0$GEN_TOPO_AUTOSCALE_PROCESSES" == "01" && $4 != 0 ]]; then
    echo $1:\$\(\(\($MULT*\$AUTOSCALE_PROCESS_FACTOR/100\) \< 16 ? \($MULT*\$AUTOSCALE_PROCESS_FACTOR/100\) : 16\)\)
  else
    echo $1:$MULT
  fi
}

math_max()
{
  echo $(($1 > $2 ? $1 : $2))
}

N_TPCTRK=$NGPUS
if [[ $OPTIMIZED_PARALLEL_ASYNC != 0 ]]; then
  # Tuned multiplicities for async Pb-Pb processing
  if [[ $SYNCMODE == "1" ]]; then echo "Must not use OPTIMIZED_PARALLEL_ASYNC with GPU or SYNCMODE" 1>&2; exit 1; fi
  if [[ $NUMAGPUIDS != 0 ]]; then N_NUMAFACTOR=1; else N_NUMAFACTOR=2; fi
  GPU_CONFIG_KEY+="GPU_proc.ompThreads=6;"
  TRD_CONFIG_KEY+="GPU_proc.ompThreads=2;"
  if [[ $GPUTYPE == "CPU" ]]; then
    N_TPCENTDEC=$((2 * $N_NUMAFACTOR))
    N_MFTTRK=$((3 * $N_NUMAFACTOR))
    N_ITSTRK=$((3 * $N_NUMAFACTOR))
    N_TPCITS=$((2 * $N_NUMAFACTOR))
    N_MCHTRK=$((1 * $N_NUMAFACTOR))
    N_TOFMATCH=$((9 * $N_NUMAFACTOR))
    N_TPCTRK=$((6 * $N_NUMAFACTOR))
  else
    N_TPCENTDEC=$(math_max $((3 * $NGPUS * $OPTIMIZED_PARALLEL_ASYNC * $N_NUMAFACTOR / 4)) 1)
    N_MFTTRK=$(math_max $((6 * $NGPUS * $OPTIMIZED_PARALLEL_ASYNC * $N_NUMAFACTOR / 4)) 1)
    N_ITSTRK=$(math_max $((6 * $NGPUS * $OPTIMIZED_PARALLEL_ASYNC * $N_NUMAFACTOR / 4)) 1)
    N_TPCITS=$(math_max $((4 * $NGPUS * $OPTIMIZED_PARALLEL_ASYNC * $N_NUMAFACTOR / 4)) 1)
    N_MCHTRK=$(math_max $((2 * $NGPUS * $OPTIMIZED_PARALLEL_ASYNC * $N_NUMAFACTOR / 4)) 1)
    N_TOFMATCH=$(math_max $((20 * $NGPUS * $OPTIMIZED_PARALLEL_ASYNC * $N_NUMAFACTOR / 4)) 1)
  fi
elif [[ $EPNPIPELINES != 0 ]]; then
  # Tuned multiplicities for sync Pb-Pb processing
  N_TPCENT=$(math_max $((3 * $EPNPIPELINES * $NGPUS / 4)) 1)
  N_TPCITS=$(math_max $((3 * $EPNPIPELINES * $NGPUS / 4)) 1)
  N_ITSTRK=$(math_max $((2 * $EPNPIPELINES * $NGPUS / 4)) 1)
  N_ITSRAWDEC=$(math_max $((3 * $EPNPIPELINES * $NGPUS / 4)) 1)
  N_EMCREC=$(math_max $((3 * $EPNPIPELINES * $NGPUS / 4)) 1)
  N_TRDENT=$(math_max $((3 * $EPNPIPELINES * $NGPUS / 4)) 1)
  N_TRDTRK=$(math_max $((3 * $EPNPIPELINES * $NGPUS / 4)) 1)
  N_TPCRAWDEC=$(math_max $((12 * $EPNPIPELINES * $NGPUS / 4)) 1)
  if [[ $GPUTYPE == "CPU" ]]; then
    N_TPCTRK=8
    GPU_CONFIG_KEY+="GPU_proc.ompThreads=4;"
  fi
  # Scale some multiplicities with the number of nodes
  RECO_NUM_NODES_WORKFLOW_CMP=$((($RECO_NUM_NODES_WORKFLOW > 15 ? $RECO_NUM_NODES_WORKFLOW : 15) * ($NUMAGPUIDS != 0 ? 2 : 1))) # Limit the lower scaling factor, multiply by 2 if we have 2 NUMA domains
  N_ITSRAWDEC=$(math_max $((3 * 60 / $RECO_NUM_NODES_WORKFLOW_CMP)) ${N_ITSRAWDEC:-1}) # This means, if we have 60 EPN nodes, we need at least 3 ITS RAW decoders
  N_MFTRAWDEC=$(math_max $((3 * 60 / $RECO_NUM_NODES_WORKFLOW_CMP)) ${N_MFTRAWDEC:-1})
  N_ITSTRK=$(math_max $((1 * 200 / $RECO_NUM_NODES_WORKFLOW_CMP)) ${N_ITSTRK:-1})
  N_MFTTRK=$(math_max $((1 * 60 / $RECO_NUM_NODES_WORKFLOW_CMP)) ${N_MFTTRK:-1})
  N_CTPRAWDEC=$(math_max $((1 * 30 / $RECO_NUM_NODES_WORKFLOW_CMP)) ${N_CTPRAWDEC:-1})
  N_TRDRAWDEC=$(math_max $((3 * 60 / $RECO_NUM_NODES_WORKFLOW_CMP)) ${N_TRDRAWDEC:-1})
  N_GENERICRAWDEV=
fi

# ---------------------------------------------------------------------------------------------------------------------
# Helper to add binaries to workflow adding automatic and custom arguments
WORKFLOW= # Make sure we start with an empty workflow
[[ "0$GEN_TOPO_ONTHEFLY" == "01" ]] && WORKFLOW="echo '{}' | "

add_W() # Add binarry to workflow command USAGE: add_W [BINARY] [COMMAND_LINE_OPTIONS] [CONFIG_KEY_VALUES] [Add ARGS_ALL_CONFIG, optional, default = 1]
{
  local NAME_PROC_ARGS="ARGS_EXTRA_PROCESS_${1//-/_}"
  local NAME_PROC_CONFIG="CONFIG_EXTRA_PROCESS_${1//-/_}"
  local KEY_VALUES=
  [[ "0$4" != "00" ]] && KEY_VALUES+="$ARGS_ALL_CONFIG;"
  [[ ! -z "$3" ]] && KEY_VALUES+="$3;"
  [[ ! -z ${!NAME_PROC_CONFIG} ]] && KEY_VALUES+="${!NAME_PROC_CONFIG};"
  [[ ! -z "$KEY_VALUES" ]] && KEY_VALUES="--configKeyValues \"$KEY_VALUES\""
  WORKFLOW+="$1 $ARGS_ALL $2 ${!NAME_PROC_ARGS} $KEY_VALUES | "
}


# ---------------------------------------------------------------------------------------------------------------------
# Input workflow
if [[ $CTFINPUT == 1 ]]; then
  GPU_INPUT=compressed-clusters-ctf
  TOF_INPUT=digits
  CTFName=`ls -t $RAWINPUTDIR/o2_ctf_*.root 2> /dev/null | head -n1`
  [[ -z $CTFName && $WORKFLOWMODE == "print" ]] && CTFName='$CTFName'
  [[ ! -z $INPUT_FILE_LIST ]] && CTFName=$INPUT_FILE_LIST
  if [[ -z $CTFName && $WORKFLOWMODE != "print" ]]; then echo "No CTF file given!"; exit 1; fi
  if [[ $NTIMEFRAMES == -1 ]]; then NTIMEFRAMES_CMD= ; else NTIMEFRAMES_CMD="--max-tf $NTIMEFRAMES"; fi
  add_W o2-ctf-reader-workflow "--delay $TFDELAY --loop $TFLOOP $NTIMEFRAMES_CMD --ctf-input ${CTFName} ${INPUT_FILE_COPY_CMD+--copy-cmd} ${INPUT_FILE_COPY_CMD} --ctf-dict ${CTF_DICT} --onlyDet $WORKFLOW_DETECTORS --pipeline $(get_N tpc-entropy-decoder TPC REST 1 TPCENTDEC)"
elif [[ $RAWTFINPUT == 1 ]]; then
  TFName=`ls -t $RAWINPUTDIR/o2_*.tf 2> /dev/null | head -n1`
  [[ -z $TFName && $WORKFLOWMODE == "print" ]] && TFName='$TFName'
  [[ ! -z $INPUT_FILE_LIST ]] && TFName=$INPUT_FILE_LIST
  if [[ -z $TFName && $WORKFLOWMODE != "print" ]]; then echo "No raw file given!"; exit 1; fi
  if [[ $NTIMEFRAMES == -1 ]]; then NTIMEFRAMES_CMD= ; else NTIMEFRAMES_CMD="--max-tf $NTIMEFRAMES"; fi
  add_W o2-raw-tf-reader-workflow "--delay $TFDELAY --loop $TFLOOP $NTIMEFRAMES_CMD --input-data ${TFName} ${INPUT_FILE_COPY_CMD+--copy-cmd} ${INPUT_FILE_COPY_CMD} --onlyDet $WORKFLOW_DETECTORS"
elif [[ $EXTINPUT == 1 ]]; then
  PROXY_CHANNEL="name=readout-proxy,type=pull,method=connect,address=ipc://${UDS_PREFIX}${INRAWCHANNAME},transport=shmem,rateLogging=$EPNSYNCMODE"
  PROXY_INSPEC="dd:FLP/DISTSUBTIMEFRAME/0"
  PROXY_IN_N=0
  for i in `echo "$WORKFLOW_DETECTORS" | sed "s/,/ /g"`; do
    if has_detector_flp_processing $i; then
      case $i in
        TOF)
          PROXY_INTYPE="CRAWDATA";;
        FT0 | FV0 | FDD)
          PROXY_INTYPE="DIGITSBC/0 DIGITSCH/0";;
        PHS)
          PROXY_INTYPE="CELLS CELLTRIGREC";;
        CPV)
          PROXY_INTYPE="DIGITS/0 DIGITTRIGREC/0 RAWHWERRORS";;
        EMC)
          PROXY_INTYPE="CELLS/0 CELLSTRGR/0 DECODERERR";;
        *)
          echo Input type for detector $i with FLP processing not defined 1>&2
          exit 1;;
      esac
    else
      PROXY_INTYPE=RAWDATA
    fi
    for j in $PROXY_INTYPE; do
      PROXY_INNAME="RAWIN$PROXY_IN_N"
      let PROXY_IN_N=$PROXY_IN_N+1
      PROXY_INSPEC+=";$PROXY_INNAME:$i/$j"
    done
  done
  [[ ! -z $TIMEFRAME_RATE_LIMIT ]] && [[ $TIMEFRAME_RATE_LIMIT != 0 ]] && PROXY_CHANNEL+=";name=metric-feedback,type=pull,method=connect,address=ipc://@metric-feedback-$NUMAID,transport=shmem,rateLogging=0"
  add_W o2-dpl-raw-proxy "--dataspec \"$PROXY_INSPEC\" --readout-proxy \"--channel-config \\\"$PROXY_CHANNEL\\\"\" ${TIMEFRAME_SHM_LIMIT+--timeframes-shm-limit} $TIMEFRAME_SHM_LIMIT" "" 0
elif [[ $DIGITINPUT == 1 ]]; then
  [[ $NTIMEFRAMES != 1 ]] && { echo "Digit input works only with NTIMEFRAMES=1"; exit 1; }
  DISABLE_DIGIT_ROOT_INPUT=
  DISABLE_DIGIT_CLUSTER_INPUT=
  TOF_INPUT=digits
  GPU_INPUT=zsonthefly
  has_detector TPC && add_W o2-tpc-reco-workflow "--input-type digits --output-type zsraw,disable-writer $DISABLE_MC --pipeline $(get_N tpc-zsEncoder TPC RAW 1 TPCRAWDEC)"
  has_detector MID && add_W o2-mid-digits-reader-workflow "$DISABLE_MC" ""
else
  if [[ $NTIMEFRAMES == -1 ]]; then NTIMEFRAMES_CMD= ; else NTIMEFRAMES_CMD="--loop $NTIMEFRAMES"; fi
  add_W o2-raw-file-reader-workflow "--detect-tf0 --delay $TFDELAY $NTIMEFRAMES_CMD --max-tf 0 --input-conf $RAWINPUTDIR/rawAll.cfg" "HBFUtils.nHBFPerTF=$NHBPERTF"
fi

# if root output is requested, record info of processed TFs DataHeader for replay of root files
[[ -z "$DISABLE_ROOT_OUTPUT" ]] && add_W o2-tfidinfo-writer-workflow

# ---------------------------------------------------------------------------------------------------------------------
# Raw decoder workflows - disabled in async mode
if [[ $CTFINPUT == 0 && $DIGITINPUT == 0 ]]; then
  if has_detector TPC && [[ $EPNSYNCMODE == 1 || "0$TPC_CONVERT_LINKZS_TO_RAW" == "01" ]] && [[ "0$TPC_NO_CONVERT_LINKZS_TO_RAW" != "01" ]]; then
    GPU_INPUT=zsonthefly
    add_W o2-tpc-raw-to-digits-workflow "--input-spec \"A:TPC/RAWDATA;dd:FLP/DISTSUBTIMEFRAME/0\" --remove-duplicates --pipeline $(get_N tpc-raw-to-digits-0 TPC RAW 1 TPCRAWDEC)"
    add_W o2-tpc-reco-workflow "--input-type digitizer --output-type zsraw,disable-writer --pipeline $(get_N tpc-zsEncoder TPC RAW 1 TPCRAWDEC)"
  fi
  has_detector ITS && add_W o2-itsmft-stf-decoder-workflow "--nthreads ${NITSDECTHREADS} --raw-data-dumps $ALPIDE_ERR_DUMPS --pipeline $(get_N its-stf-decoder ITS RAW 1 ITSRAWDEC)" "$ITSMFT_FILES"
  has_detector MFT && add_W o2-itsmft-stf-decoder-workflow "--nthreads ${NMFTDECTHREADS} --raw-data-dumps $ALPIDE_ERR_DUMPS --pipeline $(get_N mft-stf-decoder MFT RAW 1 MFTRAWDEC) --runmft true" "$ITSMFT_FILES"
  has_detector FT0 && ! has_detector_flp_processing FT0 && add_W o2-ft0-flp-dpl-workflow "$DISABLE_ROOT_OUTPUT --pipeline $(get_N ft0-datareader-dpl FT0 RAW 1)"
  has_detector FV0 && ! has_detector_flp_processing FV0 && add_W o2-fv0-flp-dpl-workflow "$DISABLE_ROOT_OUTPUT --pipeline $(get_N fv0-datareader-dpl FV0 RAW 1)"
  has_detector MID && add_W o2-mid-raw-to-digits-workflow "$MIDDEC_CONFIG --pipeline $(get_N MIDRawDecoder MID RAW 1),$(get_N MIDDecodedDataAggregator MID RAW 1)"
  has_detector MCH && add_W o2-mch-raw-to-digits-workflow "--pipeline $(get_N mch-data-decoder MCH RAW 1)"
  has_detector TOF && ! has_detector_flp_processing TOF && add_W o2-tof-compressor "--pipeline $(get_N tof-compressor TOF RAW 1)"
  has_detector FDD && ! has_detector_flp_processing FDD && add_W o2-fdd-flp-dpl-workflow "$DISABLE_ROOT_OUTPUT --pipeline $(get_N fdd-datareader-dpl FDD RAW 1)"
  has_detector TRD && add_W o2-trd-datareader "$TRD_DECODER_OPTIONS --pipeline $(get_N trd-datareader TRD RAW 1 TRDRAWDEC)" "" 0
  has_detector ZDC && add_W o2-zdc-raw2digits "$DISABLE_ROOT_OUTPUT --pipeline $(get_N zdc-datareader-dpl ZDC RAW 1)"
  has_detector HMP && add_W o2-hmpid-raw-to-digits-stream-workflow "--pipeline $(get_N HMP-RawStreamDecoder HMP RAW 1)"
  has_detector CTP && add_W o2-ctp-reco-workflow "--pipeline $(get_N CTP-RawStreamDecoder CTP RAW 1)"
  has_detector PHS && ! has_detector_flp_processing PHS && add_W o2-phos-reco-workflow "--input-type raw --output-type cells $DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT --pipeline $(get_N PHOSRawToCellConverterSpec PHS REST 1) $DISABLE_MC"
  has_detector CPV && add_W o2-cpv-reco-workflow "--input-type $CPV_INPUT --output-type clusters $DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT --pipeline $(get_N CPVRawToDigitConverterSpec CPV REST 1),$(get_N CPVClusterizerSpec CPV REST 1) $DISABLE_MC"
  has_detector EMC && ! has_detector_flp_processing EMC && add_W o2-emcal-reco-workflow "--input-type raw --output-type cells $EMCRAW2C_CONFIG $DISABLE_ROOT_OUTPUT $DISABLE_MC --pipeline $(get_N EMCALRawToCellConverterSpec EMC REST 1 EMCREC)"
fi

# ---------------------------------------------------------------------------------------------------------------------
# Common reconstruction workflows
(has_detector_reco TPC || has_detector_ctf TPC) && WORKFLOW+="o2-gpu-reco-workflow ${ARGS_ALL//-severity $SEVERITY/-severity $SEVERITY_TPC} --input-type=$GPU_INPUT $DISABLE_MC --output-type $GPU_OUTPUT --pipeline gpu-reconstruction:${N_TPCTRK:-1} $GPU_CONFIG $ARGS_EXTRA_PROCESS_o2_gpu_reco_workflow --configKeyValues \"$ARGS_ALL_CONFIG;GPU_global.deviceType=$GPUTYPE;GPU_proc.debugLevel=0;$GPU_CONFIG_KEY;$CONFIG_EXTRA_PROCESS_o2_gpu_reco_workflow\" | "
(has_detector_reco TOF || has_detector_ctf TOF) && add_W o2-tof-reco-workflow "$TOF_CONFIG --input-type $TOF_INPUT --output-type $TOF_OUTPUT $DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT $DISABLE_MC --pipeline $(get_N tof-compressed-decoder TOF RAW 1),$(get_N TOFClusterer TOF REST 1)"
has_detector_reco ITS && add_W o2-its-reco-workflow "--trackerCA $ITS_CONFIG $DISABLE_MC $DISABLE_DIGIT_CLUSTER_INPUT $DISABLE_ROOT_OUTPUT --pipeline $(get_N its-tracker ITS REST 1 ITSTRK)" "$ITS_CONFIG_KEY;$ITSMFT_FILES"
has_detectors_reco ITS TPC && has_detector_matching ITSTPC && add_W o2-tpcits-match-workflow "$DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT $DISABLE_MC --pipeline $(get_N itstpc-track-matcher MATCH REST 1 TPCITS)" "$ITSMFT_FILES"
has_detector_reco FT0 && add_W o2-ft0-reco-workflow "$DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT $DISABLE_MC --pipeline $(get_N ft0-reconstructor FT0 REST 1)"
has_detector_reco TRD && add_W o2-trd-tracklet-transformer "$DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT $DISABLE_MC $TRD_FILTER_CONFIG --pipeline $(get_N TRDTRACKLETTRANSFORMER TRD REST 1 TRDTRK)"
has_detector_reco TRD && [[ ! -z "$TRD_SOURCES" ]] && add_W o2-trd-global-tracking "$DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT $DISABLE_MC $TRD_CONFIG $TRD_FILTER_CONFIG --track-sources $TRD_SOURCES" "$TRD_CONFIG_KEY;$ITSMFT_FILES"
has_detector_reco TOF && [[ ! -z "$TOF_SOURCES" ]] && add_W o2-tof-matcher-workflow "$DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT $DISABLE_MC --track-sources $TOF_SOURCES --pipeline $(get_N tof-matcher TOF REST 1 TOFMATCH)" "$ITSMFT_FILES"
has_detectors TPC && [ -z "$DISABLE_ROOT_OUTPUT" ] && add_W o2-tpc-reco-workflow "--input-type pass-through --output-type clusters,tracks,send-clusters-per-sector $DISABLE_MC"

# ---------------------------------------------------------------------------------------------------------------------
# Reconstruction workflows normally active only in async mode in async mode ($LIST_OF_ASYNC_RECO_STEPS), but can be forced via $WORKFLOW_EXTRA_PROCESSING_STEPS
has_detector MID && has_processing_step MID_RECO && add_W o2-mid-reco-workflow "$DISABLE_ROOT_OUTPUT $DISABLE_MC --pipeline $(get_N MIDClusterizer MID REST 1),$(get_N MIDTracker MID REST 1)"
has_detector MCH && has_processing_step MCH_RECO && add_W o2-mch-reco-workflow "$DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT $DISABLE_MC --pipeline $(get_N mch-track-finder MCH REST 1 MCHTRK),$(get_N mch-cluster-finder MCH REST 1),$(get_N mch-cluster-transformer MCH REST 1)"
has_detector MFT && has_processing_step MFT_RECO && add_W o2-mft-reco-workflow "$DISABLE_DIGIT_CLUSTER_INPUT $DISABLE_MC $DISABLE_ROOT_OUTPUT --pipeline $(get_N mft-tracker MFT REST 1 MFTTRK)" "$ITSMFT_FILES"
has_detector FDD && has_processing_step FDD_RECO && add_W o2-fdd-reco-workflow "$DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT $DISABLE_MC"
has_detector FV0 && has_processing_step FV0_RECO && add_W o2-fv0-reco-workflow "$DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT $DISABLE_MC"
has_detector ZDC && has_processing_step ZDC_RECO && add_W o2-zdc-digits-reco "$DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT $DISABLE_MC"
has_detectors_reco MFT MCH && has_detector_matching MFTMCH && add_W o2-globalfwd-matcher-workflow "$DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT $DISABLE_MC --pipeline $(get_N globalfwd-track-matcher MATCH REST 1)"

# ---------------------------------------------------------------------------------------------------------------------
# Reconstruction workflows needed only in case QC was requested
has_detector_qc PHS && workflow_has_parameter QC && add_W o2-phos-reco-workflow "--input-type cells --output-type clusters $DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT $DISABLE_MC --pipeline $(get_N PHOSClusterizerSpec PHS REST 1)"

if [[ $BEAMTYPE != "cosmic" ]]; then
  has_detectors_reco ITS && has_detector_matching PRIMVTX && [[ ! -z "$VERTEXING_SOURCES" ]] && add_W o2-primary-vertexing-workflow "$DISABLE_MC $DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT $PVERTEX_CONFIG --pipeline $(get_N primary-vertexing MATCH REST 1)" "${PVERTEXING_CONFIG_KEY}"
  has_detectors_reco ITS && has_detector_matching SECVTX && [[ ! -z "$VERTEXING_SOURCES" ]] && add_W o2-secondary-vertexing-workflow "$DISABLE_DIGIT_ROOT_INPUT $DISABLE_ROOT_OUTPUT --vertexing-sources $VERTEXING_SOURCES --pipeline $(get_N secondary-vertexing MATCH REST 1)"
fi

# ---------------------------------------------------------------------------------------------------------------------
# Entropy encoding / ctf creation workflows - disabled in async mode
if has_processing_step ENTROPY_ENCODER && [[ ! -z "$WORKFLOW_DETECTORS_CTF" ]] && [[ $WORKFLOW_DETECTORS_CTF != "NONE" ]]; then
  # Entropy encoder workflows
  has_detector_ctf MFT && add_W o2-itsmft-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${MFT_ENC_MEMFACT:-1.5} --runmft true --pipeline $(get_N mft-entropy-encoder MFT CTF 1)"
  has_detector_ctf FT0 && add_W o2-ft0-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${FT0_ENC_MEMFACT:-1.5} --pipeline $(get_N ft0-entropy-encoder FT0 CTF 1)"
  has_detector_ctf FV0 && add_W o2-fv0-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${FV0_ENC_MEMFACT:-1.5} --pipeline $(get_N fv0-entropy-encoder FV0 CTF 1)"
  has_detector_ctf MID && add_W o2-mid-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${MID_ENC_MEMFACT:-1.5} --pipeline $(get_N mid-entropy-encoder MID CTF 1)"
  has_detector_ctf MCH && add_W o2-mch-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${MCH_ENC_MEMFACT:-1.5} --pipeline $(get_N mch-entropy-encoder MCH CTF 1)"
  has_detector_ctf PHS && add_W o2-phos-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${PHS_ENC_MEMFACT:-1.5} --pipeline $(get_N phos-entropy-encoder PHS CTF 1)"
  has_detector_ctf CPV && add_W o2-cpv-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${CPV_ENC_MEMFACT:-1.5} --pipeline $(get_N cpv-entropy-encoder CPV CTF 1)"
  has_detector_ctf EMC && add_W o2-emcal-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${EMC_ENC_MEMFACT:-1.5} --pipeline $(get_N emcal-entropy-encoder EMC CTF 1)"
  has_detector_ctf ZDC && add_W o2-zdc-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${ZDC_ENC_MEMFACT:-1.5} --pipeline $(get_N zdc-entropy-encoder ZDC CTF 1)"
  has_detector_ctf FDD && add_W o2-fdd-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${FDD_ENC_MEMFACT:-1.5} --pipeline $(get_N fdd-entropy-encoder FDD CTF 1)"
  has_detector_ctf HMP && add_W o2-hmpid-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${HMP_ENC_MEMFACT:-1.5} --pipeline $(get_N hmpid-entropy-encoder HMP CTF 1)"
  has_detector_ctf TOF && add_W o2-tof-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${TOF_ENC_MEMFACT:-1.5} --pipeline $(get_N tof-entropy-encoder TOF CTF 1)"
  has_detector_ctf ITS && add_W o2-itsmft-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${ITS_ENC_MEMFACT:-1.5} --pipeline $(get_N its-entropy-encoder ITS CTF 1)"
  has_detector_ctf TRD && add_W o2-trd-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${TRD_ENC_MEMFACT:-1.5} --pipeline $(get_N trd-entropy-encoder TRD CTF 1 TRDENT)"
  has_detector_ctf TPC && add_W o2-tpc-reco-workflow "--ctf-dict \"${CTF_DICT}\" --input-type compressed-clusters-flat --output-type encoded-clusters,disable-writer --mem-factor ${TPC_ENC_MEMFACT:-1.5} --pipeline $(get_N tpc-entropy-encoder TPC CTF 1 TPCENT)"
  has_detector_ctf CTP && add_W o2-ctp-entropy-encoder-workflow "--ctf-dict \"${CTF_DICT}\" --mem-factor ${CTP_ENC_MEMFACT:-1.5} --pipeline $(get_N its-entropy-encoder CTP CTF 1)"

  # CTF / dictionary writer workflow
  if [[ $SAVECTF == 1 && $WORKFLOWMODE == "run" ]]; then
    mkdir -p $CTF_DIR
  fi
  if [[ $CREATECTFDICT == 1 && $WORKFLOWMODE == "run" ]] ; then
    mkdir -p $CTF_DICT_DIR;
    rm -f $CTF_DICT
  fi
  CTF_OUTPUT_TYPE="none"
  if [[ $CREATECTFDICT == 1 ]] && [[ $SAVECTF == 1 ]]; then CTF_OUTPUT_TYPE="both"; fi
  if [[ $CREATECTFDICT == 1 ]] && [[ $SAVECTF == 0 ]]; then CTF_OUTPUT_TYPE="dict"; fi
  if [[ $CREATECTFDICT == 0 ]] && [[ $SAVECTF == 1 ]]; then CTF_OUTPUT_TYPE="ctf"; fi
  CONFIG_CTF="--output-dir \"$CTF_DIR\" --ctf-dict-dir \"$CTF_DICT_DIR\" --output-type $CTF_OUTPUT_TYPE --min-file-size ${CTF_MINSIZE} --max-ctf-per-file ${CTF_MAX_PER_FILE} --onlyDet $WORKFLOW_DETECTORS_CTF --append-det-to-period $CTF_MAXDETEXT --meta-output-dir $CTF_METAFILES_DIR"
  if [[ $CREATECTFDICT == 1 ]] && [[ $EXTINPUT == 1 ]]; then CONFIG_CTF+=" --save-dict-after $SAVE_CTFDICT_NTIMEFRAMES"; fi
  add_W o2-ctf-writer-workflow "$CONFIG_CTF"
fi

# ---------------------------------------------------------------------------------------------------------------------
# Calibration workflows
workflow_has_parameter CALIB && { source $MYDIR/calib-workflow.sh; [[ $? != 0 ]] && exit 1; }

# ---------------------------------------------------------------------------------------------------------------------
# Event display
# RS this is a temporary setting
[[ -z "$ED_TRACKS" ]] && ED_TRACKS=$TRACK_SOURCES
[[ -z "$ED_CLUSTERS" ]] && ED_CLUSTERS=$TRACK_SOURCES
workflow_has_parameter EVENT_DISPLAY && [[ $NUMAID == 0 ]] && [[ ! -z "$ED_TRACKS" ]] && [[ ! -z "$ED_CLUSTERS" ]] && add_W o2-eve-display "--display-tracks $ED_TRACKS --display-clusters $ED_CLUSTERS --skipOnEmptyInput --number-of_tracks 50000 $EVE_CONFIG $DISABLE_MC" "$ITSMFT_FILES"

# ---------------------------------------------------------------------------------------------------------------------
# AOD
[[ -z "$AOD_INPUT" ]] && AOD_INPUT=$TRACK_SOURCES
workflow_has_parameter AOD && [[ ! -z "$AOD_INPUT" ]] && add_W o2-aod-producer-workflow "--info-sources $AOD_INPUT $DISABLE_DIGIT_ROOT_INPUT --aod-writer-keep dangling --aod-writer-resfile "AO2D" --aod-writer-resmode UPDATE $DISABLE_MC"

# ---------------------------------------------------------------------------------------------------------------------
# Quality Control
workflow_has_parameter QC && { source $O2DPG_ROOT/DATA/production/qc-workflow.sh; [[ $? != 0 ]] && exit 1; }

if [[ ! -z "$EXTRA_WORKFLOW" ]]; then
  WORKFLOW+="$EXTRA_WORKFLOW"
fi

# ---------------------------------------------------------------------------------------------------------------------
# DPL run binary
WORKFLOW+="o2-dpl-run $ARGS_ALL $GLOBALDPLOPT"

if [[ "0$GEN_TOPO_AUTOSCALE_PROCESSES" == "01" ]]; then
  TOTAL_N_PIPELINES=`echo "${WORKFLOW}" | grep -o ':\$((([0-9]*\*\$AUTOSCALE_PROCESS_FACTOR' | grep -o '[0-9]*' | awk '{s+=$1} END {print s}'`
  TOTAL_N_CPUCORES=$(($NUMAGPUIDS == 1 ? 64 : 128))
  AUTOSCALE_PROCESS_FACTOR=$(($TOTAL_N_PIPELINES >= $TOTAL_N_CPUCORES && $TOTAL_N_PIPELINES != 0 ? 100 : ($TOTAL_N_CPUCORES * 100 / $TOTAL_N_PIPELINES)))
fi

# ---------------------------------------------------------------------------------------------------------------------
# Run / create / print workflow
if [[ "0$FST_BENCHMARK_STARTUP" == "01" ]]; then
  date 1>&2
  eval $WORKFLOW --dump > fst.startup.tmp.$NUMAID.json
  WORKFLOW2="cat fst.startup.tmp.$NUMAID.json | o2-dpl-run $ARGS_ALL $GLOBALDPLOPT"
  date 1>&2
  eval $WORKFLOW2
else
  [[ $WORKFLOWMODE != "print" ]] && WORKFLOW+=" --${WORKFLOWMODE}"
  [[ $WORKFLOWMODE == "print" || "0$PRINT_WORKFLOW" == "01" ]] && echo "#Workflow command:\n\n${WORKFLOW}\n" | sed -e "s/\\\\n/\n/g" -e"s/| */| \\\\\n/g" | eval cat $( [[ $WORKFLOWMODE == "dds" ]] && echo '1>&2')
  [[ $WORKFLOWMODE != "print" ]] && eval $WORKFLOW
fi

# ---------------------------------------------------------------------------------------------------------------------

<!-- doxy
\page refDetectorsCalibration Detectors Calibration
/doxy -->

# Time-interval based calibration flow for O2

The calibration flow of O2 foresees that every calibration device (expected to all run on one single aggregation node) will receive the TimeFrames with calibration input from every EPN in an asynchronous way. The calibration device will have to process the TFs in time intervals (TimeSlots) which allow to create CCDB entries with the needed granularity and update frequency (defined by the calibration device itself).

## TimeSlotCalibration<Input, Container>
Each calibration device (to be run in a workflow) has to derive from `o2::calibration::TimeSlotCalibration`, which is a templated class that takes as types the Input type (i.e. the object to be processed, coming from the upstream device) and the Container type (i.e. the object that will contain the calibration data per TimeSlot). Each calibration device has to be configured with the following parameters:

```cpp
tf-per-slot               : default length of a TiemSlot in TFs (will be widened in case of too little statistics). If this is set to `std::numeric_limits<long>::max()`, then there will be
                            only 1 slot at a time, valid till infinity.
updateInterval            : to be used together with `tf-per-slot = std::numeric_limits<long>::max()`: it allows to try to finalize the slot (and produce calibration) when the `updateInterval`
                            has passed. Note that this is an approximation (as explained in the code) due to the fact that TFs will come asynchronously (not ordered in time).
max-delay                 : maximum arrival delay of a TF with respect to the most recent one processed; units in number of TimeSlots; if beyond this, the TF will be considered too old, and discarded.
                            If `tf-per-slot == std::numeric_limits<long>::max()`, or `updateAtTheEndOfRunOnly == true`, its value is irrelevant.
updateAtTheEndOfRunOnly   : to tell the TimeCalibration to finalize the slots and prepare the CCDB entries only at the end of the run.
```
Example for the options above:
`tf-per-slot = 20`
`max-delay = 3`
Then if we are processing TF 61 and TF 0 comes, TF 0 will be discarded.

Each calibration device has to implement the following methods:

`void initOutput()`: initialization of the output object (typically a vector of calibration objects and another one with the associated CcdbObjectInfo);

`bool hasEnoughData(const o2::calibration::TimeSlot<Container>& slot)` : method to determine whether a TimeSlot has enough data to be calibrated; if not, it will be merged to the following (in time) one;

`void finalizeSlot(o2::calibration::TimeSlot<Container>& slot)` : method to process the calibration data accumulated in each TimeSlot;

`o2::calibration::TimeSlot<Container>& slot emplaceNewSlot(bool front, uint64_t tstart, uint64_t tend` : method to creata a new TimeSlot; this is specific to the calibration procedure as it instantiates the detector-calibration-specific object.

See e.g. LHCClockCalibrator.h/cxx in AliceO2/Detectors/TOF/calibration/include/TOFCalibration/LHCClockCalibrator.h and  AliceO2/Detectors/TOF/calibration/srcLHCClockCalibrator.cxx

## TimeSlot<Container>
The TimeSlot is a templated class which takes as input type the Container that will hold the calibration data needed to produce the calibration objects (histograms, vectors, array...). Each calibration device could implement its own Container, according to its needs.

The Container class needs to implement the following methods:

`void fill(const gsl::span<const Input> data)`  : method to decide how to use the calibration data within the container (e.g. fill a vector);
or
`void fill(o2::dataformats::TFIDInfo& ti, const gsl::span<const Input> data)`  : method to decide how to use the calibration data within the container (e.g. fill a vector) and having access to the TFIDInfo struct providing relevant info for current TF (tfCounter, runNumber, creationTime etc.)
If provided, this latter method will be used.

`void merge(const Container* prev)` : method to allow merging of the content of a TimeSlot to the content of the following one, when stastics is limited.

`void print()` : method to print the content of the Container

See e.g. LHCClockCalibrator.h/cxx in AliceO2/Detectors/TOF/calibration/include/TOFCalibration/LHCClockCalibrator.h and  AliceO2/Detectors/TOF/calibration/srcLHCClockCalibrator.cxx

## detector-specific-calibrator-workflow

Each calibration will need to be implemented in the form of a workflow, whose options should include those for the calibration device itself (`tf-per-slot` and `max-delay`, see above).
The output to be sent by the calibrator should include:

*   a vector of the snapshots of the object to be put in the CCDB;

*   a vector of the `o2::ccdb::CcdbObjectInfo` objects that contain the extra
information (metadata, startValidity...) associated to the objects themselves.

The origins of the pair of outputs will always be `o2::calibration::Utils::gDataOriginCDBPayload` and `o2::calibration::Utils::gDataOriginCDBWrapper` respectively, while the DataDescription must be unique for given calibration type, e.g.

```c++
output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBPayload, "TOF_LHCphase", i}, *image.get()); // vector<char>
output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBWrapper, "TOF_LHCphase", i}, w);            // root-serialized
```

See e.g. AliceO2/Detectors/TOF/calibration/testWorkflow/LHCClockCalibratorSpec.h,  AliceO2/Detectors/TOF/calibration/testWorkflow/lhc-clockphase-workflow.cxx

## ccdb-populator-workflow

This is the workflow that, connected to all workflows producting calibrations with different granularities and frequencies, will update the CCDB.

The `--ccdb-path` option of the ccdb-populator-workflow allows to define the CCDB destination (e.g. `--ccdb-path localhost:8080`).

By default the `ccdb-populator-workflow` will upload to the CCDB the inputs with any `SubSpec`. There is a possibility to run with multiple `ccdb-populators`, e.g. each one writing to particular CCDB server
(this might be needed e.g. to populate the production CCDB and the transient CCDB for DCS exchange, with the same or different objects). This can be done by piping
two populators, one of which should have device name modified. Additionally, one can pass an option for each populator to process only inputs with specific `SubSpecs`.
E.g. if your calibration device sends to the output
```
output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBPayload, "ObjA", 0}, *imageA.get());
output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBWrapper, "ObjA", 0}, wA);
output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBPayload, "ObjB", 1}, *imageB.get());
output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBWrapper, "ObjB", 1}, wB);
output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBPayload, "ObjC", 2}, *imageC.get());
output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBWrapper, "ObjC", 2}, wC);
```
in the workflow defined as:
```
<your_workflow> | o2-calibration-ccdb-populator-workflow --ccdb-path="http://localhost:8080" --name-extention loc  --sspec-min 1 --sspec-max 10 |
o2-calibration-ccdb-populator-workflow --sspec-min 0 --sspec-max 1  -b
```
then the `ObjA` will be uploaded only to the default server (`http://alice-ccdb.cern.ch`), `ObjB` will be uploaded to both default and `local` server and
`ObjC` will be uploaded to the `local` server only.

<!-- doxy
* \subpage refDetectorsCalibrationtestMacros
/doxy -->

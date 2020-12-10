#!/bin/bash -x

function update_from_upstream() {
  git checkout local-continuous-builds
  git fetch upstream
  git merge upstream/master
}

# Parameters:
#  ${1} - command to be executed
#  ${2} - log file
function execute_command_and_log() {
  echo "" >> ${2}
  echo "${1}" >> ${2}
  echo "" >> ${2}
  ${1} &>> ${2}
  return $?
}

# Parameters:
#  ${1} - target architecture
#  ${2} - xtensa_core
function run_xtensa_build() {
  TARGET_ARCH=${1}
  XTENSA_CORE=${2}

  LOG=${SCRIPT_DIR}/${TARGET_ARCH}_build_log
  rm -f ${LOG}
  echo "Building at ${HEAD_SHA}" >> ${LOG}

  execute_command_and_log "make -f tensorflow/lite/micro/tools/make/Makefile clean" ${LOG}

  #######################################################################
  # build keyword benchmark with BUILD_TYPE=release and profile the size.
  #######################################################################

  BUILD_COMMAND_RELEASE="make -f tensorflow/lite/micro/tools/make/Makefile TARGET=xtensa OPTIMIZED_KERNEL_DIR=xtensa TARGET_ARCH=${TARGET_ARCH} XTENSA_CORE=${XTENSA_CORE} keyword_benchmark BUILD_TYPE=release -j8"

  execute_command_and_log "${BUILD_COMMAND_RELEASE}" ${LOG}
  BUILD_RELEASE_RESULT=$?

  KEYWORD_BUILD_BADGE=${SCRIPT_DIR}/xtensa-${TARGET_ARCH}-keyword-build-status.svg
  BUILD_STATUS_LOG=${SCRIPT_DIR}/${TARGET_ARCH}_build_status

  if [[ ${BUILD_RELEASE_RESULT} != 0 ]]
  then
    # Here release build failed so mark failures and return appropriate error
    # code.
    /bin/cp ${SCRIPT_DIR}/TFLM-Xtensa-failed.svg ${KEYWORD_BUILD_BADGE}
    echo `date` ${HEAD_SHA} ${BUILD_RELEASE_RESULT} >> ${BUILD_STATUS_LOG}
    return ${BUILD_RELEASE_RESULT}
  fi

  # If the release build is successful, we first profile the size.
  SIZE_LOG=${SCRIPT_DIR}/${TARGET_ARCH}_size_log
  echo "" >> ${SIZE_LOG}
  date >> ${SIZE_LOG}
  echo "tensorflow version: "${HEAD_SHA} >> ${SIZE_LOG}

  xt-size tensorflow/lite/micro/tools/make/gen/xtensa_${TARGET_ARCH}/bin/keyword_benchmark &>> ${SIZE_LOG}

  # Save a plot showing the evolution of the size.
  python3 ${SCRIPT_DIR}/plot_size.py ${SIZE_LOG} --output_plot ${SCRIPT_DIR}/${TARGET_ARCH}_size_history.png --hide

  # Next, we try the non-release build where we can log the cycles.
  execute_command_and_log "make -f tensorflow/lite/micro/tools/make/Makefile clean" ${LOG}

  COMMAND="make -f tensorflow/lite/micro/tools/make/Makefile TARGET=xtensa OPTIMIZED_KERNEL_DIR=xtensa TARGET_ARCH=${TARGET_ARCH} XTENSA_CORE=${XTENSA_CORE} keyword_benchmark -j8"
  execute_command_and_log "${COMMAND}" ${LOG}
  BUILD_RESULT=$?
  if [[ ${BUILD_RESULT} != 0 ]]
  then
    /bin/cp ${SCRIPT_DIR}/TFLM-Xtensa-failed.svg ${KEYWORD_BUILD_BADGE}
    echo `date` ${HEAD_SHA} ${BUILD_RESULT} >> ${BUILD_STATUS_LOG}
    return ${BUILD_RESULT}
  fi

  # Profile the cycles.
  LATENCY_LOG=${SCRIPT_DIR}/${TARGET_ARCH}_latency_log
  echo "" >> ${LATENCY_LOG}
  date >> ${LATENCY_LOG}
  echo "tensorflow version: "${HEAD_SHA} >> ${LATENCY_LOG}
  xt-run tensorflow/lite/micro/tools/make/gen/xtensa_${TARGET_ARCH}/bin/keyword_benchmark --xtensa-core=${XTENSA_CORE} &>> ${LATENCY_LOG}

  # Save a plot showing the evolution of the latency.
  python3 ${SCRIPT_DIR}/plot_latency.py ${LATENCY_LOG} --output_plot ${SCRIPT_DIR}/${TARGET_ARCH}_latency_history.png --hide

  # Build was successful.
  /bin/cp ${SCRIPT_DIR}/TFLM-Xtensa-passing.svg ${KEYWORD_BUILD_BADGE}
  echo `date` ${HEAD_SHA} ${BUILD_RESULT} >> ${BUILD_STATUS_LOG}
}

# Parameters:
#  ${1} - target architecture
#  ${2} - xtensa_core
function run_xtensa_unittests() {
  TARGET_ARCH=${1}
  XTENSA_CORE=${1}
  LOG=${SCRIPT_DIR}/${TARGET_ARCH}_unittest_log

  execute_command_and_log "make -f tensorflow/lite/micro/tools/make/Makefile clean" ${LOG}

  TEST_COMMAND="make -f tensorflow/lite/micro/tools/make/Makefile TARGET=xtensa OPTIMIZED_KERNEL_DIR=xtensa TARGET_ARCH=${TARGET_ARCH} XTENSA_CORE=${XTENSA_CORE} test"
  execute_command_and_log "${TEST_COMMAND}" ${LOG}
  RESULT=$?

  STATUS_LOG=${SCRIPT_DIR}/${TARGET_ARCH}_unittest_status
  echo `date` ${HEAD_SHA} ${RESULT} >> ${STATUS_LOG}

  UNITTEST_BUILD_BADGE=${SCRIPT_DIR}/xtensa-${TARGET_ARCH}-unittests-status.svg
  if [[ ${RESULT} == 0 ]]
  then
    /bin/cp ${SCRIPT_DIR}/TFLM-Xtensa-passing.svg ${UNITTEST_BUILD_BADGE}
  else
    /bin/cp ${SCRIPT_DIR}/TFLM-Xtensa-failed.svg ${UNITTEST_BUILD_BADGE}
  fi

  return ${RESULT}
}

# Parameters:
# ${1} - target architechture
# ${2} - xtensa core
function test_arch() {
  run_xtensa_build ${1} ${2}
  BUILD_RESULT=$?

  run_xtensa_unittests ${1} ${2}
  UNITTEST_RESULT=$?

  if [[ ${BUILD_RESULT} == 0 && ${UNITTEST_RESULT} == 0 ]]
  then
    return 0
  fi

  return 1
}

###############################################################3
###############################################################3
# Start of the test flow.
###############################################################3
###############################################################3

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pushd ${SCRIPT_DIR}

cd ../../../../../

OVERALL_BUILD_STATUS_BADGE=${SCRIPT_DIR}/xtensa-build-status.svg
/bin/cp ${SCRIPT_DIR}/TFLM-Xtensa-failed.svg ${OVERALL_BUILD_STATUS_BADGE}


update_from_upstream
make -f tensorflow/lite/micro/tools/make/Makefile clean clean_downloads

HEAD_SHA=`git rev-parse upstream/master`

test_arch hifimini mini1m1m_RG
HIFIMINI_RESULT=$?

test_arch fusion_f1 Google_F1
FUSION_F1_RESULT=$?

if [[ ${HIFIMINI_RESULT} == 0 && ${FUSION_F1_RESULT} == 0 ]]
then
  # All is well, we can update overall badge to indicate passing.
  /bin/cp ${SCRIPT_DIR}/TFLM-Xtensa-passing.svg ${OVERALL_BUILD_STATUS_BADGE}
fi


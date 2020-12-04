#!/bin/bash -x

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pushd ${SCRIPT_DIR}

cd ../../../../../

git checkout local-continuous-builds
git fetch upstream
git merge upstream/master
HEAD_SHA=`git rev-parse upstream/master`

INVOCATION_LOG=/tmp/hifimini_build_log

rm -f ${INVOCATION_LOG}
echo "Building at ${HEAD_SHA}" >> ${INVOCATION_LOG}

CLEAN_COMMAND="make -f tensorflow/lite/micro/tools/make/Makefile clean clean_downloads"
echo "" >> ${INVOCATION_LOG}
echo "Clean command: ${CLEAN_COMMAND}" >> ${INVOCATION_LOG}
echo "" >> ${INVOCATION_LOG}
${CLEAN_COMMAND} &>> ${INVOCATION_LOG}

#######################################################################
# build keyword benchmark with BUILD_TYPE=release and profile the size.
#######################################################################

BUILD_COMMAND="make -f tensorflow/lite/micro/tools/make/Makefile TARGET=xtensa OPTIMIZED_KERNEL_DIR=xtensa TARGET_ARCH=hifimini XTENSA_CORE=mini1m1m_RG keyword_benchmark BUILD_TYPE=release"
echo "" >> ${INVOCATION_LOG}
echo "Build command: ${BUILD_COMMAND}" >> ${INVOCATION_LOG}
echo "" >> ${INVOCATION_LOG}

${BUILD_COMMAND} &>> ${INVOCATION_LOG}
BUILD_RESULT=$?

BUILD_STATUS_LOG=${SCRIPT_DIR}/hifimini_build_status
echo `date` ${HEAD_SHA} ${BUILD_RESULT} >> ${BUILD_STATUS_LOG}

KEYWORD_BUILD_BADGE=${SCRIPT_DIR}/xtensa-keyword-build-status.svg
if [[ ${BUILD_RESULT} == 0 ]]
then
  # Document successful build of keyword benchmark.
  /bin/cp ${SCRIPT_DIR}/TFLM-Xtensa-passing.svg ${KEYWORD_BUILD_BADGE}

  SIZE_COMMAND="xt-size tensorflow/lite/micro/tools/make/gen/xtensa_hifimini/bin/keyword_benchmark"
  SIZE_LOG=${SCRIPT_DIR}/hifimini_size_log
  echo "" >> ${SIZE_LOG}
  date >> ${SIZE_LOG}
  echo "tensorflow version: "${HEAD_SHA} >> ${SIZE_LOG}
  ${SIZE_COMMAND} >> ${SIZE_LOG}

  # Save a plot showing the evolution of the size.
  python3 ${SCRIPT_DIR}/plot_size.py ${SIZE_LOG} --output_plot ${SCRIPT_DIR}/hifimini_size_history.png --hide
else
  /bin/cp ${SCRIPT_DIR}/TFLM-Xtensa-failed.svg ${KEYWORD_BUILD_BADGE}
fi

########################################
# Run all the unit tests
########################################

CLEAN_COMMAND="make -f tensorflow/lite/micro/tools/make/Makefile clean"
echo "" >> ${INVOCATION_LOG}
echo "Clean command: ${CLEAN_COMMAND}" >> ${INVOCATION_LOG}
echo "" >> ${INVOCATION_LOG}
${CLEAN_COMMAND} &>> ${INVOCATION_LOG}

TEST_COMMAND="make -f tensorflow/lite/micro/tools/make/Makefile TARGET=xtensa OPTIMIZED_KERNEL_DIR=xtensa TARGET_ARCH=hifimini XTENSA_CORE=mini1m1m_RG test"
echo "" >> ${INVOCATION_LOG}
echo "Test command: ${TEST_COMMAND}" >> ${INVOCATION_LOG}
echo "" >> ${INVOCATION_LOG}

${TEST_COMMAND} &>> ${INVOCATION_LOG}
TEST_RESULT=$?

TEST_STATUS_LOG=${SCRIPT_DIR}/hifimini_test_status
echo `date` ${HEAD_SHA} ${TEST_RESULT} >> ${TEST_STATUS_LOG}

UNITTEST_BUILD_BADGE=${SCRIPT_DIR}/xtensa-unittests-status.svg
if [[ ${TEST_RESULT} == 0 ]]
then
  /bin/cp ${SCRIPT_DIR}/TFLM-Xtensa-passing.svg ${UNITTEST_BUILD_BADGE}
else
  /bin/cp ${SCRIPT_DIR}/TFLM-Xtensa-failed.svg ${UNITTEST_BUILD_BADGE}
fi


############################
# Overall status reporting
############################

OVERALL_BUILD_STATUS_BADGE=${SCRIPT_DIR}/xtensa-build-status.svg
if [[ ${BUILD_RESULT} == 0 && ${TEST_RESULT} == 0 ]]
then
  # All is well, we can update overall badge to indicate passing.
  /bin/cp ${SCRIPT_DIR}/TFLM-Xtensa-passing.svg ${OVERALL_BUILD_STATUS_BADGE}
else
  /bin/cp ${SCRIPT_DIR}/TFLM-Xtensa-failed.svg ${OVERALL_BUILD_STATUS_BADGE}
fi

mv ${INVOCATION_LOG} ${SCRIPT_DIR}


# Every optimized kernel implementation directory (i.e.
# micro/kernels/<optimized_kernel_dir>/ must have a corresponding
# micro/tools/make/ext_libs/<optimized_kernel_dir>.inc


ifeq ($(TARGET_ARCH), hifi4)
  $(eval $(call add_third_party_download,$(XTENSA_HIFI4_URL),$(XTENSA_HIFI4_MD5),xa_nnlib_hifi4,))

  NNLIB_PATH := $(MAKEFILE_DIR)/downloads/xa_nnlib_hifi4

  $(shell $(DOWNLOAD_SCRIPT) $(XTENSA_HIFI4_URL) $(XTENSA_HIFI4_MD5) $()))

  THIRD_PARTY_CC_SRCS += \
    $(call recursive_find,$(NNLIB_PATH)/CMSIS/NN/Source,*.c)

  INCLUDES += \
    -I$(NNLIB_PATH)/algo/kernels/ \
    -I$(NNLIB_PATH)/include/nnlib/ \
    -I$(NNLIB_PATH)/include/ \
    -I$(NNLIB_PATH)/algo/common/include/ \
    -I$(NNLIB_PATH)/algo/ndsp/hifi4/include/

endif

/*
 * Copyright 2020 by Siklu Ltd. All rights reserved.
 */

#include "ubi_device.h"

#include <folly/Format.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <fstream>
#include <memory>

#include "log.h"

#define PROGRAM_NAME "mtd-utils"  // used by the mtd-utils lib (must be defined)
#include <common.h>
#include <libscan.h>
#include <libubigen.h>
#include <mtd_table.h>
#include <raii.h>

#include <iostream>
#include <string>

template <class T>
struct MallocDeleter {
  void operator()(T* ptr) { free(ptr); }
};

template <class T>
using MallocUniquePtr = std::unique_ptr<T, MallocDeleter<T>>;

constexpr folly::StringPiece kDefaultCtrlDev = "/dev/ubi_ctrl";
constexpr folly::StringPiece kUbiDeviceFilePrefix = "/dev/ubi";
constexpr folly::StringPiece kMtdDeviceFilePrefix = "/dev/mtd";
constexpr folly::StringPiece kUbiVolumeByNamePrefix_path =
    "/dev/ubi-volumes/by-name";

// attach request defaults
constexpr int32_t kAttachDefaultDevNum = UBI_DEV_NUM_AUTO;
constexpr int32_t kAttachDefaultMtdNum = -1;
constexpr int32_t kAttachDefaultVidHdrOffset = 0;
constexpr int32_t kAttachDefaultMaxBebPer1024 = 0;

// make volume defaults
constexpr int32_t kMakeVolDefaultVolId = UBI_VOL_NUM_AUTO;
constexpr int32_t kMakeVolDefaultAlignment = 1;
constexpr int32_t kMakeVolDefaultVolType = UBI_DYNAMIC_VOLUME;

// format constants
constexpr int32_t kMaxConsecutiveBadBlocks = 4;

using UbiLibFileHandle = RAII<libubi_t, &libubi_close>;
using MtdLibFileHandle = RAII<libmtd_t, &libmtd_close>;
using CStyleFileHandle = RAII<int, &close>;

static folly::Expected<UbiLibFileHandle, UbiDevice::ErrorCode>
CreateUbiLibFileHandle();

static folly::Expected<MtdLibFileHandle, UbiDevice::ErrorCode>
CreateMtdLibFileHandle();

// constructor
UbiDevice::UbiDevice(int mtd_num) : is_attached_{false}, mtd_num_(mtd_num) {}

// move constructor
UbiDevice::UbiDevice(UbiDevice&& other)
    : is_attached_{false},
      mtd_num_(other.mtd_num_),
      ubi_device_file_name_(std::move(other.ubi_device_file_name_)) {
  std::swap(is_attached_, other.is_attached_);
}

// move assignment operator
UbiDevice& UbiDevice::operator=(UbiDevice&& other) {
  if (this != &other) {
    is_attached_ = false;
    std::swap(is_attached_, other.is_attached_);

    mtd_num_ = std::move(other.mtd_num_);
    ubi_device_file_name_ = std::move(other.ubi_device_file_name_);
  }

  return *this;
}

// Create
folly::Expected<std::shared_ptr<IUbiDevice>, int32_t> UbiDevice::Create(
    const std::string& mtd_device_name, bool is_to_format_first) {
  auto create_mtd_table_result = MtdTable::Create();

  if (create_mtd_table_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "MtdTable::Create failed! error code = "
                       << int(create_mtd_table_result.error());
    return folly::makeUnexpected(
        int(ErrorCode::CREATE__MTD_TABLE_CREATE_FAILED_ERROR));
  }
  auto mtd_table = create_mtd_table_result.value();

  // get mtd_num
  auto get_mtd_num_result = mtd_table->GetMtdNum(mtd_device_name);
  if (get_mtd_num_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "GetMtdNum failed! error code = "
                       << int(get_mtd_num_result.error()) << " mtd_device_name "
                       << mtd_device_name;
    return folly::makeUnexpected(
        int(ErrorCode::CREATE__MTD_NAME_TO_NUM_NOT_FOUND_ERROR));
  }

  auto mtd_num = get_mtd_num_result.value();

  // format the UBI volume. (as an optional preperation before the UBI object
  // creation)
  if (is_to_format_first) {
    auto get_format_result = Format(mtd_num);
    if (get_format_result.hasError()) {
      SKL_LOG(SKL_ERROR) << "Format failed! error code = "
                         << int(get_format_result.error()) << " mtd_num "
                         << mtd_num;
      return folly::makeUnexpected(int(get_format_result.error()));
    }
  }

  UbiDevice ubi_device(mtd_num);

  auto attach_result = ubi_device.Attach();
  if (attach_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "ubi_device_.Attach failed! "
                       << " error=" << int(attach_result.error());
    return folly::makeUnexpected(int(attach_result.error()));
  }

  return std::make_unique<UbiDevice>(std::move(ubi_device));
}

// CheckKernelSupportForAttachDetachRequest
folly::Expected<folly::Unit, UbiDevice::ErrorCode>
UbiDevice::CheckKernelSupportForAttachDetachRequest(libubi_t lib_ubi_fd) {
  struct ubi_info ubi_info;
  int ret = 0;

  ret = ubi_get_info(lib_ubi_fd, &ubi_info);
  if (ret) {
    SKL_LOG(SKL_ERROR) << "ubi_get_info failed! error code = " << ret;
    return folly::makeUnexpected(
        ErrorCode::CHECK_KERNEL_SUPPORT__CANNOT_GET_UBI_INFORMATION_ERROR);
  }

  if (ubi_info.ctrl_major == -1) {
    SKL_LOG(SKL_ERROR)
        << "Mtd Attach Detach feature is not supported by the kernel";
    return folly::makeUnexpected(
        ErrorCode::
            CHECK_KERNEL_SUPPORT__ATTACH_DETACH_FEATURE_IS_NOT_SUPPORTED);
  }

  return folly::unit;
}

// Attach
folly::Expected<folly::Unit, UbiDevice::ErrorCode> UbiDevice::Attach(void) {
  int ret;
  struct ubi_attach_request req;

  // get UBI lib file descriptor
  auto get_ubi_lib_fd_result = CreateUbiLibFileHandle();
  if (get_ubi_lib_fd_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "CreateUbiLibFileHandle failed! error code = "
                       << int(get_ubi_lib_fd_result.error());
    return folly::makeUnexpected(get_ubi_lib_fd_result.error());
  }
  libubi_t lib_ubi_fd = get_ubi_lib_fd_result.value().GetValue();

  // Make sure the kernel is fresh enough and this feature is supported.
  auto kernel_support_result =
      CheckKernelSupportForAttachDetachRequest(lib_ubi_fd);
  if (kernel_support_result.hasError()) {
    SKL_LOG(SKL_ERROR)
        << "CheckKernelSupportForAttachDetachRequest failed! error code = "
        << int(kernel_support_result.error());
    return folly::makeUnexpected(kernel_support_result.error());
  }

  req.mtd_dev_node = nullptr;
  req.dev_num = kAttachDefaultDevNum;
  req.mtd_num = mtd_num_;
  req.vid_hdr_offset = kAttachDefaultVidHdrOffset;
  req.max_beb_per1024 = kAttachDefaultMaxBebPer1024;

  ret = ubi_attach(lib_ubi_fd, kDefaultCtrlDev.str().c_str(), &req);
  if (ret < 0) {
    SKL_LOG(SKL_ERROR) << "ubi_attach failed! error code = " << ret
                       << " mtd_num=" << mtd_num_;
    return folly::makeUnexpected(ErrorCode::ATTACH__CANNOT_ATTACH_MTD_DEVICE);
  }

  // get UBI dev number
  int ubi_dev_num;
  ret = mtd_num2ubi_dev(lib_ubi_fd, mtd_num_, &ubi_dev_num);
  if (ret) {
    SKL_LOG(SKL_ERROR) << "mtd_num2ubi_dev failed! error code = " << ret
                       << " mtd_num=" << mtd_num_;
    return folly::makeUnexpected(ErrorCode::ATTACH__MTD_NUM_TO_UBI_ERROR);
  }

  ubi_device_file_name_ =
      std::move(folly::sformat("{}{}", kUbiDeviceFilePrefix, ubi_dev_num));
  is_attached_ = true;

  return folly::unit;
}

// Detach
folly::Expected<folly::Unit, UbiDevice::ErrorCode> UbiDevice::Detach(void) {
  int ret = 0;

  // get UBI lib file descriptor
  auto get_ubi_lib_fd_result = CreateUbiLibFileHandle();
  if (get_ubi_lib_fd_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "CreateUbiLibFileHandle failed! error code = "
                       << int(get_ubi_lib_fd_result.error());
    return folly::makeUnexpected(get_ubi_lib_fd_result.error());
  }
  libubi_t lib_ubi_fd = get_ubi_lib_fd_result.value().GetValue();

  // Make sure the kernel is fresh enough and this feature is supported.
  auto kernel_support_result =
      CheckKernelSupportForAttachDetachRequest(lib_ubi_fd);
  if (kernel_support_result.hasError()) {
    SKL_LOG(SKL_ERROR)
        << "CheckKernelSupportForAttachDetachRequest failed! error code = "
        << int(kernel_support_result.error());
    return folly::makeUnexpected(
        ErrorCode::
            CHECK_KERNEL_SUPPORT__ATTACH_DETACH_FEATURE_IS_NOT_SUPPORTED);
  }

  ret = ubi_detach_mtd(lib_ubi_fd, kDefaultCtrlDev.str().c_str(), mtd_num_);
  if (ret < 0) {
    SKL_LOG(SKL_ERROR) << "ubi_detach_mtd failed! error code = " << ret
                       << " mtd_num_=" << mtd_num_;
    return folly::makeUnexpected(
        ErrorCode::DETACH__CANNOT_DETACH_MTD_DEVICE_ERROR);
  }

  return folly::unit;
}

// MakeVolume
folly::Expected<folly::Unit, int32_t> UbiDevice::MakeVolume(
    const std::string& vol_name, uint32_t size_in_bytes) {
  int ret = 0;
  struct ubi_dev_info dev_info;
  struct ubi_mkvol_request req = {};

  // get UBI lib file descriptor
  auto get_ubi_lib_fd_result = CreateUbiLibFileHandle();
  if (get_ubi_lib_fd_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "CreateUbiLibFileHandle failed! error code = "
                       << int(get_ubi_lib_fd_result.error());
    return folly::makeUnexpected(int(get_ubi_lib_fd_result.error()));
  }
  libubi_t lib_ubi_fd = get_ubi_lib_fd_result.value().GetValue();

  // ubi probe node
  auto ubi_probe_node_result = UbiProbeNode(lib_ubi_fd, ubi_device_file_name_);
  if (ubi_probe_node_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "UbiProbeNode failed! error code = "
                       << int(ubi_probe_node_result.error())
                       << "ubi_device_file_name_=" << ubi_device_file_name_;
    return folly::makeUnexpected(int(ErrorCode::UBI_PROBE_NODE_FAILED_ERROR));
  }

  ret = ubi_get_dev_info(lib_ubi_fd, ubi_device_file_name_.c_str(), &dev_info);
  if (ret) {
    SKL_LOG(SKL_ERROR)
        << "ubi_get_dev_info failed! cannot get information about UBI "
           "device. ubi_device_file_name_="
        << ubi_device_file_name_;
    return folly::makeUnexpected(
        int(ErrorCode::MAKE_VOLUME__UBI_GET_DEV_INFO_ERROR));
  }

  if (dev_info.avail_bytes == 0) {
    SKL_LOG(SKL_ERROR) << "UBI device does not have free logical eraseblocks. "
                          "ubi_device_file_name_="
                       << ubi_device_file_name_;
    return folly::makeUnexpected(int(
        ErrorCode::
            MAKE_VOLUME__UBI_DEVICE_NOT_ENOUGH_FREE_LOGICAL_ERASEBLOCKS_ERROR));
  }

  req.vol_id = kMakeVolDefaultVolId;
  req.alignment = kMakeVolDefaultAlignment;
  req.bytes = (size_in_bytes == 0 ? dev_info.avail_bytes : size_in_bytes);
  // req.bytes = size_in_bytes;
  req.vol_type = kMakeVolDefaultVolType;
  req.name = vol_name.c_str();

  ret = ubi_mkvol(lib_ubi_fd, ubi_device_file_name_.c_str(), &req);
  if (ret < 0) {
    SKL_LOG(SKL_ERROR) << "ubi_mkvol failed! ubi_device_file_name_="
                       << ubi_device_file_name_ << " name=" << req.name
                       << " size=" << req.bytes;
    return folly::makeUnexpected(int(ErrorCode::MAKE_VOLUME__GENERAL_ERROR));
  }

  return folly::unit;
}

// RemoveVolume
folly::Expected<folly::Unit, int32_t> UbiDevice::RemoveVolume(
    const std::string& vol_name, bool is_to_print_log_error) {
  int ret = 0;

  // get UBI lib file descriptor
  auto get_ubi_lib_fd_result = CreateUbiLibFileHandle();
  if (get_ubi_lib_fd_result.hasError()) {
    if (is_to_print_log_error) {
      SKL_LOG(SKL_ERROR) << "CreateUbiLibFileHandle failed! error code = "
                         << int(get_ubi_lib_fd_result.error());
    }
    return folly::makeUnexpected(int(get_ubi_lib_fd_result.error()));
  }
  libubi_t lib_ubi_fd = get_ubi_lib_fd_result.value().GetValue();

  // ubi probe node
  auto ubi_probe_node_result =
      UbiProbeNode(lib_ubi_fd, ubi_device_file_name_, is_to_print_log_error);
  if (ubi_probe_node_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "UbiProbeNode failed! error code = "
                       << int(ubi_probe_node_result.error())
                       << "ubi_device_file_name_=" << ubi_device_file_name_;
    return folly::makeUnexpected(int(ErrorCode::UBI_PROBE_NODE_FAILED_ERROR));
  }

  struct ubi_dev_info dev_info;
  struct ubi_vol_info vol_info;

  ret = ubi_get_dev_info(lib_ubi_fd, ubi_device_file_name_.c_str(), &dev_info);
  if (ret) {
    if (is_to_print_log_error) {
      SKL_LOG(SKL_ERROR)
          << "ubi_get_dev_info failed! cannot get information about UBI "
             " device. ubi_device_file_name_="
          << ubi_device_file_name_ << " ret=" << ret;
    }
    return folly::makeUnexpected(
        int(ErrorCode::
                REMOVE_VOLUME__CANNOT_FIND_INFORMATION_ABOUT_UBI_DEVICE_ERROR));
  }

  ret = ubi_get_vol_info1_nm(lib_ubi_fd, dev_info.dev_num, vol_name.c_str(),
                             &vol_info);
  if (ret) {
    if (is_to_print_log_error) {
      SKL_LOG(SKL_ERROR)
          << "ubi_get_vol_info1_nm failed! cannot find UBI volume. UBI device="
          << ubi_device_file_name_ << " dev_num=" << dev_info.dev_num
          << " ret=" << ret;
    }
    return folly::makeUnexpected(int(ErrorCode::CANNOT_FIND_UBI_VOLUME_ERROR));
  }

  ret = ubi_rmvol(lib_ubi_fd, ubi_device_file_name_.c_str(), vol_info.vol_id);
  if (ret) {
    if (is_to_print_log_error) {
      SKL_LOG(SKL_ERROR)
          << "ubi_rmvol_nm failed! cannot UBI remove volume.  UBI device="
          << ubi_device_file_name_ << " volume_id=" << vol_info.vol_id
          << " ret=" << ret;
    }
    return folly::makeUnexpected(int(ErrorCode::REMOVE_VOLUME__GENERAL_ERROR));
  }

  return folly::unit;
}

// destructor
UbiDevice::~UbiDevice() {
  if (is_attached_) {
    auto detach_result = Detach();
    if (detach_result.hasError()) {
      SKL_LOG(SKL_ERROR) << "Detach() failed! "
                         << " error=" << int(detach_result.error());
    }
  }
}

// CreateUbiLibFileHandle
static folly::Expected<UbiLibFileHandle, UbiDevice::ErrorCode>
CreateUbiLibFileHandle() {
  libubi_t libubi_fd = libubi_open();
  if (!libubi_fd) {
    if (errno == 0) {
      SKL_LOG(SKL_ERROR)
          << "libubi_open failed! UBI is not present in the system";
      return folly::makeUnexpected(
          UbiDevice::ErrorCode::OPEN_LIB_UBI__UBI_IS_NOT_PRESENT_IN_THE_SYSTEM);
    }
    SKL_LOG(SKL_ERROR) << "libubi_open failed! cannot open libubi";
    return folly::makeUnexpected(
        UbiDevice::ErrorCode::OPEN_LIB_UBI__CANNOT_OPEN_LIBUBI_ERROR);
  }

  return UbiLibFileHandle(libubi_fd);
}

// CreateMtdLibFileHandle
static folly::Expected<MtdLibFileHandle, UbiDevice::ErrorCode>
CreateMtdLibFileHandle() {
  libmtd_t libmtd_fd = libmtd_open();
  if (!libmtd_fd) {
    if (errno == 0) {
      SKL_LOG(SKL_ERROR)
          << "libmtd_open failed! MTD is not present in the system";
      return folly::makeUnexpected(
          UbiDevice::ErrorCode::OPEN_LIB_MTD__MTD_IS_NOT_PRESENT_IN_THE_SYSTEM);
    }
    SKL_LOG(SKL_ERROR) << "libmtd_open failed! cannot open libmtd";
    return folly::makeUnexpected(
        UbiDevice::ErrorCode::OPEN_LIB_MTD__CANNOT_OPEN_LIBMTD_ERROR);
  }

  return MtdLibFileHandle(libmtd_fd);
}

// CreateCStyleFileHandle
static folly::Expected<CStyleFileHandle, UbiDevice::ErrorCode>
CreateCStyleFileHandle(std::string file_name, int mode) {
  int fd = open(file_name.c_str(), mode);
  if (fd == -1) {
    SKL_LOG(SKL_ERROR) << "open " << file_name << " failed! mode= " << mode;
    return folly::makeUnexpected(
        UbiDevice::ErrorCode::CANNOT_OPEN_MTD_DEVICE_FILE_ERROR);
  }

  return CStyleFileHandle(fd);
}

// Format
folly::Expected<folly::Unit, UbiDevice::ErrorCode> UbiDevice::Format(
    MtdTable::MtdNum mtd_num) {
  auto ret = 0;

  struct FormatAttr format_attr;

  struct mtd_info mtd_info = {};
  struct mtd_dev_info mtd = {};

  // get MTD lib file descriptor
  auto get_mtd_lib_fd_result = CreateMtdLibFileHandle();
  if (get_mtd_lib_fd_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "CreateMtdLibFileHandle failed! error code = "
                       << int(get_mtd_lib_fd_result.error());
    return folly::makeUnexpected(get_mtd_lib_fd_result.error());
  }
  libmtd_t lib_mtd_fd = get_mtd_lib_fd_result.value().GetValue();

  // mtd_get_info
  ret = mtd_get_info(lib_mtd_fd, &mtd_info);
  if (ret) {
    SKL_LOG(SKL_ERROR)
        << "mtd_get_info failed! cannot get MTD information. error code = "
        << ret;
    return folly::makeUnexpected(ErrorCode::FORMAT__MTD_GET_INFO_FAILURE_ERROR);
  }

  // combine mtd device file name
  std::string mtd_device_file_name =
      std::move(folly::sformat("{}{}", kMtdDeviceFilePrefix, mtd_num));

  // mtd_get_dev_info
  ret = mtd_get_dev_info(lib_mtd_fd, mtd_device_file_name.c_str(), &mtd);
  if (ret) {
    SKL_LOG(SKL_ERROR)
        << "mtd_get_dev_info failed! cannot get information about "
        << mtd_device_file_name << " error code = " << ret;
    return folly::makeUnexpected(
        ErrorCode::FORMAT__MTD_GET_DEV_INFO_FAILURE_ERROR);
  }

  // is power of 2 validation
  if (!is_power_of_2(mtd.min_io_size)) {
    SKL_LOG(SKL_ERROR) << "min. I/O size is " << mtd.min_io_size
                       << " but should be power of 2";
    return folly::makeUnexpected(
        ErrorCode::FORMAT__MIN_IO_SIZE_NOT_POWER_OF_2_ERROR);
  }

  if (!mtd_info.sysfs_supported) {
    /*
     * Linux kernels older than 2.6.30 did not support sysfs
     * interface, and it is impossible to find out sub-page
     * size in these kernels.
     */
    SKL_LOG(SKL_WARNING)
        << "your MTD system is old and it is impossible to detect "
           "sub-page size. Use -s to get rid of this warning"
        << " assume sub-page to be << mtd.subpage_size";
  }

  // get MTD device file descriptor
  auto mode = O_RDWR;
  auto create_c_style_fd_result =
      CreateCStyleFileHandle(mtd_device_file_name, mode);
  if (create_c_style_fd_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "CreateCStyleFileHandle failed! error code = "
                       << int(create_c_style_fd_result.error())
                       << "mtd_device_file_name=" << mtd_device_file_name
                       << "mode=" << mode;
    return folly::makeUnexpected(create_c_style_fd_result.error());
  }
  format_attr.node_fd = create_c_style_fd_result.value().GetValue();

  // validate the mtd file is writable
  if (!mtd.writable) {
    SKL_LOG(SKL_ERROR) << mtd_device_file_name << " is a read-only device";
    return folly::makeUnexpected(
        ErrorCode::FORMAT__MTD_DEVICE_IS_A_READ_ONLY_DEVICE_ERROR);
  }

  // get UBI lib file descriptor
  auto get_ubi_lib_fd_result = CreateUbiLibFileHandle();
  if (get_ubi_lib_fd_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "CreateUbiLibFileHandle failed! error code = "
                       << int(get_ubi_lib_fd_result.error());
    return folly::makeUnexpected(get_ubi_lib_fd_result.error());
  }
  libubi_t lib_ubi_fd = get_ubi_lib_fd_result.value().GetValue();

  int ubi_dev_num;

  // check that the mtd device is not already attached to a UBI device
  ret = mtd_num2ubi_dev(lib_ubi_fd, mtd.mtd_num, &ubi_dev_num);
  if (!ret) {
    SKL_LOG(SKL_ERROR) << "mtd" << mtd.mtd_num << "is already attached to ubi"
                       << ubi_dev_num << " and needs to be detached first";
    return folly::makeUnexpected(
        ErrorCode::FORMAT__MTD_DEVICE_IS_ALREADY_ATTACHED_TO_UBI_DEVICE_ERROR);
  }

  SKL_LOG(SKL_INFO) << "\n**** UBI Formatting mtd" << mtd_num << "****";

  struct ubi_scan_info* si;

  // scan ubi
  ret = ubi_scan(&mtd, format_attr.node_fd, &si, 0 /*verbose*/);
  if (ret) {
    SKL_LOG(SKL_ERROR) << "ubi_scan failed! failed to scan mtd" << mtd.mtd_num;
    return folly::makeUnexpected(ErrorCode::FORMAT__UBI_SCAN_FAILURE_ERROR);
  }

  // erase blocks check
  if (si->good_cnt == 0) {
    SKL_LOG(SKL_ERROR) << "ubi_scan mtd" << mtd.mtd_num << " all "
                       << si->bad_cnt << " eraseblocks are bad";
    return folly::makeUnexpected(
        ErrorCode::FORMAT__BAD_ERASEBLOCKS_AFTER_SCAN_ERROR);
  }

  if (si->good_cnt < 2) {
    SKL_LOG(SKL_ERROR) << "ubi_scan mtd" << mtd.mtd_num
                       << " too few non-bad eraseblocks=" << si->good_cnt;
    return folly::makeUnexpected(
        ErrorCode::FORMAT__TOO_FEW_NON_BAD_ERASE_BLOCKS_AFTER_SCAN_ERROR);
  }

  if (si->alien_cnt) {
    SKL_LOG(SKL_WARNING) << si->alien_cnt << " of " << si->good_cnt
                         << " eraseblocks contain non-UBI data";
  }

  if (si->empty_cnt < si->good_cnt) {
    int percent = (si->ok_cnt * 100) / si->good_cnt;
    /*
     * Make sure the majority of eraseblocks have valid
     * erase counters.
     */
    if (percent < 50) {
      SKL_LOG(SKL_WARNING)
          << "only " << si->ok_cnt << " of " << si->good_cnt
          << " eraseblocks have valid erase counter. erase counter 0 "
             "will be used for all eraseblocks";

      format_attr.ec = 0;
      format_attr.override_ec = 1;
    } else if (percent < 50) {
      SKL_LOG(SKL_WARNING)
          << "only " << si->ok_cnt << " of " << si->good_cnt
          << " eraseblocks have valid erase counter. erase counter 0 "
             "will be used for all eraseblocks. mean erase counter "
             "%lld will be used for the rest of eraseblock "
          << si->mean_ec;
      format_attr.ec = si->mean_ec;
      format_attr.override_ec = 1;
    }
  }

  struct ubigen_info ui;
  ubigen_info_init(&ui, mtd.eb_size, mtd.min_io_size, mtd.subpage_size,
                   format_attr.vid_hdr_offs, format_attr.ubi_ver,
                   format_attr.image_seq);

  if (si->vid_hdr_offs != -1 && ui.vid_hdr_offs != si->vid_hdr_offs) {
    /*
     * what we read from flash and what we calculated using
     * min. I/O unit size and sub-page size differs.
     */
    SKL_LOG(SKL_WARNING) << "VID header and data offsets on flash are "
                         << si->vid_hdr_offs << " and " << si->data_offs
                         << " which is different to requested offsets "
                         << ui.vid_hdr_offs << " and " << ui.data_offs
                         << ". use new offsets " << ui.vid_hdr_offs << " and "
                         << ui.data_offs;

    ubigen_info_init(&ui, mtd.eb_size, mtd.min_io_size, 0, si->vid_hdr_offs,
                     format_attr.ubi_ver, format_attr.image_seq);
  }

  auto do_format_result = FormatExec(lib_mtd_fd, &mtd, &ui, si, 0, format_attr);
  if (do_format_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "FormatExec failed! error code = "
                       << int(do_format_result.error());
    return folly::makeUnexpected(do_format_result.error());
  }

  return folly::unit;
}

// FormatExec
folly::Expected<folly::Unit, UbiDevice::ErrorCode> UbiDevice::FormatExec(
    libmtd_t lib_mtd_fd, const struct mtd_dev_info* mtd,
    const struct ubigen_info* ui, ubi_scan_info* si, int start_eb,
    const struct FormatAttr& format_attr) {
  auto ret = 0;
  auto eb1 = -1, eb2 = -1;
  long long ec1 = -1, ec2 = -1;

  auto write_size = UBI_EC_HDR_SIZE + mtd->subpage_size - 1;
  write_size /= mtd->subpage_size;
  write_size *= mtd->subpage_size;

  auto ptr = std::make_unique<uint8_t[]>(write_size);
  auto hdr = reinterpret_cast<struct ubi_ec_hdr*>(ptr.get());

  std::memset(hdr, 0xFF, write_size);

  for (int eb = start_eb; eb < mtd->eb_cnt; eb++) {
    long long ec;

    if (si->ec[eb] == EB_BAD) {
      continue;
    }

    if (format_attr.override_ec) {
      ec = format_attr.ec;
    } else if (si->ec[eb] <= EC_MAX) {
      ec = si->ec[eb] + 1;
    } else {
      ec = si->mean_ec;
    }
    ubigen_init_ec_hdr(ui, hdr, ec);

    ret = mtd_erase(lib_mtd_fd, mtd, format_attr.node_fd, eb);

    if (ret) {
      SKL_LOG(SKL_ERROR) << "failed to erase eraseblock=" << eb << "ret=" << ret
                         << "errno=" << errno;
      if (errno != EIO) {
        return folly::makeUnexpected(
            ErrorCode::FORMAT__FAILED_TO_ERASE_ERASEBLOCK_ERROR);
      }

      auto mark_bad_result = MarkBadBlocks(mtd, si, eb, format_attr.node_fd);
      if (mark_bad_result.hasError()) {
        SKL_LOG(SKL_ERROR) << "MarkBadBlocks failed! error code = "
                           << int(mark_bad_result.error()) << " eb=" << eb;
        return folly::makeUnexpected(ErrorCode::FORMAT__MARK_BAD_FAILED_ERROR);
      }

      continue;
    }

    if ((eb1 == -1 || eb2 == -1)) {
      if (eb1 == -1) {
        eb1 = eb;
        ec1 = ec;
      } else if (eb2 == -1) {
        eb2 = eb;
        ec2 = ec;
      }
      continue;
    }

    ret = mtd_write(lib_mtd_fd, mtd, format_attr.node_fd, eb, 0, hdr,
                    write_size, NULL, 0, 0);

    if (ret) {
      SKL_LOG(SKL_ERROR) << "cannot write EC header (" << write_size
                         << " bytes buffer) to eraseblock " << eb
                         << "ret=" << ret << "errno=" << errno;

      if (errno != EIO) {
        if (format_attr.subpage_size != mtd->min_io_size) {
          SKL_LOG(SKL_ERROR) << "may be sub-page size is incorrect?";
          return folly::makeUnexpected(
              ErrorCode::FORMAT__CANNOT_WRITE_EC_HEADER);
        }
      }
      ret = mtd_torture(lib_mtd_fd, mtd, format_attr.node_fd, eb);
      if (ret) {
        auto mark_bad_result = MarkBadBlocks(mtd, si, eb, format_attr.node_fd);
        if (mark_bad_result.hasError()) {
          SKL_LOG(SKL_ERROR) << "MarkBadBlocks failed! error code = "
                             << int(mark_bad_result.error()) << " eb=" << eb;
          return folly::makeUnexpected(
              ErrorCode::FORMAT__MARK_BAD_FAILED_ERROR);
        }
      }
      continue;
    }
  }

  if (eb1 == -1 || eb2 == -1) {
    SKL_LOG(SKL_ERROR) << "no eraseblocks for volume table";
    return folly::makeUnexpected(
        ErrorCode::FORMAT__NO_ERASEBLOCKS_FOR_VOLUME_TABLE_ERROR);
  }

  struct ubi_vtbl_record* vtbl = ubigen_create_empty_vtbl(ui);
  if (!vtbl) {
    SKL_LOG(SKL_ERROR) << "ubigen_create_empty_vtbl failed!";
    return folly::makeUnexpected(
        ErrorCode::FORMAT__UBIGEN_CREATE_EMPTY_VTBL_ERROR);
  }

  MallocUniquePtr<struct ubi_vtbl_record> vtbl_unique_ptr(vtbl);

  ret = ubigen_write_layout_vol(ui, eb1, eb2, ec1, ec2, vtbl_unique_ptr.get(),
                                format_attr.node_fd);

  if (ret) {
    SKL_LOG(SKL_ERROR) << "cannot write layout volume";
    return folly::makeUnexpected(ErrorCode::FORMAT__CANNOT_WRITE_LAYOUT_VOLUME);
  }

  return folly::unit;
}

// MarkBadBlocks
folly::Expected<folly::Unit, UbiDevice::ErrorCode> UbiDevice::MarkBadBlocks(
    const struct mtd_dev_info* mtd, struct ubi_scan_info* si, int eb,
    int mtd_device_fd) {
  auto ret = 0;

  if (!mtd->bb_allowed) {
    SKL_LOG(SKL_ERROR) << "bad blocks not supported by this flash";
    return folly::makeUnexpected(
        ErrorCode::FORMAT__BAD_BLOCK_NOT_SUPPORTED_BY_THIS_FLASH_ERROR);
  }

  ret = mtd_mark_bad(mtd, mtd_device_fd, eb);
  if (ret) {
    SKL_LOG(SKL_ERROR) << "mtd_mark_bad failed! ret=" << ret << "eb=" << eb;
    return folly::makeUnexpected(ErrorCode::FORMAT__MTD_MARK_BAD_FAILED_ERROR);
  }

  si->bad_cnt += 1;
  si->ec[eb] = EB_BAD;

  auto consecutive_bad_check_result = ConsecutiveBadBlocksCheck(eb);
  if (consecutive_bad_check_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "ConsecutiveBadBlocksCheck failed! error code = "
                       << int(consecutive_bad_check_result.error())
                       << " eb=" << eb;
    return folly::makeUnexpected(
        ErrorCode::FORMAT__CONSECUTIVE_BAD_CHECK_ERROR);
  }
  return folly::unit;
}

// ConsecutiveBadBlocksCheck
folly::Expected<folly::Unit, UbiDevice::ErrorCode>
UbiDevice::ConsecutiveBadBlocksCheck(int eb) {
  static int consecutive_bad_blocks = 1;
  static int prev_bb = -1;

  if (prev_bb == -1) {
    prev_bb = eb;
  }

  if (eb == prev_bb + 1) {
    consecutive_bad_blocks += 1;
  } else {
    consecutive_bad_blocks = 1;
  }

  prev_bb = eb;

  if (consecutive_bad_blocks >= kMaxConsecutiveBadBlocks) {
    SKL_LOG(SKL_ERROR) << "consecutive bad blocks exceed limit: "
                       << kMaxConsecutiveBadBlocks << " bad flash?";
    return folly::makeUnexpected(
        ErrorCode::FORMAT__CONSECUTIVE_BAD_BLOCKS_EXCEED_LIMIT_ERROR);
  }

  return folly::unit;
}

// UpdateVolume
folly::Expected<folly::Unit, int32_t> UbiDevice::UpdateVolume(
    const std::string& vol_name, const std::string& ubifs_image_file_str,
    uint32_t skip_bytes, uint32_t size) {
  int ret = 0;

  // check that image file exists
  std::ifstream f(ubifs_image_file_str);
  if (!f.good()) {
    SKL_LOG(SKL_ERROR) << ubifs_image_file_str << " not exist!";
    return folly::makeUnexpected(
        int(ErrorCode::UPDATE_VOL__UBIFS_IMAGE_FILE_NOT_EXIST_ERROR));
  }

  // get UBI lib file descriptor
  auto get_ubi_lib_fd_result = CreateUbiLibFileHandle();
  if (get_ubi_lib_fd_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "CreateUbiLibFileHandle failed! error code = "
                       << int(get_ubi_lib_fd_result.error());
    return folly::makeUnexpected(int(get_ubi_lib_fd_result.error()));
  }
  libubi_t lib_ubi_fd = get_ubi_lib_fd_result.value().GetValue();

  // ubi probe node
  auto ubi_probe_node_result = UbiProbeNode(lib_ubi_fd, ubi_device_file_name_);
  if (ubi_probe_node_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "UbiProbeNode failed! error code = "
                       << int(ubi_probe_node_result.error())
                       << "ubi_device_file_name_=" << ubi_device_file_name_;
    return folly::makeUnexpected(int(ErrorCode::UBI_PROBE_NODE_FAILED_ERROR));
  }

  struct ubi_dev_info dev_info;
  struct ubi_vol_info vol_info;

  ret = ubi_get_dev_info(lib_ubi_fd, ubi_device_file_name_.c_str(), &dev_info);
  if (ret) {
    SKL_LOG(SKL_ERROR)
        << "ubi_get_dev_info failed! cannot get information about UBI "
           "device.ubi_device_file_name_ = "
        << ubi_device_file_name_ << " ret=" << ret;
    return folly::makeUnexpected(
        int(ErrorCode::
                REMOVE_VOLUME__CANNOT_FIND_INFORMATION_ABOUT_UBI_DEVICE_ERROR));
  }

  ret = ubi_get_vol_info1_nm(lib_ubi_fd, dev_info.dev_num, vol_name.c_str(),
                             &vol_info);
  if (ret) {
    SKL_LOG(SKL_ERROR)
        << "ubi_get_vol_info1_nm failed! cannot find UBI volume. UBI device="
        << ubi_device_file_name_ << " dev_num=" << dev_info.dev_num
        << " ret=" << ret;
    return folly::makeUnexpected(int(ErrorCode::CANNOT_FIND_UBI_VOLUME_ERROR));
  }

  std::string ubi_volume_file_name = std::move(
      folly::sformat("{}_{}", ubi_device_file_name_, vol_info.vol_id));

  SKL_LOG(SKL_INFO) << "\n**** UBI updating volume " << vol_name << " ("
                    << ubi_volume_file_name << ") ****";

  auto buf = std::make_unique<char[]>(vol_info.leb_size);

  // calc the amount of bytes to update
  long long bytes = 0;

  if (size > 0) {
    bytes = size;
  } else {
    struct stat st;
    ret = stat(ubifs_image_file_str.c_str(), &st);
    if (ret < 0) {
      SKL_LOG(SKL_ERROR) << "stat failed on " << ubifs_image_file_str;
      return folly::makeUnexpected(
          int(ErrorCode::UPDATE_VOL__STAT_FAILED_ERROR));
    }

    bytes = st.st_size - skip_bytes;
  }

  if (bytes > vol_info.rsvd_bytes) {
    SKL_LOG(SKL_ERROR) << ubifs_image_file_str << " size=" << bytes
                       << " will not fit volume=" << ubi_volume_file_name
                       << " size= " << vol_info.rsvd_bytes;

    return folly::makeUnexpected(int(ErrorCode::UPDATE_VOL__NO_SPACE_ERROR));
  }

  // create fd for ubi volume file
  auto mode = O_RDWR;
  auto create_ubi_vol_fd_result =
      CreateCStyleFileHandle(ubi_volume_file_name, mode);
  if (create_ubi_vol_fd_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "CreateCStyleFileHandle failed! error code = "
                       << int(create_ubi_vol_fd_result.error())
                       << "ubi_volume_file_name=" << ubi_volume_file_name
                       << "mode=" << mode;
    return folly::makeUnexpected(int(create_ubi_vol_fd_result.error()));
  }
  int fd_vol = create_ubi_vol_fd_result.value().GetValue();

  // create fd for ubifs image file
  mode = O_RDONLY;
  auto create_ubifs_image_fd_result =
      CreateCStyleFileHandle(ubifs_image_file_str, mode);
  if (create_ubifs_image_fd_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "CreateCStyleFileHandle failed! error code = "
                       << int(create_ubifs_image_fd_result.error())
                       << "ubifs_image_file_str=" << ubifs_image_file_str
                       << "mode=" << mode;
    return folly::makeUnexpected(int(create_ubifs_image_fd_result.error()));
  }
  int fd_image = create_ubifs_image_fd_result.value().GetValue();

  if (skip_bytes > 0) {
    if (lseek(fd_image, skip_bytes, SEEK_CUR) == -1) {
      SKL_LOG(SKL_ERROR) << "lseek input by " << skip_bytes << " failed!";
      return folly::makeUnexpected(
          int(ErrorCode::UPDATE_VOL__LSEEK_ON_IMAGE_FD_FAILED_ERROR));
    }
  }

  // start volume
  ret = ubi_update_start(lib_ubi_fd, fd_vol, bytes);
  if (ret) {
    SKL_LOG(SKL_ERROR) << "ubi_update_start failed! cannot start volume "
                       << ubi_volume_file_name << " update";
    return folly::makeUnexpected(
        int(ErrorCode::UPDATE_VOL__CANNOT_CANNOT_START_VOLUME_ERROR));
  }

  int sav_bytes = bytes;  // for info log

  // write UBIFS image to ubi volume
  while (bytes) {
    ssize_t size;
    int to_copy = min(vol_info.leb_size, bytes);

    size = read(fd_image, buf.get(), to_copy);

    if (size <= 0) {
      if (errno == EINTR) {
        SKL_LOG(SKL_ERROR) << "do not interrupt me!";
        continue;
      } else {
        SKL_LOG(SKL_ERROR) << "cannot read " << to_copy << " bytes from "
                           << ubifs_image_file_str;
        return folly::makeUnexpected(int(
            ErrorCode::UPDATE_VOL__CANNOT_READ_FROM_UBIFS_IMAGE_FILE_ERROR));
      }
    }

    auto ubi_write_result =
        UbiWrite(fd_vol, buf.get(), size, ubi_volume_file_name);
    if (ubi_write_result.hasError()) {
      SKL_LOG(SKL_ERROR) << "UbiWrite failed! error code = "
                         << int(ubi_write_result.error()) << "size=" << size
                         << "fd_vol=" << fd_vol;
      return folly::makeUnexpected(
          int(ErrorCode::UPDATE_VOL__UBI_WRITE_FAILED_ERROR));
    }
    bytes -= size;
  }

  SKL_LOG(SKL_INFO) << "UBI update volume operation finished successfully"
                    << " ubi volume file name=" << ubi_volume_file_name
                    << " ubifs image file name=" << ubifs_image_file_str
                    << " image file size=" << sav_bytes
                    << " volume reserved bytes=" << vol_info.rsvd_bytes;

  return folly::unit;
}

// UbiWrite
folly::Expected<folly::Unit, UbiDevice::ErrorCode> UbiDevice::UbiWrite(
    int fd, char* buf, ssize_t size, const std::string& ubi_volume_file_name) {
  int ret_size;

  while (size) {
    ret_size = write(fd, buf, size);
    if (ret_size < 0) {
      if (errno == EINTR) {
        SKL_LOG(SKL_WARNING) << "do not interrupt me!";
        continue;
      }

      SKL_LOG(SKL_ERROR) << "cannot write " << size << " bytes to volume "
                         << ubi_volume_file_name;
      return folly::makeUnexpected(ErrorCode::UBI_WRITE_FAILED_ERROR);
    }

    if (ret_size == 0) {
      SKL_LOG(SKL_ERROR) << "cannot write " << size << " bytes to volume "
                         << ubi_volume_file_name;
      return folly::makeUnexpected(ErrorCode::UBI_WRITE_FAILED_ERROR);
    }

    size -= ret_size;
    buf += ret_size;
  }

  return folly::unit;
}

// GetUbiVolumeFile
folly::Expected<std::string, UbiDevice::ErrorCode> UbiDevice::GetUbiVolumeFile(
    std::string vol_name) {
  int ret = 0;

  // get UBI lib file descriptor
  auto get_ubi_lib_fd_result = CreateUbiLibFileHandle();
  if (get_ubi_lib_fd_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "CreateUbiLibFileHandle failed! error code = "
                       << int(get_ubi_lib_fd_result.error());
    return folly::makeUnexpected(get_ubi_lib_fd_result.error());
  }
  libubi_t lib_ubi_fd = get_ubi_lib_fd_result.value().GetValue();

  // ubi probe node
  auto ubi_probe_node_result = UbiProbeNode(lib_ubi_fd, ubi_device_file_name_);
  if (ubi_probe_node_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "UbiProbeNode failed! error code = "
                       << int(ubi_probe_node_result.error())
                       << "ubi_device_file_name_=" << ubi_device_file_name_;
    return folly::makeUnexpected(ErrorCode::UBI_PROBE_NODE_FAILED_ERROR);
  }

  struct ubi_dev_info dev_info;
  struct ubi_vol_info vol_info;

  ret = ubi_get_dev_info(lib_ubi_fd, ubi_device_file_name_.c_str(), &dev_info);
  if (ret) {
    SKL_LOG(SKL_ERROR)
        << "ubi_get_dev_info failed! cannot get information about UBI "
           "device.ubi_device_file_name_ = "
        << ubi_device_file_name_ << " ret=" << ret;
    return folly::makeUnexpected(
        ErrorCode::
            REMOVE_VOLUME__CANNOT_FIND_INFORMATION_ABOUT_UBI_DEVICE_ERROR);
  }

  ret = ubi_get_vol_info1_nm(lib_ubi_fd, dev_info.dev_num, vol_name.c_str(),
                             &vol_info);
  if (ret) {
    SKL_LOG(SKL_ERROR)
        << "ubi_get_vol_info1_nm failed! cannot find UBI volume. UBI device="
        << ubi_device_file_name_ << " dev_num=" << dev_info.dev_num
        << " ret=" << ret;
    return folly::makeUnexpected(ErrorCode::CANNOT_FIND_UBI_VOLUME_ERROR);
  }

  return std::move(
      folly::sformat("{}_{}", ubi_device_file_name_, vol_info.vol_id));
}

// UbiProbeNode
folly::Expected<folly::Unit, UbiDevice::ErrorCode> UbiDevice::UbiProbeNode(
    libubi_t lib_ubi_fd, const std::string& ubi_device_file_name,
    bool is_to_print_log_error) {
  int ret = 0;

  ret = ubi_probe_node(lib_ubi_fd, ubi_device_file_name_.c_str());
  if (ret == 2) {
    if (is_to_print_log_error) {
      SKL_LOG(SKL_ERROR) << "ubi_probe_node failed! " << ubi_device_file_name_
                         << " is an UBI volume node, not an UBI device node";
    }
    return folly::makeUnexpected(ErrorCode::NOT_AN_UBI_DEVICE_NODE_ERROR);
  } else if (ret < 0) {
    if (errno == ENODEV) {
      if (is_to_print_log_error) {
        SKL_LOG(SKL_ERROR) << "ubi_probe_node failed! " << ubi_device_file_name_
                           << " is not an UBI device node. ret=" << ret;
      }
      return folly::makeUnexpected(ErrorCode::NOT_AN_UBI_DEVICE_NODE_ERROR);
    } else {
      if (is_to_print_log_error) {
        SKL_LOG(SKL_ERROR) << "ubi_probe_node failed! "
                           << " ubi_device_file_name_=" << ubi_device_file_name_
                           << " ret=" << ret;
      }
      return folly::makeUnexpected(ErrorCode::PROBE_NODE_ERROR);
    }
  }

  return folly::unit;
}

// MountVolume
folly::Expected<folly::Unit, int32_t> UbiDevice::MountVolume(
    const std::string& vol_name, const std::string& dir_to_mount) {
  auto get_ubi_vol_file_result = GetUbiVolumeFile(vol_name);
  if (get_ubi_vol_file_result.hasError()) {
    SKL_LOG(SKL_ERROR) << "GetUbiVolumeFile failed! error code = "
                       << int(get_ubi_vol_file_result.error())
                       << "vol_name=" << vol_name;
    return folly::makeUnexpected(int(get_ubi_vol_file_result.error()));
  }

  std::string ubi_volume_file_name = std::move(get_ubi_vol_file_result.value());

  int ret = mount(ubi_volume_file_name.c_str(), dir_to_mount.c_str(), "ubifs",
                  0, NULL);
  if (ret) {
    SKL_LOG(SKL_ERROR) << "mount failed! "
                       << " ubi_volume_full_file_name_=" << ubi_volume_file_name
                       << " dir_to_mount=" << dir_to_mount
                       << " errno=" << errno;
    return folly::makeUnexpected(
        int(ErrorCode::MOUNT_VOLUME__MOUNT_FAILED_ERROR));
  }
  return folly::unit;
}

// UnmountVolume
folly::Expected<folly::Unit, int32_t> UbiDevice::UnmountVolume(
    const std::string& dir_to_unmount) {
  int ret = 0;

  ret = umount(dir_to_unmount.c_str());
  if (ret) {
    SKL_LOG(SKL_ERROR) << "umount failed! "
                       << "dir_to_unmount=" << dir_to_unmount
                       << "errno=" << errno;
    return folly::makeUnexpected(
        int(ErrorCode::UNMOUNT_VOLUME__UMOUNT_FAILED_ERROR));
  }

  return folly::unit;
}
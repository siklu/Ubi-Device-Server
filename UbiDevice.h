/*
 * Copyright 2020 by Siklu Ltd. All rights reserved.
 */

#pragma once

#include <MtdTable.h>
#include <folly/Expected.h>
#include <libmtd.h>
#include <libubi.h>

#include <string>

#include "IUbiDevice.h"

/**
 * @brief A C++ wrapper class for UBI lib operations
 *
 * MTD device can be attached/detach to/from an UBI device
 * When MTD device is attached to an UBI device, an UBI volume can be created
 * based on the attached MTD device (MakeVolume).
 * RemoveVolume() will remove the created volume.
 * Mtd is attached to UBI at the object creation and detached at destruction
 * In Create there is an option for doing UBI format before the object creation.
 *
 * The class code uses static libraries of mtd-util (e.g. libubi.a, libscan.a)
 * The code itself is strongly based on open source executables in mtd-utils:
 *
 * ubiformat.c for Format
 * ubiattach.c for Attach
 * ubidetach.c for Detach
 * ubimkvol.c for MakeVolume
 * ubiemvol.c for RemoveVolume
 *
 * The motivation was not to call those executables directly (via system OS
 * call)
 *
 */

class UbiDevice : public IUbiDevice {
 public:
  // TBD - instead taking this constants from here - take it from the building
  // ubifs image source (currently they are duplicated)
  static constexpr int kUbifsMinimumIOUnitSize = 4096;
  static constexpr int kUbifsLogicalEraseBlockSize = 253952;
  static constexpr int kUbifsMaximumLogicalEraseBlockCount = 924;

  /**
   * @brief Construct a new Ubi Wrapper object (move constructor)
   *
   * @param other
   */
  UbiDevice(UbiDevice&& other);

  /**
   * @brief move assignment operator.
   *
   * @param other
   * @return IUbiDevice&
   */
  UbiDevice& operator=(UbiDevice&& other);

  /**
   * @brief - Create object of type IUbiDevice
   *
   * @param mtd_device_name - mtd device name (e.g, "first_bank")
   * @return object of type UbiDevice or error code
   */
  static folly::Expected<std::shared_ptr<IUbiDevice>, int32_t> Create(
      const std::string& mtd_device_name, bool is_to_format_first = false);

  /**
   * @brief format the UBI volume. (as an optional preperation before the UBI
   * object creation - hence, this function is static)
   *
   * @param mtd_num - mtd number
   * @return error code
   */
  static folly::Expected<folly::Unit, ErrorCode> Format(
      MtdTable::MtdNum mtd_num);

  /**
   * @brief make ubi volume
   *
   * @param vol_name  - volume name (e.g rootfs)
   * @param size_in_bytes - size of valume. 0 means max available size
   * @return error code
   */
  folly::Expected<folly::Unit, int32_t> MakeVolume(
      const std::string& vol_name, uint32_t size_in_bytes = 0) override;
  /**
   * @brief remove UBI volume
   *
   * @param vol_name - UBI volume name
   * @param is_to_print_log_error - is to print error log in case of failures
   * explantion: sometimes the call to  RemoveVolume is only to clean possible
   * existing volume before calling MakeVolume. in this case it is ok to fail
   * and there is need to print error to the log
   * @return error code
   */
  folly::Expected<folly::Unit, int32_t> RemoveVolume(
      const std::string& vol_name, bool is_to_print_log_error = true) override;

  /**
   * @brief - write ubifs image to ubi volume
   *
   * @param vol_name - UBI volume name
   * @param ubifs_image_file_str - UBIFS image file name
   * @param skip_bytes - leading bytes to skip from input file - default is 0
   * @param size - bytes to read from input. default 0 means until the end of
   * file
   * @return error code
   */
  folly::Expected<folly::Unit, int32_t> UpdateVolume(
      const std::string& vol_name, const std::string& ubifs_image_file_str,
      uint32_t skip_bytes = 0, uint32_t size = 0) override;

  /**
   * @brief Get the Ubi Volume File by volume name
   *
   * @param vol_name - e.g, "rootfs"
   * @return volume file (e.g, /dev/ubi2_0) or error code
   */
  folly::Expected<std::string, ErrorCode> GetUbiVolumeFile(
      std::string vol_name) override;

  /**
   * @brief mount a volume
   *
   * @param vol_name - volume name
   * @param dir_to_mount - dir to nount. e.g. /tmp/mnt
   * @return error code
   */
  folly::Expected<folly::Unit, int32_t> MountVolume(
      const std::string& vol_name, const std::string& dir_to_mount) override;

  /**
   * @brief unmount volume
   *
   * @param dir_to_unmount - the mount target dir
   * @return error code
   */
  folly::Expected<folly::Unit, int32_t> UnmountVolume(
      const std::string& dir_to_unmount) override;

  folly::Expected<folly::Unit, int32_t> Format(void) override;

  /**
   * @brief Attach MTD device to the UBI device
   *
   * @return error code
   */
  folly::Expected<folly::Unit, int32_t> Attach() override;

  /**
   * @brief Detach MTD device from the UBI device
   *
   * @return error code
   */
  folly::Expected<folly::Unit, int32_t> Detach() override;

  /**
   * @brief Destroy the Ubi Wrapper object
   *
   */
  ~UbiDevice();

  // disallow copy constructor
  UbiDevice(const UbiDevice&) = delete;

  // disallow assignment operator
  UbiDevice& operator=(const UbiDevice&) = delete;

 private:
  /**
   * @brief structure with UBI format attributes
   *
   */
  struct FormatAttr {
    unsigned int override_ec = 0;
    int subpage_size = 0;
    int vid_hdr_offs = 0;
    int ubi_ver = 1;
    uint32_t image_seq = 0;
    long long ec = 0;
    int node_fd = 0;
  };

  /*
   * constructor
   * @param: mtd_num  - mtd number
   * note: the constructor is private - create object of this class only with
   * Create()
   */
  UbiDevice(int mtd_num);

  /**
   * @brief execute the UBI format (internal function which is called from
   * Format)
   *
   * @param libmtd - lib mtd file descriptor
   * @param mtd - mtd_info
   * @param ui - ubigen_info
   * @param si - ubi scan info
   * @param start_eb - start eraseblock
   * @param mtd_device_fd - mtd device file descriptor
   * @return error code
   */
  static folly::Expected<folly::Unit, UbiDevice::ErrorCode> FormatExec(
      libmtd_t lib_mtd_fd, const struct mtd_dev_info* mtd,
      const struct ubigen_info* ui, struct ubi_scan_info* si, int start_eb,
      const struct FormatAttr& format_attr);

  /**
   * @brief - mark bad blocks (internal format operation)
   *
   * @param mtd - mtd_info
   * @param si - ubi scan info
   * @param eb - eraseblock
   * @return error code
   */
  static folly::Expected<folly::Unit, UbiDevice::ErrorCode> MarkBadBlocks(
      const struct mtd_dev_info* mtd, struct ubi_scan_info* si, int eb,
      int mtd_device_fd);

  /**
   * @brief - check consecutive bad blocks - they must not exceed limit.
   * (internal format operation)
   *
   * @param eb - eradeblock
   * @return error code
   */
  static folly::Expected<folly::Unit, UbiDevice::ErrorCode>
  ConsecutiveBadBlocksCheck(int eb);

  /**
   * @brief Check that the kernel is fresh enough for Attach/Detach feature
   *
   * @param lib_ubi_fd - descriptor for UBI lib
   * @return error code
   */
  folly::Expected<folly::Unit, ErrorCode>
  CheckKernelSupportForAttachDetachRequest(libubi_t lib_ubi_fd);

  /**
   * @brief - test UBI node (whether the node is a UBI device or volume node)
   *
   * @param lib_ubi_fd - descriptor for UBI lib
   * @param ubi_device_file_name - file name of UBI device
   * @param is_to_print_log_error - is to print log error
   * @return error code
   */
  folly::Expected<folly::Unit, ErrorCode> UbiProbeNode(
      libubi_t lib_ubi_fd, const std::string& ubi_device_file_name,
      bool is_to_print_log_error = true);

  /**
   * @brief - write data to UBI volume file
   *
   * @param fd - file descriptor to ubi volume device
   * @param buf - data buffer to write
   * @param size - size to write from beginning of buffer
   * @param ubi_volume_file_name - to ubi volume file name (needed for logging)
   * @return error code
   */
  folly::Expected<folly::Unit, ErrorCode> UbiWrite(
      int fd, char* buf, ssize_t size, const std::string& ubi_volume_file_name);

  // true if the ubi device is attached to mtd
  bool is_attached_;

  // mtd number
  MtdTable::MtdNum mtd_num_;

  // ubi device file name (e.g. /dev/ubi0, /dev/ubi1 etc.)
  std::string ubi_device_file_name_;
};

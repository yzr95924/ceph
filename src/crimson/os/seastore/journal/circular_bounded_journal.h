// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include "crimson/common/log.h"

#include <boost/intrusive_ptr.hpp>

#include <seastar/core/future.hh>

#include "include/ceph_assert.h"
#include "include/buffer.h"
#include "include/denc.h"

#include "crimson/osd/exceptions.h"
#include "crimson/os/seastore/journal.h"
#include "include/uuid.h"
#include "crimson/os/seastore/random_block_manager.h"
#include "crimson/os/seastore/random_block_manager/rbm_device.h"
#include <list>

namespace crimson::os::seastore::journal {

constexpr rbm_abs_addr CBJOURNAL_START_ADDRESS = 0;
constexpr uint64_t CBJOURNAL_MAGIC = 0xCCCC;
using RBMDevice = random_block_device::RBMDevice;

/**
 * CircularBoundedJournal
 *
 * 
 * CircularBoundedJournal (CBJournal) is the journal that works like circular
 * queue. With CBJournal, Seastore will append some of the records if the size
 * of the record is small (most likely metadata), at which point the head
 * (written_to) will be moved. Then, eventually, Seastore applies the records
 * in CBjournal to RBM (TODO).
 *
 * - Commit time
 * After submit_record is done, written_to is increased(this in-memory value)
 * ---written_to represents where the new record will be appended. Note that
 * applied_to is not changed here.
 *
 * - Replay time
 * At replay time, CBJournal begins to replay records in CBjournal by reading
 * records from dirty_tail. Then, CBJournal examines whether the records is valid
 * one by one, at which point written_to is recovered
 * if the valid record is founded. Note that applied_to is stored
 * permanently when the apply work---applying the records in CBJournal to RBM---
 * is done by CBJournal (TODO).
 *
 * TODO: apply records from CircularBoundedJournal to RandomBlockManager
 *
 */

constexpr uint64_t DEFAULT_SIZE = 1 << 26;
constexpr uint64_t DEFAULT_BLOCK_SIZE = 4096;

class CircularBoundedJournal : public Journal {
public:
  struct mkfs_config_t {
    std::string path;
    size_t block_size = 0;
    size_t total_size = 0;
    device_id_t device_id = 0;
    seastore_meta_t meta;
    static mkfs_config_t get_default() {
      device_id_t d_id = 1 << (std::numeric_limits<device_id_t>::digits - 1);
      return mkfs_config_t {
	"",
	DEFAULT_BLOCK_SIZE,
	DEFAULT_SIZE,
	d_id,
	seastore_meta_t {}
      };
    }
  };

  CircularBoundedJournal(
      JournalTrimmer &trimmer, RBMDevice* device, const std::string &path);
  ~CircularBoundedJournal() {}

  JournalTrimmer &get_trimmer() final {
    return trimmer;
  }

  open_for_mkfs_ret open_for_mkfs() final;

  open_for_mount_ret open_for_mount() final;

  close_ertr::future<> close() final;

  journal_type_t get_type() final {
    return journal_type_t::RANDOM_BLOCK;
  }

  submit_record_ret submit_record(
    record_t &&record,
    OrderingHandle &handle
  ) final;

  seastar::future<> flush(
    OrderingHandle &handle
  ) final {
    // TODO
    return seastar::now();
  }

  replay_ret replay(delta_handler_t &&delta_handler) final;

  struct cbj_header_t;
  using write_ertr = submit_record_ertr;
  /*
   * device_write_bl
   *
   * @param device address to write
   * @param bufferlist to write
   *
   */
  write_ertr::future<> device_write_bl(rbm_abs_addr offset, ceph::bufferlist &bl);

  using read_ertr = crimson::errorator<
    crimson::ct_error::input_output_error,
    crimson::ct_error::invarg,
    crimson::ct_error::enoent,
    crimson::ct_error::erange>;
  using read_record_ertr = read_ertr;
  using read_record_ret = read_record_ertr::future<
	std::optional<std::pair<record_group_header_t, bufferlist>>
	>;
  using read_header_ertr = read_ertr;
  using read_header_ret = read_header_ertr::future<
	std::optional<std::pair<cbj_header_t, bufferlist>>
	>;
  /*
   * read_record
   *
   * read record from given address
   *
   * @param paddr_t to read
   * @param expected_seq
   *
   */
  read_record_ret read_record(paddr_t offset, segment_seq_t expected_seq);
  /*
   * read_header
   *
   * read header block from given absolute address
   *
   * @param absolute address
   *
   */
  read_header_ret read_header();

  ceph::bufferlist encode_header();

  using mkfs_ertr = crimson::errorator<
    crimson::ct_error::input_output_error,
    crimson::ct_error::invarg
  >;
  using mkfs_ret = mkfs_ertr::future<>;

  /*
   * mkfs
   *
   * make a new journal layout even if old journal exists
   *
   * @param mkfs_config_t
   *
   */
  mkfs_ret mkfs(const mkfs_config_t& config);


  /**
   * CircularBoundedJournal structure
   *
   * +-------------------------------------------------------+
   * |   header    | record | record | record | record | ... |
   * +-------------------------------------------------------+
   *               ^-----------block aligned-----------------^
   * <----fixed---->
   */


  /**
   *
   * CircularBoundedJournal write
   *
   * NVMe will support a large block write (< 512KB) with atomic write unit command.
   * With this command, we expect that the most of incoming data can be stored
   * as a single write call, which has lower overhead than existing
   * way that uses a combination of system calls such as write() and sync().
   *
   */

  struct cbj_header_t {
    uint64_t magic = CBJOURNAL_MAGIC;
    uuid_d uuid;
    uint64_t block_size = 0; // block size of underlying device
    uint64_t size = 0;   // max length of journal

    // start offset of CircularBoundedJournal in the device
    journal_seq_t dirty_tail;
    journal_seq_t alloc_tail;

    device_id_t device_id;

    DENC(cbj_header_t, v, p) {
      DENC_START(1, 1, p);
      denc(v.magic, p);
      denc(v.uuid, p);
      denc(v.block_size, p);
      denc(v.size, p);

      denc(v.dirty_tail, p);
      denc(v.alloc_tail, p);

      denc(v.device_id, p);

      DENC_FINISH(p);
    }
  };

  /**
   *
   * Write position for CircularBoundedJournal
   *
   * | written to rbm |    written length to CircularBoundedJournal    | new write |
   * ----------------->------------------------------------------------>
   *                  ^      	                                       ^
   *            applied_to                                        written_to
   *
   */

  size_t get_used_size() const {
    auto rbm_written_to = get_rbm_addr(get_written_to());
    auto rbm_tail = get_rbm_addr(get_dirty_tail());
    return rbm_written_to >= rbm_tail ?
      rbm_written_to - rbm_tail :
      rbm_written_to + header.size + get_block_size()
      - rbm_tail;
  }
  size_t get_total_size() const {
    return header.size;
  }
  rbm_abs_addr get_start_addr() const {
    return CBJOURNAL_START_ADDRESS + get_block_size();
  }
  size_t get_available_size() const {
    return get_total_size() - get_used_size();
  }

  seastar::future<> update_journal_tail(
    journal_seq_t dirty,
    journal_seq_t alloc) {
    header.dirty_tail = dirty;
    header.alloc_tail = alloc;
    return write_header(
    ).handle_error(
      crimson::ct_error::assert_all{
      "encountered invalid error in update_journal_tail"
    });
  }
  journal_seq_t get_dirty_tail() const {
    return header.dirty_tail;
  }
  journal_seq_t get_alloc_tail() const {
    return header.alloc_tail;
  }

  write_ertr::future<> write_header();

  read_record_ret return_record(record_group_header_t& header, bufferlist bl);

  void set_write_pipeline(WritePipeline *_write_pipeline) final {
    write_pipeline = _write_pipeline;
  }

  journal_seq_t get_written_to() const {
    return written_to;
  }
  rbm_abs_addr get_rbm_addr(journal_seq_t seq) const {
    return convert_paddr_to_abs_addr(seq.offset);
  }
  void set_written_to(journal_seq_t seq) {
    rbm_abs_addr addr = convert_paddr_to_abs_addr(seq.offset);
    assert(addr >= get_start_addr());
    assert(addr < get_journal_end());
    written_to = seq;
  }
  device_id_t get_device_id() const {
    return header.device_id;
  }
  extent_len_t get_block_size() const {
    return header.block_size;
  }
  rbm_abs_addr get_journal_end() const {
    return get_start_addr() + header.size + get_block_size(); // journal size + header length
  }
  seastar::future<> finish_commit(transaction_type_t type) final;
private:
  cbj_header_t header;
  JournalTrimmer &trimmer;
  RBMDevice* device;
  std::string path;
  WritePipeline *write_pipeline = nullptr;
  /**
   * initialized
   *
   * true after open_device_read_header, set to false in close().
   * Indicates that device is open and in-memory header is valid.
   */
  bool initialized = false;

  // start address where the newest record will be written
  // should be in range [get_start_addr(), get_journal_end())
  // written_to.segment_seq is circulation seq to track 
  // the sequence to written records
  journal_seq_t written_to;
};

std::ostream &operator<<(std::ostream &out, const CircularBoundedJournal::cbj_header_t &header);

}

WRITE_CLASS_DENC_BOUNDED(crimson::os::seastore::journal::CircularBoundedJournal::cbj_header_t)

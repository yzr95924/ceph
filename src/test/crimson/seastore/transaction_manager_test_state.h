// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <random>
#include <boost/iterator/counting_iterator.hpp>

#include "crimson/os/seastore/cache.h"
#include "crimson/os/seastore/extent_placement_manager.h"
#include "crimson/os/seastore/logging.h"
#include "crimson/os/seastore/transaction_manager.h"
#include "crimson/os/seastore/segment_manager/ephemeral.h"
#include "crimson/os/seastore/seastore.h"
#include "crimson/os/seastore/segment_manager.h"
#include "crimson/os/seastore/collection_manager/flat_collection_manager.h"
#include "crimson/os/seastore/onode_manager/staged-fltree/fltree_onode_manager.h"
#include "crimson/os/seastore/random_block_manager/rbm_device.h"
#include "crimson/os/seastore/journal/circular_bounded_journal.h"

using namespace crimson;
using namespace crimson::os;
using namespace crimson::os::seastore;

class EphemeralTestState {
protected:
  segment_manager::EphemeralSegmentManagerRef segment_manager;
  std::list<segment_manager::EphemeralSegmentManagerRef> secondary_segment_managers;
  std::unique_ptr<random_block_device::RBMDevice> rb_device;
  journal_type_t journal_type;

  EphemeralTestState(std::size_t num_segment_managers) {
    assert(num_segment_managers > 0);
    secondary_segment_managers.resize(num_segment_managers - 1);
  }

  std::size_t get_num_devices() const {
    return secondary_segment_managers.size() + 1;
  }

  virtual void _init() = 0;

  virtual void _destroy() = 0;
  virtual seastar::future<> _teardown() = 0;
  seastar::future<> teardown() {
    return _teardown().then([this] {
      _destroy();
    });
  }


  virtual FuturizedStore::mkfs_ertr::future<> _mkfs() = 0;
  virtual FuturizedStore::mount_ertr::future<> _mount() = 0;

  seastar::future<> restart_fut() {
    LOG_PREFIX(EphemeralTestState::restart_fut);
    SUBINFO(test, "begin ...");
    return teardown().then([this] {
      segment_manager->remount();
      for (auto &sec_sm : secondary_segment_managers) {
        sec_sm->remount();
      }
      _init();
      return _mount().handle_error(crimson::ct_error::assert_all{});
    }).then([FNAME] {
      SUBINFO(test, "finish");
    });
  }

  void restart() {
    restart_fut().get0();
  }

  seastar::future<> segment_setup()
  {
    LOG_PREFIX(EphemeralTestState::segment_setup);
    segment_manager = segment_manager::create_test_ephemeral();
    for (auto &sec_sm : secondary_segment_managers) {
      sec_sm = segment_manager::create_test_ephemeral();
    }
    return segment_manager->init(
    ).safe_then([this] {
      return crimson::do_for_each(
        secondary_segment_managers.begin(),
        secondary_segment_managers.end(),
        [](auto &sec_sm)
      {
        return sec_sm->init();
      });
    }).safe_then([this] {
      return segment_manager->mkfs(
        segment_manager::get_ephemeral_device_config(0, get_num_devices()));
    }).safe_then([this] {
      return seastar::do_with(std::size_t(0), [this](auto &cnt) {
        return crimson::do_for_each(
          secondary_segment_managers.begin(),
          secondary_segment_managers.end(),
          [this, &cnt](auto &sec_sm)
        {
          ++cnt;
          return sec_sm->mkfs(
            segment_manager::get_ephemeral_device_config(cnt, get_num_devices()));
        });
      });
    }).safe_then([this] {
      _init();
      return _mkfs();
    }).safe_then([this] {
      return restart_fut();
    }).handle_error(
      crimson::ct_error::assert_all{}
    ).then([FNAME] {
      SUBINFO(test, "finish");
    });
  }

  seastar::future<> randomblock_setup()
  {
    auto config =
      journal::CircularBoundedJournal::mkfs_config_t::get_default();
    rb_device.reset(new random_block_device::TestMemory(
          config.total_size + config.block_size));
    rb_device->set_device_id(
      1 << (std::numeric_limits<device_id_t>::digits - 1));
    return rb_device->mount().handle_error(crimson::ct_error::assert_all{}
    ).then([this]() {
      return segment_setup();
    });
  }

  seastar::future<> tm_setup(
    journal_type_t type = journal_type_t::SEGMENTED) {
    LOG_PREFIX(EphemeralTestState::tm_setup);
    SUBINFO(test, "begin with {} devices ...", get_num_devices());
    journal_type = type;
    // FIXME: should not initialize segment_manager with circularbounded-journal
    if (journal_type == journal_type_t::SEGMENTED) {
      return segment_setup();
    } else {
      assert(journal_type == journal_type_t::RANDOM_BLOCK);
      return randomblock_setup();
    }
  }

  seastar::future<> tm_teardown() {
    LOG_PREFIX(EphemeralTestState::tm_teardown);
    SUBINFO(test, "begin");
    return teardown().then([this, FNAME] {
      segment_manager.reset();
      for (auto &sec_sm : secondary_segment_managers) {
        sec_sm.reset();
      }
      rb_device.reset();
      SUBINFO(test, "finish");
    });
  }
};

class TMTestState : public EphemeralTestState {
protected:
  TransactionManagerRef tm;
  LBAManager *lba_manager;
  Cache* cache;
  ExtentPlacementManager *epm;
  uint64_t seq = 0;

  TMTestState() : EphemeralTestState(1) {}

  TMTestState(std::size_t num_devices) : EphemeralTestState(num_devices) {}

  virtual void _init() override {
    std::vector<Device*> sec_devices;
    for (auto &sec_sm : secondary_segment_managers) {
      sec_devices.emplace_back(sec_sm.get());
    }
    if (journal_type == journal_type_t::RANDOM_BLOCK) {
      // FIXME: should not initialize segment_manager with circularbounded-journal
      // FIXME: no secondary device in the single device test
      sec_devices.emplace_back(segment_manager.get());
      tm = make_transaction_manager(rb_device.get(), sec_devices, true);
    } else {
      tm = make_transaction_manager(segment_manager.get(), sec_devices, true);
    }
    epm = tm->get_epm();
    lba_manager = tm->get_lba_manager();
    cache = tm->get_cache();
  }

  virtual void _destroy() override {
    epm = nullptr;
    lba_manager = nullptr;
    cache = nullptr;
    tm.reset();
  }

  virtual seastar::future<> _teardown() {
    return tm->close().safe_then([this] {
      _destroy();
      return seastar::now();
    }).handle_error(
      crimson::ct_error::assert_all{"Error in teardown"}
    );
  }

  virtual FuturizedStore::mount_ertr::future<> _mount() {
    return tm->mount(
    ).handle_error(
      crimson::ct_error::assert_all{"Error in mount"}
    ).then([this] {
      return epm->stop_background();
    }).then([this] {
      return epm->run_background_work_until_halt();
    });
  }

  virtual FuturizedStore::mkfs_ertr::future<> _mkfs() {
    if (journal_type == journal_type_t::SEGMENTED) {
      return tm->mkfs(
      ).handle_error(
	crimson::ct_error::assert_all{"Error in mkfs"}
      );
    } else {
      auto config = journal::CircularBoundedJournal::mkfs_config_t::get_default();
      return static_cast<journal::CircularBoundedJournal*>(tm->get_journal())->mkfs(
	config
      ).safe_then([this]() {
	return tm->mkfs(
	).handle_error(
	  crimson::ct_error::assert_all{"Error in mkfs"}
	);
      }).handle_error(
	crimson::ct_error::assert_all{"Error in mkfs"}
      );
    }
  }

  auto create_mutate_transaction() {
    return tm->create_transaction(
        Transaction::src_t::MUTATE, "test_mutate");
  }

  auto create_read_transaction() {
    return tm->create_transaction(
        Transaction::src_t::READ, "test_read");
  }

  auto create_weak_transaction() {
    return tm->create_transaction(
        Transaction::src_t::READ, "test_read_weak", true);
  }

  auto submit_transaction_fut2(Transaction& t) {
    return tm->submit_transaction(t);
  }

  auto submit_transaction_fut(Transaction &t) {
    return with_trans_intr(
      t,
      [this](auto &t) {
	return tm->submit_transaction(t);
      });
  }
  auto submit_transaction_fut_with_seq(Transaction &t) {
    using ertr = TransactionManager::base_iertr;
    return with_trans_intr(
      t,
      [this](auto &t) {
	return tm->submit_transaction(t
	).si_then([this] {
	  return ertr::make_ready_future<uint64_t>(seq++);
	});
      });
  }

  void submit_transaction(TransactionRef t) {
    submit_transaction_fut(*t).unsafe_get0();
    epm->run_background_work_until_halt().get0();
  }
};

class TestSegmentManagerWrapper final : public SegmentManager {
  SegmentManager &sm;
  device_id_t device_id = 0;
  secondary_device_set_t set;
public:
  TestSegmentManagerWrapper(
    SegmentManager &sm,
    device_id_t device_id = 0)
    : sm(sm), device_id(device_id) {}

  device_id_t get_device_id() const {
    return device_id;
  }

  mount_ret mount() final {
    return mount_ertr::now(); // we handle this above
  }

  mkfs_ret mkfs(device_config_t c) final {
    return mkfs_ertr::now(); // we handle this above
  }

  close_ertr::future<> close() final {
    return sm.close();
  }

  secondary_device_set_t& get_secondary_devices() final {
    return sm.get_secondary_devices();
  }

  magic_t get_magic() const final {
    return sm.get_magic();
  }

  open_ertr::future<SegmentRef> open(segment_id_t id) final {
    return sm.open(id);
  }

  release_ertr::future<> release(segment_id_t id) final {
    return sm.release(id);
  }

  read_ertr::future<> read(
    paddr_t addr, size_t len, ceph::bufferptr &out) final {
    return sm.read(addr, len, out);
  }

  size_t get_available_size() const final { return sm.get_available_size(); }
  extent_len_t get_block_size() const final { return sm.get_block_size(); }
  segment_off_t get_segment_size() const final {
    return sm.get_segment_size();
  }
  const seastore_meta_t &get_meta() const final {
    return sm.get_meta();
  }
  ~TestSegmentManagerWrapper() final {}
};

class SeaStoreTestState : public EphemeralTestState {
  class TestMDStoreState {
    std::map<std::string, std::string> md;
    public:
    class Store final : public SeaStore::MDStore {
      TestMDStoreState &parent;
    public:
      Store(TestMDStoreState &parent) : parent(parent) {}

      write_meta_ret write_meta(
	const std::string& key, const std::string& value) final {
	parent.md[key] = value;
	return seastar::now();
      }

      read_meta_ret read_meta(const std::string& key) final {
	auto iter = parent.md.find(key);
	if (iter != parent.md.end()) {
	  return read_meta_ret(
	    read_meta_ertr::ready_future_marker{},
	    iter->second);
	} else {
	  return read_meta_ret(
	    read_meta_ertr::ready_future_marker{},
	    std::nullopt);
	}
      }
    };
    Store get_mdstore() {
      return Store(*this);
    }
  } mdstore_state;

protected:
  std::unique_ptr<SeaStore> seastore;

  SeaStoreTestState() : EphemeralTestState(1) {}

  virtual void _init() final {
    seastore = make_test_seastore(
      std::make_unique<TestSegmentManagerWrapper>(*segment_manager),
      std::make_unique<TestMDStoreState::Store>(mdstore_state.get_mdstore()));
  }

  virtual void _destroy() final {
    seastore.reset();
  }

  virtual seastar::future<> _teardown() final {
    return seastore->umount();
  }

  virtual FuturizedStore::mount_ertr::future<> _mount() final {
    return seastore->mount();
  }

  virtual FuturizedStore::mkfs_ertr::future<> _mkfs() final {
    return seastore->mkfs(uuid_d{});
  }
};

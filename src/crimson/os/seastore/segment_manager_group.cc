// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab expandtab

#include "crimson/os/seastore/segment_manager_group.h"

#include "crimson/os/seastore/logging.h"

SET_SUBSYS(seastore_journal);

namespace crimson::os::seastore {

SegmentManagerGroup::read_segment_tail_ret
SegmentManagerGroup::read_segment_tail(segment_id_t segment)
{
  assert(has_device(segment.device_id()));
  auto& segment_manager = *segment_managers[segment.device_id()];
  return segment_manager.read(
    paddr_t::make_seg_paddr(
      segment,
      segment_manager.get_segment_size() - get_rounded_tail_length()),
    get_rounded_tail_length()
  ).handle_error(
    read_segment_header_ertr::pass_further{},
    crimson::ct_error::assert_all{
      "Invalid error in SegmentManagerGroup::read_segment_tail"
    }
  ).safe_then([=, &segment_manager](bufferptr bptr) -> read_segment_tail_ret {
    LOG_PREFIX(SegmentManagerGroup::read_segment_tail);
    DEBUG("segment {} bptr size {}", segment, bptr.length());

    segment_tail_t tail;
    bufferlist bl;
    bl.push_back(bptr);

    DEBUG("segment {} block crc {}",
          segment,
          bl.begin().crc32c(segment_manager.get_block_size(), 0));

    auto bp = bl.cbegin();
    try {
      decode(tail, bp);
    } catch (ceph::buffer::error &e) {
      DEBUG("segment {} unable to decode tail, skipping -- {}",
            segment, e);
      return crimson::ct_error::enodata::make();
    }
    DEBUG("segment {} tail {}", segment, tail);
    return read_segment_tail_ret(
      read_segment_tail_ertr::ready_future_marker{},
      tail);
  });
}

SegmentManagerGroup::read_segment_header_ret
SegmentManagerGroup::read_segment_header(segment_id_t segment)
{
  assert(has_device(segment.device_id()));
  auto& segment_manager = *segment_managers[segment.device_id()];
  return segment_manager.read(
    paddr_t::make_seg_paddr(segment, 0),
    get_rounded_header_length()
  ).handle_error(
    read_segment_header_ertr::pass_further{},
    crimson::ct_error::assert_all{
      "Invalid error in SegmentManagerGroup::read_segment_header"
    }
  ).safe_then([=, &segment_manager](bufferptr bptr) -> read_segment_header_ret {
    LOG_PREFIX(SegmentManagerGroup::read_segment_header);
    DEBUG("segment {} bptr size {}", segment, bptr.length());

    segment_header_t header;
    bufferlist bl;
    bl.push_back(bptr);

    DEBUG("segment {} block crc {}",
          segment,
          bl.begin().crc32c(segment_manager.get_block_size(), 0));

    auto bp = bl.cbegin();
    try {
      decode(header, bp);
    } catch (ceph::buffer::error &e) {
      DEBUG("segment {} unable to decode header, skipping -- {}",
            segment, e);
      return crimson::ct_error::enodata::make();
    }
    DEBUG("segment {} header {}", segment, header);
    return read_segment_header_ret(
      read_segment_header_ertr::ready_future_marker{},
      header);
  });
}

SegmentManagerGroup::scan_valid_records_ret
SegmentManagerGroup::scan_valid_records(
  scan_valid_records_cursor &cursor,
  segment_nonce_t nonce,
  size_t budget,
  found_record_handler_t &handler)
{
  LOG_PREFIX(SegmentManagerGroup::scan_valid_records);
  assert(has_device(cursor.get_segment_id().device_id()));
  auto& segment_manager =
    *segment_managers[cursor.get_segment_id().device_id()];
  if (cursor.get_segment_offset() == 0) {
    INFO("start to scan segment {}", cursor.get_segment_id());
    cursor.increment_seq(segment_manager.get_block_size());
  }
  DEBUG("starting at {}, budget={}", cursor, budget);
  auto retref = std::make_unique<size_t>(0);
  auto &budget_used = *retref;
  return crimson::repeat(
    [=, &cursor, &budget_used, &handler, this]() mutable
    -> scan_valid_records_ertr::future<seastar::stop_iteration> {
      return [=, &handler, &cursor, &budget_used, this] {
	if (!cursor.last_valid_header_found) {
	  return read_validate_record_metadata(cursor.seq.offset, nonce
	  ).safe_then([=, &cursor](auto md) {
	    if (!md) {
	      cursor.last_valid_header_found = true;
	      if (cursor.is_complete()) {
	        INFO("complete at {}, invalid record group metadata",
                     cursor);
	      } else {
	        DEBUG("found invalid record group metadata at {}, "
	              "processing {} pending record groups",
	              cursor.seq,
	              cursor.pending_record_groups.size());
	      }
	      return scan_valid_records_ertr::now();
	    } else {
	      auto& [header, md_bl] = *md;
	      DEBUG("found valid {} at {}", header, cursor.seq);
	      cursor.emplace_record_group(header, std::move(md_bl));
	      return scan_valid_records_ertr::now();
	    }
	  }).safe_then([=, &cursor, &budget_used, &handler, this] {
	    DEBUG("processing committed record groups until {}, {} pending",
		  cursor.last_committed,
		  cursor.pending_record_groups.size());
	    return crimson::repeat(
	      [=, &budget_used, &cursor, &handler, this] {
		if (cursor.pending_record_groups.empty()) {
		  /* This is only possible if the segment is empty.
		   * A record's last_commited must be prior to its own
		   * location since it itself cannot yet have been committed
		   * at its own time of submission.  Thus, the most recently
		   * read record must always fall after cursor.last_committed */
		  return scan_valid_records_ertr::make_ready_future<
		    seastar::stop_iteration>(seastar::stop_iteration::yes);
		}
		auto &next = cursor.pending_record_groups.front();
		journal_seq_t next_seq = {cursor.seq.segment_seq, next.offset};
		if (cursor.last_committed == JOURNAL_SEQ_NULL ||
		    next_seq > cursor.last_committed) {
		  return scan_valid_records_ertr::make_ready_future<
		    seastar::stop_iteration>(seastar::stop_iteration::yes);
		}
		return consume_next_records(cursor, handler, budget_used
		).safe_then([] {
		  return scan_valid_records_ertr::make_ready_future<
		    seastar::stop_iteration>(seastar::stop_iteration::no);
		});
	      });
	  });
	} else {
	  assert(!cursor.pending_record_groups.empty());
	  auto &next = cursor.pending_record_groups.front();
	  return read_validate_data(next.offset, next.header
	  ).safe_then([this, FNAME, &budget_used, &cursor, &handler, &next](auto valid) {
	    if (!valid) {
	      INFO("complete at {}, invalid record group data at {}, {}",
		   cursor, next.offset, next.header);
	      cursor.pending_record_groups.clear();
	      return scan_valid_records_ertr::now();
	    }
            return consume_next_records(cursor, handler, budget_used);
	  });
	}
      }().safe_then([=, &budget_used, &cursor] {
	if (cursor.is_complete() || budget_used >= budget) {
	  DEBUG("finish at {}, budget_used={}, budget={}",
                cursor, budget_used, budget);
	  return seastar::stop_iteration::yes;
	} else {
	  return seastar::stop_iteration::no;
	}
      });
    }).safe_then([retref=std::move(retref)]() mutable -> scan_valid_records_ret {
      return scan_valid_records_ret(
	scan_valid_records_ertr::ready_future_marker{},
	std::move(*retref));
    });
}

SegmentManagerGroup::read_validate_record_metadata_ret
SegmentManagerGroup::read_validate_record_metadata(
  paddr_t start,
  segment_nonce_t nonce)
{
  LOG_PREFIX(SegmentManagerGroup::read_validate_record_metadata);
  auto& seg_addr = start.as_seg_paddr();
  assert(has_device(seg_addr.get_segment_id().device_id()));
  auto& segment_manager = *segment_managers[seg_addr.get_segment_id().device_id()];
  auto block_size = segment_manager.get_block_size();
  auto segment_size = static_cast<int64_t>(segment_manager.get_segment_size());
  if (seg_addr.get_segment_off() + block_size > segment_size) {
    DEBUG("failed -- record group header block {}~4096 > segment_size {}", start, segment_size);
    return read_validate_record_metadata_ret(
      read_validate_record_metadata_ertr::ready_future_marker{},
      std::nullopt);
  }
  TRACE("reading record group header block {}~4096", start);
  return segment_manager.read(start, block_size
  ).safe_then([=, &segment_manager](bufferptr bptr) mutable
              -> read_validate_record_metadata_ret {
    auto block_size = segment_manager.get_block_size();
    bufferlist bl;
    bl.append(bptr);
    auto maybe_header = try_decode_records_header(bl, nonce);
    if (!maybe_header.has_value()) {
      return read_validate_record_metadata_ret(
        read_validate_record_metadata_ertr::ready_future_marker{},
        std::nullopt);
    }
    auto& seg_addr = start.as_seg_paddr();
    auto& header = *maybe_header;
    if (header.mdlength < block_size ||
        header.mdlength % block_size != 0 ||
        header.dlength % block_size != 0 ||
        (header.committed_to != JOURNAL_SEQ_NULL &&
         header.committed_to.offset.as_seg_paddr().get_segment_off() % block_size != 0) ||
        (seg_addr.get_segment_off() + header.mdlength + header.dlength > segment_size)) {
      ERROR("failed, invalid record group header {}", start);
      return crimson::ct_error::input_output_error::make();
    }
    if (header.mdlength == block_size) {
      return read_validate_record_metadata_ret(
        read_validate_record_metadata_ertr::ready_future_marker{},
        std::make_pair(std::move(header), std::move(bl))
      );
    }

    auto rest_start = paddr_t::make_seg_paddr(
        seg_addr.get_segment_id(),
        seg_addr.get_segment_off() + block_size
    );
    auto rest_len = header.mdlength - block_size;
    TRACE("reading record group header rest {}~{}", rest_start, rest_len);
    return segment_manager.read(rest_start, rest_len
    ).safe_then([header=std::move(header), bl=std::move(bl)
                ](auto&& bptail) mutable {
      bl.push_back(bptail);
      return read_validate_record_metadata_ret(
        read_validate_record_metadata_ertr::ready_future_marker{},
        std::make_pair(std::move(header), std::move(bl)));
    });
  }).safe_then([](auto p) {
    if (p && validate_records_metadata(p->second)) {
      return read_validate_record_metadata_ret(
        read_validate_record_metadata_ertr::ready_future_marker{},
        std::move(*p)
      );
    } else {
      return read_validate_record_metadata_ret(
        read_validate_record_metadata_ertr::ready_future_marker{},
        std::nullopt);
    }
  });
}

SegmentManagerGroup::read_validate_data_ret
SegmentManagerGroup::read_validate_data(
  paddr_t record_base,
  const record_group_header_t &header)
{
  LOG_PREFIX(SegmentManagerGroup::read_validate_data);
  assert(has_device(record_base.get_device_id()));
  auto& segment_manager = *segment_managers[record_base.get_device_id()];
  auto data_addr = record_base.add_offset(header.mdlength);
  TRACE("reading record group data blocks {}~{}", data_addr, header.dlength);
  return segment_manager.read(
    data_addr,
    header.dlength
  ).safe_then([=, &header](auto bptr) {
    bufferlist bl;
    bl.append(bptr);
    return validate_records_data(header, bl);
  });
}

SegmentManagerGroup::consume_record_group_ertr::future<>
SegmentManagerGroup::consume_next_records(
  scan_valid_records_cursor& cursor,
  found_record_handler_t& handler,
  std::size_t& budget_used)
{
  LOG_PREFIX(SegmentManagerGroup::consume_next_records);
  auto& next = cursor.pending_record_groups.front();
  auto total_length = next.header.dlength + next.header.mdlength;
  budget_used += total_length;
  auto locator = record_locator_t{
    next.offset.add_offset(next.header.mdlength),
    write_result_t{
      journal_seq_t{
        cursor.seq.segment_seq,
        next.offset
      },
      total_length
    }
  };
  DEBUG("processing {} at {}, budget_used={}",
        next.header, locator, budget_used);
  return handler(
    locator,
    next.header,
    next.mdbuffer
  ).safe_then([FNAME, &cursor] {
    cursor.pop_record_group();
    if (cursor.is_complete()) {
      INFO("complete at {}, no more record group", cursor);
    }
  });
}

SegmentManagerGroup::find_journal_segment_headers_ret
SegmentManagerGroup::find_journal_segment_headers()
{
  return seastar::do_with(
    get_segment_managers(),
    find_journal_segment_headers_ret_bare{},
    [this](auto &sms, auto& ret) -> find_journal_segment_headers_ret
  {
    return crimson::do_for_each(sms,
      [this, &ret](SegmentManager *sm)
    {
      LOG_PREFIX(SegmentManagerGroup::find_journal_segment_headers);
      auto device_id = sm->get_device_id();
      auto num_segments = sm->get_num_segments();
      DEBUG("processing {} with {} segments",
            device_id_printer_t{device_id}, num_segments);
      return crimson::do_for_each(
        boost::counting_iterator<device_segment_id_t>(0),
        boost::counting_iterator<device_segment_id_t>(num_segments),
        [this, &ret, device_id](device_segment_id_t d_segment_id)
      {
        segment_id_t segment_id{device_id, d_segment_id};
        return read_segment_header(segment_id
        ).safe_then([segment_id, &ret](auto &&header) {
          if (header.get_type() == segment_type_t::JOURNAL) {
            ret.emplace_back(std::make_pair(segment_id, std::move(header)));
          }
        }).handle_error(
          crimson::ct_error::enoent::handle([](auto) {
            return find_journal_segment_headers_ertr::now();
          }),
          crimson::ct_error::enodata::handle([](auto) {
            return find_journal_segment_headers_ertr::now();
          }),
          crimson::ct_error::input_output_error::pass_further{}
        );
      });
    }).safe_then([&ret]() mutable {
      return find_journal_segment_headers_ret{
        find_journal_segment_headers_ertr::ready_future_marker{},
        std::move(ret)};
    });
  });
}

} // namespace crimson::os::seastore

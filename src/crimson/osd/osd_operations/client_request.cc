// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab expandtab

#include "messages/MOSDOp.h"
#include "messages/MOSDOpReply.h"

#include "crimson/common/exception.h"
#include "crimson/osd/pg.h"
#include "crimson/osd/osd.h"
#include "common/Formatter.h"
#include "crimson/osd/osd_operation_external_tracking.h"
#include "crimson/osd/osd_operations/client_request.h"
#include "crimson/osd/osd_connection_priv.h"

namespace {
  seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_osd);
  }
}

namespace crimson::osd {


void ClientRequest::Orderer::requeue(
  ShardServices &shard_services, Ref<PG> pg)
{
  for (auto &req: list) {
    logger().debug("{}: {} requeueing {}", __func__, *pg, req);
    req.reset_instance_handle();
    std::ignore = req.with_pg_int(shard_services, pg);
  }
}

void ClientRequest::Orderer::clear_and_cancel()
{
  for (auto i = list.begin(); i != list.end(); ) {
    logger().debug(
      "{}: ClientRequest::Orderer::clear_and_cancel {}",
      *i);
    i->complete_request();
    remove_request(*(i++));
  }
}

void ClientRequest::complete_request()
{
  track_event<CompletionEvent>();
  on_complete.set_value();
}

ClientRequest::ClientRequest(
  ShardServices &shard_services, crimson::net::ConnectionRef conn,
  Ref<MOSDOp> &&m)
  : put_historic_shard_services(&shard_services),
    conn(std::move(conn)),
    m(std::move(m)),
    instance_handle(new instance_handle_t)
{}

ClientRequest::~ClientRequest()
{
  logger().debug("{}: destroying", *this);
}

void ClientRequest::print(std::ostream &lhs) const
{
  lhs << "m=[" << *m << "]";
}

void ClientRequest::dump_detail(Formatter *f) const
{
  logger().debug("{}: dumping", *this);
  std::apply([f] (auto... event) {
    (..., event.dump(f));
  }, tracking_events);
}

ConnectionPipeline &ClientRequest::get_connection_pipeline()
{
  return get_osd_priv(conn.get()).client_request_conn_pipeline;
}

ConnectionPipeline &ClientRequest::cp()
{
  return get_osd_priv(conn.get()).client_request_conn_pipeline;
}

ClientRequest::PGPipeline &ClientRequest::pp(PG &pg)
{
  return pg.client_request_pg_pipeline;
}

bool ClientRequest::same_session_and_pg(const ClientRequest& other_op) const
{
  return &get_osd_priv(conn.get()) == &get_osd_priv(other_op.conn.get()) &&
         m->get_spg() == other_op.m->get_spg();
}

bool ClientRequest::is_pg_op() const
{
  return std::any_of(
    begin(m->ops), end(m->ops),
    [](auto& op) { return ceph_osd_op_type_pg(op.op.op); });
}

seastar::future<> ClientRequest::with_pg_int(
  ShardServices &shard_services, Ref<PG> pgref)
{
  epoch_t same_interval_since = pgref->get_interval_start_epoch();
  logger().debug("{} same_interval_since: {}", *this, same_interval_since);
  if (m->finish_decode()) {
    m->clear_payload();
  }
  const auto this_instance_id = instance_id++;
  OperationRef opref{this};
  auto instance_handle = get_instance_handle();
  auto &ihref = *instance_handle;
  return interruptor::with_interruption(
    [this, pgref, this_instance_id, &ihref, &shard_services]() mutable {
      PG &pg = *pgref;
      if (pg.can_discard_op(*m)) {
	return shard_services.send_incremental_map(
	  std::ref(*conn), m->get_map_epoch()
	).then([this, this_instance_id, pgref] {
	  logger().debug("{}.{}: discarding", *this, this_instance_id);
	  pgref->client_request_orderer.remove_request(*this);
	  complete_request();
	  return interruptor::now();
	});
      }
      return ihref.enter_stage<interruptor>(pp(pg).await_map, *this
      ).then_interruptible([this, this_instance_id, &pg, &ihref] {
	logger().debug("{}.{}: after await_map stage", *this, this_instance_id);
	return ihref.enter_blocker(
	  *this, pg.osdmap_gate, &decltype(pg.osdmap_gate)::wait_for_map,
	  m->get_min_epoch(), nullptr);
      }).then_interruptible([this, this_instance_id, &pg, &ihref](auto map) {
	logger().debug("{}.{}: after wait_for_map", *this, this_instance_id);
	return ihref.enter_stage<interruptor>(pp(pg).wait_for_active, *this);
      }).then_interruptible([this, this_instance_id, &pg, &ihref]() {
	logger().debug(
	  "{}.{}: after wait_for_active stage", *this, this_instance_id);
	return ihref.enter_blocker(
	  *this,
	  pg.wait_for_active_blocker,
	  &decltype(pg.wait_for_active_blocker)::wait);
      }).then_interruptible([this, pgref, this_instance_id, &ihref]() mutable
			    -> interruptible_future<> {
	logger().debug(
	  "{}.{}: after wait_for_active", *this, this_instance_id);
	if (is_pg_op()) {
	  return process_pg_op(pgref);
	} else {
	  return process_op(
	    ihref,
	    pgref
	  ).then_interruptible([](auto){});
	}
      }).then_interruptible([this, this_instance_id, pgref] {
	logger().debug("{}.{}: after process*", *this, this_instance_id);
	pgref->client_request_orderer.remove_request(*this);
	complete_request();
      });
    }, [this, this_instance_id, pgref](std::exception_ptr eptr) {
      // TODO: better debug output
      logger().debug("{}.{}: interrupted {}", *this, this_instance_id, eptr);
    }, pgref).finally(
      [opref=std::move(opref), pgref=std::move(pgref),
       instance_handle=std::move(instance_handle), &ihref] {
      ihref.handle.exit();
    });
}

seastar::future<> ClientRequest::with_pg(
  ShardServices &shard_services, Ref<PG> pgref)
{
  put_historic_shard_services = &shard_services;
  pgref->client_request_orderer.add_request(*this);
  auto ret = on_complete.get_future();
  std::ignore = with_pg_int(
    shard_services, std::move(pgref)
  );
  return ret;
}

ClientRequest::interruptible_future<>
ClientRequest::process_pg_op(
  Ref<PG> &pg)
{
  return pg->do_pg_ops(
    m
  ).then_interruptible([this, pg=std::move(pg)](MURef<MOSDOpReply> reply) {
    return conn->send(std::move(reply));
  });
}

ClientRequest::interruptible_future<ClientRequest::seq_mode_t>
ClientRequest::process_op(instance_handle_t &ihref, Ref<PG> &pg)
{
  return ihref.enter_stage<interruptor>(
    pp(*pg).recover_missing,
    *this
  ).then_interruptible(
    [this, pg]() mutable {
    return do_recover_missing(pg, m->get_hobj());
  }).then_interruptible([this, pg, &ihref]() mutable {
    return pg->already_complete(m->get_reqid()).then_interruptible(
      [this, pg, &ihref](auto completed) mutable
      -> PG::load_obc_iertr::future<seq_mode_t> {
      if (completed) {
        auto reply = crimson::make_message<MOSDOpReply>(
          m.get(), completed->err, pg->get_osdmap_epoch(),
          CEPH_OSD_FLAG_ACK | CEPH_OSD_FLAG_ONDISK, false);
	reply->set_reply_versions(completed->version, completed->user_version);
        return conn->send(std::move(reply)).then([] {
          return seastar::make_ready_future<seq_mode_t>(seq_mode_t::OUT_OF_ORDER);
        });
      } else {
        return ihref.enter_stage<interruptor>(pp(*pg).get_obc, *this
	).then_interruptible(
          [this, pg, &ihref]() mutable -> PG::load_obc_iertr::future<seq_mode_t> {
          logger().debug("{}: got obc lock", *this);
          op_info.set_from_op(&*m, *pg->get_osdmap());
          // XXX: `do_with()` is just a workaround for `with_obc_func_t` imposing
          // `future<void>`.
          return seastar::do_with(
	    seq_mode_t{},
	    [this, &pg, &ihref](seq_mode_t& mode) {
	      return pg->with_locked_obc(
		m->get_hobj(), op_info,
		[this, pg, &mode, &ihref](auto obc) mutable {
		  return ihref.enter_stage<interruptor>(pp(*pg).process, *this
		  ).then_interruptible(
		    [this, pg, obc, &mode, &ihref]() mutable {
		      return do_process(ihref, pg, obc
		      ).then_interruptible([&mode] (seq_mode_t _mode) {
			mode = _mode;
			return seastar::now();
		      });
		    });
		}).safe_then_interruptible([&mode] {
		  return PG::load_obc_iertr::make_ready_future<seq_mode_t>(mode);
		});
	    });
	  });
      }
      });
  }).safe_then_interruptible([pg=std::move(pg)] (const seq_mode_t mode) {
    return seastar::make_ready_future<seq_mode_t>(mode);
  }, PG::load_obc_ertr::all_same_way([](auto &code) {
    logger().error("ClientRequest saw error code {}", code);
    return seastar::make_ready_future<seq_mode_t>(seq_mode_t::OUT_OF_ORDER);
  }));
}

auto ClientRequest::reply_op_error(Ref<PG>& pg, int err)
{
  logger().debug("{}: replying with error {}", *this, err);
  auto reply = crimson::make_message<MOSDOpReply>(
    m.get(), err, pg->get_osdmap_epoch(),
    m->get_flags() & (CEPH_OSD_FLAG_ACK|CEPH_OSD_FLAG_ONDISK),
    !m->has_flag(CEPH_OSD_FLAG_RETURNVEC));
  reply->set_reply_versions(eversion_t(), 0);
  reply->set_op_returns(std::vector<pg_log_op_return_item_t>{});
  return conn->send(std::move(reply)).then([] {
    return seastar::make_ready_future<ClientRequest::seq_mode_t>
      (seq_mode_t::OUT_OF_ORDER);
  });
}

ClientRequest::interruptible_future<ClientRequest::seq_mode_t>
ClientRequest::do_process(
  instance_handle_t &ihref,
  Ref<PG>& pg, crimson::osd::ObjectContextRef obc)
{
  if (!pg->is_primary()) {
    // primary can handle both normal ops and balanced reads
    if (is_misdirected(*pg)) {
      logger().trace("do_process: dropping misdirected op");
      return seastar::make_ready_future<seq_mode_t>(seq_mode_t::OUT_OF_ORDER);
    } else if (const hobject_t& hoid = m->get_hobj();
               !pg->get_peering_state().can_serve_replica_read(hoid)) {
      return reply_op_error(pg, -EAGAIN);
    }
  }
  if (m->has_flag(CEPH_OSD_FLAG_PARALLELEXEC)) {
    return reply_op_error(pg, -EINVAL);
  }
  if (m->get_oid().name.size()
    > crimson::common::local_conf()->osd_max_object_name_len) {
    return reply_op_error(pg, -ENAMETOOLONG);
  } else if (m->get_hobj().get_key().size()
    > crimson::common::local_conf()->osd_max_object_name_len) {
    return reply_op_error(pg, -ENAMETOOLONG);
  } else if (m->get_hobj().nspace.size()
    > crimson::common::local_conf()->osd_max_object_namespace_len) {
    return reply_op_error(pg, -ENAMETOOLONG);
  } else if (m->get_hobj().oid.name.empty()) {
    return reply_op_error(pg, -EINVAL);
  }

  if (!obc->obs.exists && !op_info.may_write()) {
    return reply_op_error(pg, -ENOENT);
  }

  return pg->do_osd_ops(m, obc, op_info).safe_then_unpack_interruptible(
    [this, pg, &ihref](auto submitted, auto all_completed) mutable {
      return submitted.then_interruptible([this, pg, &ihref] {
	return ihref.enter_stage<interruptor>(pp(*pg).wait_repop, *this);
      }).then_interruptible(
	[this, pg, all_completed=std::move(all_completed), &ihref]() mutable {
	  return all_completed.safe_then_interruptible(
	    [this, pg, &ihref](MURef<MOSDOpReply> reply) {
	      return ihref.enter_stage<interruptor>(pp(*pg).send_reply, *this
	      ).then_interruptible(
		[this, reply=std::move(reply)]() mutable {
		  logger().debug("{}: sending response", *this);
		  return conn->send(std::move(reply)).then([] {
		    return seastar::make_ready_future<seq_mode_t>(seq_mode_t::IN_ORDER);
		  });
		});
	    }, crimson::ct_error::eagain::handle([this, pg, &ihref]() mutable {
	      return process_op(ihref, pg);
	    }));
	});
    }, crimson::ct_error::eagain::handle([this, pg, &ihref]() mutable {
      return process_op(ihref, pg);
    }));
}

bool ClientRequest::is_misdirected(const PG& pg) const
{
  // otherwise take a closer look
  if (const int flags = m->get_flags();
      flags & CEPH_OSD_FLAG_BALANCE_READS ||
      flags & CEPH_OSD_FLAG_LOCALIZE_READS) {
    if (!op_info.may_read()) {
      // no read found, so it can't be balanced read
      return true;
    }
    if (op_info.may_write() || op_info.may_cache()) {
      // write op, but i am not primary
      return true;
    }
    // balanced reads; any replica will do
    return pg.is_nonprimary();
  }
  // neither balanced nor localize reads
  return true;
}

void ClientRequest::put_historic() const
{
  ceph_assert_always(put_historic_shard_services);
  put_historic_shard_services->get_registry().put_historic(*this);
}

}

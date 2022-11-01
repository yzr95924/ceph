
import logging
import json

from teuthology.task import Task
from teuthology import misc

log = logging.getLogger(__name__)


class CheckCounter(Task):
    """
    Use this task to validate that some daemon perf counters were
    incremented by the nested tasks.

    Config:
     'cluster_name': optional, specify which cluster
     'target': dictionary of daemon type to list of performance counters.
     'dry_run': just log the value of the counters, don't fail if they
                aren't nonzero.

    Success condition is that for all of the named counters, at least
    one of the daemons of that type has the counter nonzero.

    Example to check cephfs dirfrag splits are happening:
    - install:
    - ceph:
    - ceph-fuse:
    - check-counter:
        counters:
            mds:
                - "mds.dir_split"
                -
                    name: "mds.dir_update"
                    min: 3
    - workunit: ...
    """

    def start(self):
        log.info("START")

    def end(self):
        overrides = self.ctx.config.get('overrides', {})
        misc.deep_merge(self.config, overrides.get('check-counter', {}))

        cluster_name = self.config.get('cluster_name', None)
        dry_run = self.config.get('dry_run', False)
        targets = self.config.get('counters', {})

        if cluster_name is None:
            cluster_name = next(iter(self.ctx.managers.keys()))

        for daemon_type, counters in targets.items():
            # List of 'a', 'b', 'c'...
            daemon_ids = list(misc.all_roles_of_type(self.ctx.cluster, daemon_type))
            daemons = dict([(daemon_id,
                             self.ctx.daemons.get_daemon(daemon_type, daemon_id))
                            for daemon_id in daemon_ids])

            expected = set()
            seen = set()

            for daemon_id, daemon in daemons.items():
                if not daemon.running():
                    log.info("Ignoring daemon {0}, it isn't running".format(daemon_id))
                    continue
                else:
                    log.debug("Getting stats from {0}".format(daemon_id))

                manager = self.ctx.managers[cluster_name]
                proc = manager.admin_socket(daemon_type, daemon_id, ["perf", "dump"])
                response_data = proc.stdout.getvalue().strip()
                if response_data:
                    perf_dump = json.loads(response_data)
                else:
                    log.warning("No admin socket response from {0}, skipping".format(daemon_id))
                    continue

                for counter in counters:
                    if isinstance(counter, dict):
                        name = counter['name']
                        minval = counter['min']
                    else:
                        name = counter
                        minval = 1
                    expected.add(name)

                    val = perf_dump
                    for key in name.split('.'):
                        if key not in val:
                            log.warning(f"Counter '{name}' not found on daemon {daemon_type}.{daemon_id}")
                            val = None
                            break

                        val = val[key]

                    if val is not None:
                        log.info(f"Daemon {daemon_type}.{daemon_id} {name}={val}")
                        if val >= minval:
                            seen.add(name)

            if not dry_run:
                unseen = set(expected) - set(seen)
                if unseen:
                    raise RuntimeError("The following counters failed to be set "
                                       "on {0} daemons: {1}".format(
                        daemon_type, unseen
                    ))

task = CheckCounter

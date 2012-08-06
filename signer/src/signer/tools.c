/*
 * $Id$
 *
 * Copyright (c) 2009 NLNet Labs. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * Zone signing tools.
 *
 */

#include "config.h"
#include "daemon/dnshandler.h"
#include "adapter/adapter.h"
#include "shared/file.h"
#include "shared/log.h"
#include "signer/tools.h"
#include "signer/zone.h"

#include <sys/types.h>
#include <sys/wait.h>

static const char* tools_str = "tools";


/**
 * Load zone signconf.
 *
 */
ods_status
tools_signconf(zone_type* zone)
{
    ods_status status = ODS_STATUS_OK;
    signconf_type* new_signconf = NULL;
    task_id denial_what = TASK_NONE;

    ods_log_assert(zone);
    ods_log_assert(zone->name);
    status = zone_load_signconf(zone, &new_signconf);
    if (status == ODS_STATUS_OK) {
        ods_log_assert(new_signconf);
        /* Denial of Existence Rollover? */
        denial_what = signconf_compare_denial(zone->signconf, new_signconf);
        if (denial_what == TASK_NSECIFY) {
            /* or NSEC -> NSEC3, or NSEC3 -> NSEC, or NSEC3PARAM changed */
            namedb_wipe_denial(zone->db);
            namedb_cleanup_denials(zone->db);
            namedb_init_denials(zone->db);
        }
        /* all ok, switch signer configuration */
        signconf_cleanup(zone->signconf);
        ods_log_debug("[%s] zone %s switch to new signconf", tools_str,
            zone->name);
        zone->signconf = new_signconf;
        signconf_log(zone->signconf, zone->name);
        zone->default_ttl = (uint32_t) duration2time(zone->signconf->soa_min);
    } else if (status != ODS_STATUS_UNCHANGED) {
        ods_log_error("[%s] unable to load signconf for zone %s: %s",
            tools_str, zone->name, ods_status2str(status));
    }
    return status;
}


/**
 * Read zone from input adapter.
 *
 */
ods_status
tools_input(zone_type* zone)
{
    ods_status status = ODS_STATUS_OK;
    time_t start = 0;
    time_t end = 0;

    ods_log_assert(zone);
    ods_log_assert(zone->name);
    ods_log_assert(zone->adinbound);
    ods_log_assert(zone->signconf);
    /* Key Rollover? */
    status = zone_publish_dnskeys(zone);
    if (status != ODS_STATUS_OK) {
        ods_log_error("[%s] unable to read zone %s: failed to "
            "publish dnskeys (%s)", tools_str, zone->name,
            ods_status2str(status));
        zone_rollback_dnskeys(zone);
        zone_rollback_nsec3param(zone);
        namedb_rollback(zone->db);
        return status;
    }
    /* Denial of Existence Rollover? */
    status = zone_publish_nsec3param(zone);
    if (status != ODS_STATUS_OK) {
        ods_log_error("[%s] unable to read zone %s: failed to "
            "publish nsec3param (%s)", tools_str, zone->name,
            ods_status2str(status));
        zone_rollback_dnskeys(zone);
        zone_rollback_nsec3param(zone);
        namedb_rollback(zone->db);
        return status;
    }

    if (zone->stats) {
        lock_basic_lock(&zone->stats->stats_lock);
        zone->stats->sort_done = 0;
        zone->stats->sort_count = 0;
        zone->stats->sort_time = 0;
        lock_basic_unlock(&zone->stats->stats_lock);
    }
    /* Input Adapter */
    start = time(NULL);
    status = adapter_read((void*)zone);
    if (status != ODS_STATUS_OK) {
        ods_log_error("[%s] unable to read zone %s: adapter failed (%s)",
            tools_str, zone->name, ods_status2str(status));
        zone_rollback_dnskeys(zone);
        zone_rollback_nsec3param(zone);
        namedb_rollback(zone->db);
    }
    end = time(NULL);
    if (status == ODS_STATUS_OK && zone->stats) {
        lock_basic_lock(&zone->stats->stats_lock);
        zone->stats->start_time = start;
        zone->stats->sort_time = (end-start);
        zone->stats->sort_done = 1;
        lock_basic_unlock(&zone->stats->stats_lock);
    }
    return status;
}


/**
 * Write zone to output adapter.
 *
 */
ods_status
tools_output(zone_type* zone, engine_type* engine)
{
    ods_status status = ODS_STATUS_OK;
    ods_log_assert(engine);
    ods_log_assert(engine->config);
    ods_log_assert(zone);
    ods_log_assert(zone->db);
    ods_log_assert(zone->name);
    ods_log_assert(zone->signconf);
    ods_log_assert(zone->adoutbound);
    /* prepare */
    if (zone->stats) {
        lock_basic_lock(&zone->stats->stats_lock);
        if (zone->stats->sort_done == 0 &&
            (zone->stats->sig_count <= zone->stats->sig_soa_count)) {
            ods_log_verbose("[%s] skip write zone %s serial %u (zone not "
                "changed)", tools_str, zone->name?zone->name:"(null)",
                zone->db->intserial);
            stats_clear(zone->stats);
            lock_basic_unlock(&zone->stats->stats_lock);
            zone->db->intserial =
                zone->db->outserial;
            return ODS_STATUS_OK;
        }
        lock_basic_unlock(&zone->stats->stats_lock);
    }
    /* Output Adapter */
    status = adapter_write((void*)zone);
    if (status != ODS_STATUS_OK) {
        ods_log_error("[%s] unable to write zone %s: adapter failed (%s)",
            tools_str, zone->name, ods_status2str(status));
        return status;
    }
    zone->db->outserial = zone->db->intserial;
    zone->db->is_initialized = 1;
    lock_basic_lock(&zone->ixfr->ixfr_lock);
    ixfr_purge(zone->ixfr);
    lock_basic_unlock(&zone->ixfr->ixfr_lock);
    /* kick the nameserver */
    if (zone->notify_ns) {
        int status;
        pid_t pid;
        ods_log_verbose("[%s] notify nameserver: %s", tools_str,
            zone->notify_ns);
	/** fork */
        switch ((pid = fork())) {
            case -1: /* error */
                ods_log_error("[%s] notify nameserver failed: unable to fork "
                    "(%s)", tools_str, strerror(errno));
                return ODS_STATUS_FORK_ERR;
            case 0: /* child */
                /** execv */
                execvp(zone->notify_ns, zone->notify_args);
                /** error */
                ods_log_error("[%s] notify nameserver failed: execv() failed "
                    "(%s)", tools_str, strerror(errno));
                exit(1);
                break;
            default: /* parent */
                ods_log_debug("[%s] notify nameserver process forked",
                    tools_str);
                /** wait for completion  */
                while (wait(&status) != pid) {
                    ;
                }
                break;
        }
    }
    if (engine->dnshandler) {
        dnshandler_fwd_notify(engine->dnshandler, (uint8_t*) ODS_SE_NOTIFY_CMD,
            strlen(ODS_SE_NOTIFY_CMD));
    }
    /* log stats */
    if (zone->stats) {
        lock_basic_lock(&zone->stats->stats_lock);
        zone->stats->end_time = time(NULL);
        ods_log_debug("[%s] log stats for zone %s", tools_str,
            zone->name?zone->name:"(null)");
        stats_log(zone->stats, zone->name, zone->signconf->nsec_type);
        stats_clear(zone->stats);
        lock_basic_unlock(&zone->stats->stats_lock);
    }
    return status;
}

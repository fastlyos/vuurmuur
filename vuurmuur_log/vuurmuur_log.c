/***************************************************************************
 *   Copyright (C) 2002-2019 by Victor Julien                              *
 *   victor@vuurmuur.org                                                   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "vuurmuur_log.h"
#include "nflog.h"
#include "stats.h"
#include "logfile.h"
#include "vuurmuur_ipc.h"
#include "conntrack.h"

#include <libnfnetlink/libnfnetlink.h>
#include <libnetfilter_log/libnetfilter_log.h>

char version_string[128];

/*@null@*/
struct vrmr_shm_table *shm_table = 0;
struct vrmr_hash_table zone_htbl;
struct vrmr_hash_table service_htbl;
static struct logcounters counters = {
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,

        0,
        0,
        0,
        0,
};
static FILE *g_traffic_log = NULL;
FILE *g_conn_new_log_fp = NULL;
FILE *g_connections_log_fp = NULL;

/*
    we put this here, because we only use it here in main.
*/
static int sigint_count = 0;
static int sighup_count = 0;
static int sigterm_count = 0;

static void handle_sigint(int sig ATTR_UNUSED)
{
    sigint_count = 1;
}

static void handle_sigterm(int sig ATTR_UNUSED)
{
    sigterm_count = 1;
}

static void handle_sighup(int sig ATTR_UNUSED)
{
    sighup_count = 1;
}

static void setup_signal_handler(int sig, void (*handler)(int))
{
    struct sigaction action;

    action.sa_handler = handler;
    sigemptyset(&(action.sa_mask));
    sigaddset(&(action.sa_mask), sig);
    action.sa_flags = 0;
    sigaction(sig, &action, 0);
}

static void print_help(void)
{
    fprintf(stdout, "Usage: vuurmuur_log [OPTIONS]\n");
    fprintf(stdout, "\n");
    fprintf(stdout, "Options:\n");
    fprintf(stdout, " -h, --help\t\t\tgives this help\n");
    fprintf(stdout, " -v, --verbose\t\t\tverbose mode\n");
    fprintf(stdout, " -n, --nodaemon\t\t\tdo NOT start as a daemon\n");
    fprintf(stdout, " -c, --configfile\t\tuse the given configfile\n");
    fprintf(stdout, " -d, --debug\t\t\tenable debugging (1 = low, 3 = high)\n");
    fprintf(stdout, " -K, --killme\t\t\tkill running daemon\n");
    fprintf(stdout, " -V, --version\t\t\tgives the version\n");
    fprintf(stdout, "\n");
    exit(EXIT_SUCCESS);
}

/* process one line/record */
int process_logrecord(struct vrmr_log_record *log_record)
{
    char line_out[1024] = "";

    int result =
            vrmr_log_record_get_names(log_record, &zone_htbl, &service_htbl);
    switch (result) {
        case -1:
            vrmr_debug(NONE, "vrmr_log_record_get_names returned -1");
            exit(EXIT_FAILURE);
            break;
        case 0:
            counters.invalid_loglines++;
            break;
        default:
            if (vrmr_log_record_build_line(
                        log_record, line_out, sizeof(line_out)) < 0) {
                vrmr_debug(NONE, "Could not build output line");
            } else {
                upd_action_ctrs(log_record->action, &counters);

                fprintf(g_traffic_log, "%s", line_out);
                fflush(g_traffic_log);
            }
            break;
    }

    return 0;
}

/** \internal
 *
 *  \brief open or reopen conntrack output logfiles
 */
static int conntrack_open_logs(struct vrmr_config *cnf)
{
    if (g_conn_new_log_fp != NULL)
        fclose(g_conn_new_log_fp);
    g_conn_new_log_fp = fopen(cnf->connnewlog_location, "a");
    if (g_conn_new_log_fp == NULL) {
        vrmr_error(-1, "Error", "fopen() %s failed: %s",
                cnf->connnewlog_location, strerror(errno));
        return (-1);
    }

    if (g_connections_log_fp != NULL)
        fclose(g_connections_log_fp);
    g_connections_log_fp = fopen(cnf->connlog_location, "a");
    if (g_connections_log_fp == NULL) {
        vrmr_error(-1, "Error", "fopen() %s failed: %s", cnf->connlog_location,
                strerror(errno));
        return (-1);
    }

    return (0);
}

int main(int argc, char *argv[])
{
    struct vrmr_ctx vctx;
    FILE *system_log = NULL;
    int result;
    pid_t pid;
    int optch;
    static char optstring[] = "hc:vnd:VsKN";
    int verbose = 0, nodaemon = 0;
    struct option prog_opts[] = {
            {"help", no_argument, NULL, 'h'},
            {"verbose", no_argument, &verbose, 1},
            {"nodaemon", no_argument, &nodaemon, 1},
            {"configfile", required_argument, NULL, 'c'},
            {"debug", required_argument, NULL, 'd'},
            {"killme", required_argument, NULL, 'K'},
            {"version", no_argument, NULL, 'V'},
            {0, 0, 0, 0},
    };
    int option_index = 0;
    char *sscanf_str = NULL;

    struct vrmr_log_record logrule;
    struct vrmr_log_record logconn;
    int debug_level = NONE;

    /* shm, sem stuff */
    int shm_id;
    int reload = 0;
    char quit = 0;

    snprintf(version_string, sizeof(version_string),
            "%s (using libvuurmuur %s)", VUURMUUR_VERSION,
            libvuurmuur_get_version());

    vrmr_init(&vctx, "vuurmuur_log");

    /* init signals */
    setup_signal_handler(SIGINT, handle_sigint);
    setup_signal_handler(SIGTERM, handle_sigterm);
    setup_signal_handler(SIGHUP, handle_sighup);

    /* process the options */
    while ((optch = getopt_long(
                    argc, argv, optstring, prog_opts, &option_index)) != -1) {
        switch (optch) {
            case 0:
                /* This is used for the flags long options */
                break;

            case 'h':
                print_help();
                break;

            case 'v':
                verbose = 1;
                break;

            case 'n':
                nodaemon = 1;
                break;

            case 'c':
                /* config file */
                if (vctx.conf.verbose_out == TRUE)
                    fprintf(stdout, "Using this configfile: %s\n", optarg);

                if (strlcpy(vctx.conf.configfile, optarg,
                            sizeof(vctx.conf.configfile)) >=
                        sizeof(vctx.conf.configfile)) {
                    fprintf(stderr,
                            "Error: configfile (-c): argument too long (max: "
                            "%d).\n",
                            (int)sizeof(vctx.conf.configfile) - 1);
                    exit(EXIT_FAILURE);
                }
                break;

            case 'd':
                /* debugging */
                fprintf(stdout, "vuurmuur: debugging enabled.\n");

                /* convert the debug string and check the result */
                debug_level = atoi(optarg);
                if (debug_level < 0 || debug_level > HIGH) {
                    fprintf(stdout,
                            "Error: illegal debug level: %d (max: %d).\n",
                            debug_level, HIGH);
                    exit(EXIT_FAILURE);
                }
                vrmr_debug_level = debug_level;

                fprintf(stdout, "vuurmuur-log: debug level: %d\n", debug_level);
                break;

            case 'K':
                if (vrmr_check_pidfile(PIDFILE, &pid) == -1) {
                    vrmr_debug(NONE, "Terminating %u", pid);
                    kill(pid, 15);
                    exit(EXIT_SUCCESS);
                }
                exit(EXIT_FAILURE);
                break;

            case 'V':
                /* print version */
                fprintf(stdout, "Vuurmuur_log %s\n", version_string);
                fprintf(stdout, "%s\n", VUURMUUR_COPYRIGHT);
                exit(EXIT_SUCCESS);
        }
    }

    /* check if the pidfile already exists */
    if (vrmr_check_pidfile(PIDFILE, &pid) == -1)
        exit(EXIT_FAILURE);

    /* init the config file */
    if (vrmr_init_config(&vctx.conf) < VRMR_CNF_OK) {
        vrmr_error(-1, "Error", "initializing the config failed.");
        exit(EXIT_FAILURE);
    }

    if (verbose)
        vrmr_info("Info", "Vuurmuur_log %s", version_string);

    /* now setup the print function */
    if (verbose)
        vrprint.error = vrmr_stdoutprint_error;
    else
        vrprint.error = vrmr_logprint_error;

    vrprint.warning = vrmr_logprint_warning;
    vrprint.info = vrmr_logprint_info;
    vrprint.debug = vrmr_logprint_debug;
    vrprint.audit = vrmr_logprint_audit;

    /* get the current user */
    vrmr_audit("Vuurmuur_log %s started by user %s.", version_string,
            vctx.user_data.realusername);

    /* Setup nflog after vrmr_init_config as and logging as we need &conf in
     * subscribe_nflog() */
    vrmr_debug(NONE, "Setting up nflog");
    if (subscribe_nflog(&vctx.conf, &logrule) < 0) {
        vrmr_error(-1, "Error", "could not set up nflog subscription");
        exit(EXIT_FAILURE);
    }
    if (conntrack_subscribe(&logconn) < 0) {
        vrmr_error(-1, "Error", "could not set up conntrack subscription");
        exit(EXIT_FAILURE);
    }
    if (conntrack_open_logs(&vctx.conf) != 0) {
        vrmr_error(-1, "Error", "could not open connection log files");
        exit(EXIT_FAILURE);
    }

    if (vrmr_backends_load(&vctx.conf, &vctx) < 0) {
        vrmr_error(-1, "Error", "loading plugins failed, bailing out.");
        exit(EXIT_FAILURE);
    }

    if (open_vuurmuurlog(&vctx.conf, &g_traffic_log) < 0) {
        vrmr_error(-1, "Error", "opening logfiles failed.");
        exit(EXIT_FAILURE);
    }

    /* load the services into memory */
    if (vrmr_services_load(&vctx, &vctx.services, &vctx.reg) == -1)
        exit(EXIT_FAILURE);

    /* load the interfaces into memory */
    if (vrmr_interfaces_load(&vctx, &vctx.interfaces) == -1)
        exit(EXIT_FAILURE);

    /* load the zonedata into memory */
    if (vrmr_zones_load(&vctx, &vctx.zones, &vctx.interfaces, &vctx.reg) == -1)
        exit(EXIT_FAILURE);

    /* insert the interfaces as VRMR_TYPE_FIREWALL's into the zonelist as
     * 'firewall', so this appears in to log as 'firewall(interface)' */
    if (vrmr_ins_iface_into_zonelist(&vctx.interfaces.list, &vctx.zones.list) <
            0) {
        vrmr_error(-1, "Error", "iface_into_zonelist failed");
        exit(EXIT_FAILURE);
    }

    /* these are removed by: vrmr_rem_iface_from_zonelist() (see below) */
    if (vrmr_add_broadcasts_zonelist(&vctx.zones) < 0) {
        vrmr_error(-1, "Error", "unable to add broadcasts to list.");
        exit(EXIT_FAILURE);
    }

    vrmr_info("Info", "Creating hash-table for the zones...");
    if (vrmr_init_zonedata_hashtable(vctx.zones.list.len * 3, &vctx.zones.list,
                vrmr_hash_ipaddress, vrmr_compare_ipaddress, &zone_htbl) < 0) {
        vrmr_error(-1, "Error", "vrmr_init_zonedata_hashtable failed.");
        exit(EXIT_FAILURE);
    }

    vrmr_info("Info", "Creating hash-table for the services...");
    if (vrmr_init_services_hashtable(vctx.services.list.len * 500,
                &vctx.services.list, vrmr_hash_port, vrmr_compare_ports,
                &service_htbl) < 0) {
        vrmr_error(-1, "Error", "vrmr_init_services_hashtable failed.");
        exit(EXIT_FAILURE);
    }

    if (nodaemon == 0) {
        if (daemon(1, 1) != 0) {
            vrmr_error(-1, "Error", "daemon() failed: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    if (ipc_setup(&shm_id, &shm_table) == -1)
        exit(EXIT_FAILURE);

    if (vrmr_create_pidfile(PIDFILE, shm_id) < 0)
        exit(EXIT_FAILURE);

    if (sigint_count || sigterm_count)
        quit = 1;

    /* enter the main loop */
    while (quit == 0) {
        reload = ipc_check_reload(shm_table);
        if (reload == 0) {
            switch (conntrack_read(&logconn)) {
                case 0:
                    break;
                case -1:
                    break;
            }
            switch (readnflog()) {
                case -1:
                    vrmr_error(-1, "Error", "could not read from nflog");
                    exit(EXIT_FAILURE);
                    break;
                case 0:
                    usleep(100000);
                    break;
            }
        }

        /*
            hey! we received a sighup. We will reload the data.
        */
        if (sighup_count || reload) {
            sighup_count = 0;

            /*
                clean up data
            */

            /* destroy hashtables */
            vrmr_hash_cleanup(&zone_htbl);
            vrmr_hash_cleanup(&service_htbl);

            /* destroy the ServicesList */
            vrmr_destroy_serviceslist(&vctx.services);
            /* destroy the ZonedataList */
            vrmr_destroy_zonedatalist(&vctx.zones);
            /* destroy the InterfacesList */
            vrmr_destroy_interfaceslist(&vctx.interfaces);

            /* close backend */
            result = vrmr_backends_unload(&vctx.conf, &vctx);
            if (result < 0) {
                vrmr_error(-1, "Error", "unloading backends failed.");
                exit(EXIT_FAILURE);
            }

            vrmr_shm_update_progress(sem_id, &shm_table->reload_progress, 10);

            /* reload the config

               if it fails it's no big deal, we just keep using the old config.
            */
            if (vrmr_reload_config(&vctx.conf) < VRMR_CNF_OK) {
                vrmr_warning("Warning",
                        "reloading config failed, using old config.");
            }

            vrmr_shm_update_progress(sem_id, &shm_table->reload_progress, 20);

            /* open backends */
            result = vrmr_backends_load(&vctx.conf, &vctx);
            if (result < 0) {
                vrmr_error(-1, "Error", "re-opening backends failed.");
                exit(EXIT_FAILURE);
            }

            vrmr_shm_update_progress(sem_id, &shm_table->reload_progress, 30);

            /* re-initialize the data */
            vrmr_info("Info", "Initializing interfaces...");
            if (vrmr_init_interfaces(&vctx, &vctx.interfaces) < 0) {
                vrmr_error(-1, "Error", "initializing interfaces failed.");
                exit(EXIT_FAILURE);
            }

            vrmr_shm_update_progress(sem_id, &shm_table->reload_progress, 40);

            vrmr_info("Info", "Initializing zones...");
            if (vrmr_init_zonedata(
                        &vctx, &vctx.zones, &vctx.interfaces, &vctx.reg) < 0) {
                vrmr_error(-1, "Error", "initializing zones failed.");
                exit(EXIT_FAILURE);
            }

            vrmr_shm_update_progress(sem_id, &shm_table->reload_progress, 50);

            vrmr_info("Info", "Initializing services...");
            if (vrmr_init_services(&vctx, &vctx.services, &vctx.reg) < 0) {
                vrmr_error(-1, "Error", "initializing services failed.");
                exit(EXIT_FAILURE);
            }

            vrmr_shm_update_progress(sem_id, &shm_table->reload_progress, 60);

            /* insert the interfaces as VRMR_TYPE_FIREWALL's into the zonelist
             * as 'firewall', so this appears in to log as 'firewall(interface)'
             */
            if (vrmr_ins_iface_into_zonelist(
                        &vctx.interfaces.list, &vctx.zones.list) < 0) {
                vrmr_error(-1, "Error", "iface_into_zonelist failed");
                exit(EXIT_FAILURE);
            }

            /* these are removed by: vrmr_rem_iface_from_zonelist() (see below)
             */
            if (vrmr_add_broadcasts_zonelist(&vctx.zones) < 0) {
                vrmr_error(-1, "Error", "unable to add broadcasts to list.");
                exit(EXIT_FAILURE);
            }
            vrmr_shm_update_progress(sem_id, &shm_table->reload_progress, 70);

            vrmr_info("Info", "Creating hash-table for the zones...");
            if (vrmr_init_zonedata_hashtable(vctx.zones.list.len * 3,
                        &vctx.zones.list, vrmr_hash_ipaddress,
                        vrmr_compare_ipaddress, &zone_htbl) < 0) {
                vrmr_error(result, "Error",
                        "vrmr_init_zonedata_hashtable failed.");
                exit(EXIT_FAILURE);
            }
            vrmr_shm_update_progress(sem_id, &shm_table->reload_progress, 80);

            vrmr_info("Info", "Creating hash-table for the services...");
            if (vrmr_init_services_hashtable(vctx.services.list.len * 500,
                        &vctx.services.list, vrmr_hash_port, vrmr_compare_ports,
                        &service_htbl) < 0) {
                vrmr_error(result, "Error",
                        "vrmr_init_services_hashtable failed.");
                exit(EXIT_FAILURE);
            }
            vrmr_shm_update_progress(sem_id, &shm_table->reload_progress, 90);

            if (reopen_vuurmuurlog(&vctx.conf, &g_traffic_log) < 0) {
                vrmr_error(-1, "Error", "re-opening logfiles failed.");
                exit(EXIT_FAILURE);
            }
            vrmr_shm_update_progress(sem_id, &shm_table->reload_progress, 92);
            if (conntrack_open_logs(&vctx.conf) != 0) {
                vrmr_error(
                        -1, "Error", "could not re-open connection log files");
                exit(EXIT_FAILURE);
            }
            vrmr_shm_update_progress(sem_id, &shm_table->reload_progress, 95);

            /* only ok now */
            result = 0;

            /* if we are reloading because of an IPC command, we need to
             * communicate with the caller */
            if (reload == 1)
                ipc_sync(30, &result, shm_table, &reload);
        }

        /* check for a signal */
        if (sigint_count || sigterm_count)
            quit = 1;
    }

    /*
        cleanup
    */
    if (ipc_destroy(shm_id) == -1) {
        vrmr_error(-1, "Error", "Detach from VM IPC failed.");
        /* fall through */
    }

    /* free the sscanf parser string */
    free(sscanf_str);

    /* close the logfiles */
    if (g_traffic_log != NULL)
        fclose(g_traffic_log);
    if (g_connections_log_fp != NULL)
        fclose(g_connections_log_fp);
    if (g_conn_new_log_fp != NULL)
        fclose(g_conn_new_log_fp);
    if (system_log != NULL)
        fclose(system_log);

    conntrack_disconnect();

    /* destroy hashtables */
    vrmr_hash_cleanup(&zone_htbl);
    vrmr_hash_cleanup(&service_htbl);

    /* destroy the ServicesList */
    vrmr_destroy_serviceslist(&vctx.services);
    /* destroy the ZonedataList */
    vrmr_destroy_zonedatalist(&vctx.zones);
    /* destroy the InterfacesList */
    vrmr_destroy_interfaceslist(&vctx.interfaces);

    if (nodaemon)
        show_stats(&counters);

    if (vrmr_backends_unload(&vctx.conf, &vctx) < 0) {
        vrmr_error(-1, "Error", "unloading backends failed.");
    }

    /* remove the pidfile */
    if (vrmr_remove_pidfile(PIDFILE) < 0) {
        vrmr_error(
                -1, "Error", "unable to remove pidfile: %s.", strerror(errno));
    }

    exit(EXIT_SUCCESS);
}

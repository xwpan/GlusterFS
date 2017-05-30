# -*- coding: utf-8 -*-
#
#  Copyright (c) 2016 Red Hat, Inc. <http://www.redhat.com>
#  This file is part of GlusterFS.
#
#  This file is licensed to you under your choice of the GNU Lesser
#  General Public License, version 3 or any later version (LGPLv3 or
#  later), or the GNU General Public License, version 2 (GPLv2), in all
#  cases as published by the Free Software Foundation.
#

SERVER_ADDRESS = "0.0.0.0"
DEFAULT_CONFIG_FILE = "/usr/local/etc/glusterfs/eventsconfig.json"
CUSTOM_CONFIG_FILE_TO_SYNC = "/events/config.json"
CUSTOM_CONFIG_FILE = "/var/lib/glusterd" + CUSTOM_CONFIG_FILE_TO_SYNC
WEBHOOKS_FILE_TO_SYNC = "/events/webhooks.json"
WEBHOOKS_FILE = "/var/lib/glusterd" + WEBHOOKS_FILE_TO_SYNC
LOG_FILE = "/var/log/glusterfs/events.log"
EVENTSD = "glustereventsd"
CONFIG_KEYS = ["log_level", "port"]
BOOL_CONFIGS = []
INT_CONFIGS = ["port"]
RESTART_CONFIGS = ["port"]
EVENTS_ENABLED = 0
UUID_FILE = "/var/lib/glusterd/glusterd.info"
PID_FILE = "/var/run/glustereventsd.pid"
AUTO_BOOL_ATTRIBUTES = ["force", "push-pem", "no-verify"]
AUTO_INT_ATTRIBUTES = ["ssh-port"]

# Errors
ERROR_SAME_CONFIG = 2
ERROR_ALL_NODES_STATUS_NOT_OK = 3
ERROR_PARTIAL_SUCCESS = 4
ERROR_WEBHOOK_ALREADY_EXISTS = 5
ERROR_WEBHOOK_NOT_EXISTS = 6
ERROR_INVALID_CONFIG = 7
ERROR_WEBHOOK_SYNC_FAILED = 8
ERROR_CONFIG_SYNC_FAILED = 9

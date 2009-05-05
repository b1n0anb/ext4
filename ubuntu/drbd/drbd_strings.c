/*
  drbd.h
  Kernel module for 2.6.x Kernels

  This file is part of DRBD by Philipp Reisner and Lars Ellenberg.

  Copyright (C) 2003-2008, LINBIT Information Technologies GmbH.
  Copyright (C) 2003-2008, Philipp Reisner <philipp.reisner@linbit.com>.
  Copyright (C) 2003-2008, Lars Ellenberg <lars.ellenberg@linbit.com>.

  drbd is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2, or (at your option)
  any later version.

  drbd is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with drbd; see the file COPYING.  If not, write to
  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include <linux/drbd.h>

static const char *drbd_conn_s_names[] = {
	[StandAlone]     = "StandAlone",
	[Disconnecting]  = "Disconnecting",
	[Unconnected]    = "Unconnected",
	[Timeout]        = "Timeout",
	[BrokenPipe]     = "BrokenPipe",
	[NetworkFailure] = "NetworkFailure",
	[ProtocolError]  = "ProtocolError",
	[WFConnection]   = "WFConnection",
	[WFReportParams] = "WFReportParams",
	[TearDown]       = "TearDown",
	[Connected]      = "Connected",
	[StartingSyncS]  = "StartingSyncS",
	[StartingSyncT]  = "StartingSyncT",
	[WFBitMapS]      = "WFBitMapS",
	[WFBitMapT]      = "WFBitMapT",
	[WFSyncUUID]     = "WFSyncUUID",
	[SyncSource]     = "SyncSource",
	[SyncTarget]     = "SyncTarget",
	[VerifyS]        = "VerifyS",
	[VerifyT]        = "VerifyT",
	[PausedSyncS]    = "PausedSyncS",
	[PausedSyncT]    = "PausedSyncT"
};

static const char *drbd_role_s_names[] = {
	[Primary]   = "Primary",
	[Secondary] = "Secondary",
	[Unknown]   = "Unknown"
};

static const char *drbd_disk_s_names[] = {
	[Diskless]     = "Diskless",
	[Attaching]    = "Attaching",
	[Failed]       = "Failed",
	[Negotiating]  = "Negotiating",
	[Inconsistent] = "Inconsistent",
	[Outdated]     = "Outdated",
	[DUnknown]     = "DUnknown",
	[Consistent]   = "Consistent",
	[UpToDate]     = "UpToDate",
};

static const char *drbd_state_sw_errors[] = {
	[-SS_TwoPrimaries] = "Multiple primaries not allowed by config",
	[-SS_NoUpToDateDisk] =
		"Refusing to be Primary without at least one UpToDate disk",
	[-SS_BothInconsistent] = "Refusing to be inconsistent on both nodes",
	[-SS_SyncingDiskless] = "Refusing to be syncing and diskless",
	[-SS_ConnectedOutdates] = "Refusing to be Outdated while Connected",
	[-SS_PrimaryNOP] = "Refusing to be Primary while peer is not outdated",
	[-SS_ResyncRunning] = "Can not start OV/resync since it is already active",
	[-SS_AlreadyStandAlone] = "Can not disconnect a StandAlone device",
	[-SS_CW_FailedByPeer] = "State changed was refused by peer node",
	[-SS_IsDiskLess] =
		"Device is diskless, the requesed operation requires a disk",
	[-SS_DeviceInUse] = "Device is held open by someone",
	[-SS_NoNetConfig] = "Have no net/connection configuration",
	[-SS_NoVerifyAlg] = "Need a verify algorithm to start online verify",
	[-SS_NeedConnection] = "Need a connection to start verify or resync",
	[-SS_NotSupported] = "Peer does not support protocol",
	[-SS_LowerThanOutdated] = "Disk state is lower than outdated",
	[-SS_InTransientState] = "In transient state, retry after next state change",
	[-SS_ConcurrentStChg] = "Concurrent state changes detected and aborted",
};

const char *conns_to_name(enum drbd_conns s)
{
	/* enums are unsigned... */
	return s > PausedSyncT ? "TOO_LARGE" : drbd_conn_s_names[s];
}

const char *roles_to_name(enum drbd_role s)
{
	return s > Secondary   ? "TOO_LARGE" : drbd_role_s_names[s];
}

const char *disks_to_name(enum drbd_disk_state s)
{
	return s > UpToDate    ? "TOO_LARGE" : drbd_disk_s_names[s];
}

const char *set_st_err_name(enum set_st_err err)
{
	return err < SS_NotSupported ? "TOO_SMALL" :
	       err > SS_TwoPrimaries ? "TOO_LARGE"
			: drbd_state_sw_errors[-err];
}

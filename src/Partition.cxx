/*
 * Copyright 2003-2022 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "Partition.hxx"
#include "Instance.hxx"
#include "Log.hxx"
#include "config/PartitionConfig.hxx"
#include "lib/fmt/ExceptionFormatter.hxx"
#include "song/DetachedSong.hxx"
#include "IdleFlags.hxx"
#include "client/Listener.hxx"
#include "client/Client.hxx"
#include "input/cache/Manager.hxx"
#include "util/Domain.hxx"

static constexpr Domain cache_domain("cache");

Partition::Partition(Instance &_instance,
		     const char *_name,
		     const PartitionConfig &_config) noexcept
	:instance(_instance),
	 name(_name),
	 config(_config),
	 listener(new ClientListener(instance.event_loop, *this)),
	 idle_monitor(instance.event_loop, BIND_THIS_METHOD(OnIdleMonitor)),
	 global_events(instance.event_loop, BIND_THIS_METHOD(OnGlobalEvent)),
	 playlist(config.queue.max_length, *this),
	 outputs(pc, *this),
	 pc(*this, outputs,
	    instance.input_cache.get(),
	    config.player)
{
	UpdateEffectiveReplayGainMode();
}

Partition::~Partition() noexcept = default;

void
Partition::BeginShutdown() noexcept
{
	pc.Kill();
	listener.reset();
}

static void
PrefetchSong(InputCacheManager &cache, const char *uri) noexcept
{
	if (cache.Contains(uri))
		return;

	FmtDebug(cache_domain, "Prefetch '{}'", uri);

	try {
		cache.Prefetch(uri);
	} catch (...) {
		FmtError(cache_domain,
			 "Prefetch '{}' failed: {}",
			 uri, std::current_exception());
	}
}

static void
PrefetchSong(InputCacheManager &cache, const DetachedSong &song) noexcept
{
	PrefetchSong(cache, song.GetURI());
}

inline void
Partition::PrefetchQueue() noexcept
{
	if (!instance.input_cache)
		return;

	auto &cache = *instance.input_cache;

	int next = playlist.GetNextPosition();
	if (next >= 0)
		PrefetchSong(cache, playlist.queue.Get(next));

	// TODO: prefetch more songs
}

void
Partition::UpdateEffectiveReplayGainMode() noexcept
{
	auto mode = replay_gain_mode;
	if (mode == ReplayGainMode::AUTO)
	    mode = playlist.queue.random
		    ? ReplayGainMode::TRACK
		    : ReplayGainMode::ALBUM;

	pc.LockSetReplayGainMode(mode);

	outputs.SetReplayGainMode(mode);
}

#ifdef ENABLE_DATABASE

const Database *
Partition::GetDatabase() const noexcept
{
	return instance.GetDatabase();
}

const Database &
Partition::GetDatabaseOrThrow() const
{
	return instance.GetDatabaseOrThrow();
}

void
Partition::DatabaseModified(const Database &db) noexcept
{
	playlist.DatabaseModified(db);
	EmitIdle(IDLE_DATABASE);
}

#endif

void
Partition::TagModified() noexcept
{
	auto song = pc.LockReadTaggedSong();
	if (song)
		playlist.TagModified(std::move(*song));
}

void
Partition::TagModified(const char *uri, const Tag &tag) noexcept
{
	playlist.TagModified(uri, tag);
}

void
Partition::SyncWithPlayer() noexcept
{
	playlist.SyncWithPlayer(pc);

	/* TODO: invoke this function in batches, to let the hard disk
	   spin down in between */
	PrefetchQueue();
}

void
Partition::BorderPause() noexcept
{
	playlist.BorderPause(pc);
}

void
Partition::OnQueueModified() noexcept
{
	EmitIdle(IDLE_PLAYLIST);
}

void
Partition::OnQueueOptionsChanged() noexcept
{
	EmitIdle(IDLE_OPTIONS);
}

void
Partition::OnQueueSongStarted() noexcept
{
	EmitIdle(IDLE_PLAYER);
}

void
Partition::OnPlayerError() noexcept
{
	EmitIdle(IDLE_PLAYER);
}

void
Partition::OnPlayerStateChanged() noexcept
{
	EmitIdle(IDLE_PLAYER);
}

void
Partition::OnPlayerOptionsChanged() noexcept
{
	EmitIdle(IDLE_OPTIONS);
}

void
Partition::OnPlayerSync() noexcept
{
	EmitGlobalEvent(SYNC_WITH_PLAYER);
}

void
Partition::OnPlayerTagModified() noexcept
{
	EmitGlobalEvent(TAG_MODIFIED);

	/* notify all clients that the tag of the current song has
	   changed */
	EmitIdle(IDLE_PLAYER);
}

void
Partition::OnBorderPause() noexcept
{
	EmitGlobalEvent(BORDER_PAUSE);
}

void
Partition::OnMixerVolumeChanged(Mixer &, int) noexcept
{
	mixer_memento.InvalidateHardwareVolume();

	/* notify clients */
	EmitIdle(IDLE_MIXER);
}

void
Partition::OnMixerChanged() noexcept
{
	/* notify clients */
	EmitIdle(IDLE_MIXER);
}

void
Partition::OnIdleMonitor(unsigned mask) noexcept
{
	/* send "idle" notifications to all subscribed
	   clients */
	for (auto &client : clients)
		client.IdleAdd(mask);

	if (mask & (IDLE_PLAYLIST|IDLE_PLAYER|IDLE_MIXER|IDLE_OUTPUT))
		instance.OnStateModified();
}

void
Partition::OnGlobalEvent(unsigned mask) noexcept
{
	if ((mask & SYNC_WITH_PLAYER) != 0)
		SyncWithPlayer();

	if ((mask & TAG_MODIFIED) != 0)
		TagModified();

	if ((mask & BORDER_PAUSE) != 0)
		BorderPause();
}

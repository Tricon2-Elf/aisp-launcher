// notify_nicolive_reload (0xE342) reaches every client on a map when /screen or /channel changes
// what its screens show, but the client itself only acts on it on the Stage (its Nico Live
// billboard re-navigates); a town map's own screens (channel-screen pages outside a room) have
// nothing that listens. This patch adds that: when the client has taken the packet, the hook
// reloads the primary browser of every such screen (only those on one channel number when the
// packet's live id is lv0..lv99; lv100 means all; lv200 all, with what each plays stopped
// first), and the client's handling goes on as before.
// The patch site is verified against the expected bytes first; see PatchNicoliveReloadNotify.
#pragma once

namespace aisp
{
void PatchNicoliveReloadNotify();
} // namespace aisp

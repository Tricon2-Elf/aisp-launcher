// The room TV panel's comment button, filled in by patching the client at load: its click
// case, its second image, the state feed that drives it and its pressed frames. Everything it
// touches is verified against the expected bytes first; see the comment on PatchTvCommentButton.
#pragma once

namespace aisp
{
void PatchTvCommentButton();
} // namespace aisp

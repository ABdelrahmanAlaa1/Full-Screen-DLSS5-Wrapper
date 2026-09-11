#pragma once
#include "effects/real/executor.h"
#include "interior/capture_name.h"

namespace real {

// What a screenshot is asked for with: where to put it, what to call it, and what shaped the picture.
struct SnapshotOrder
{
    interior::DirectoryPath folder;
    interior::CaptureLabel label;
    interior::LiveSettings live;
    bool everything; // every parameter goes into the name, the ones at their defaults included
};

// The two files a screenshot became, and the fence the copies out of the GPU were waited for at.
struct Snapshot
{
    interior::FilePath original;
    interior::FilePath processed;
    interior::FenceValue fence;
};

// Copies the picture the model was given and the picture shown for it out of the GPU, as the frame just
// submitted left them, and writes both as PNG files named for the settings and the moment. Waits for the
// copies and for the files, so the frame loop stands still for as long as that takes.
[[nodiscard]] infra::Result<Snapshot, Error> SaveSnapshot(const Gpu& gpu, const FrameContext& frame, const interior::FrameState& after, const SnapshotOrder& order) noexcept;

} // namespace real
